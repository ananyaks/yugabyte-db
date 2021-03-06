/*--------------------------------------------------------------------------------------------------
 *
 * ybccmds.c
 *        YB commands for creating and altering table structures and settings
 *
 * Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.  You may obtain a copy of the License at
 *
 * http: *www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied.  See the License for the specific language governing permissions and limitations
 * under the License.
 *
 * IDENTIFICATION
 *        src/backend/commands/ybccmds.c
 *
 *--------------------------------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"
#include "catalog/pg_class.h"
#include "commands/dbcommands.h"
#include "commands/ybccmds.h"
#include "commands/ybctype.h"

#include "yb/yql/pggate/ybc_pggate.h"
#include "pg_yb_utils.h"

/* ---------------------------------------------------------------------------------------------- */
/*  Database Functions. */

void
YBCCreateDatabase(Oid dboid, const char *dbname, Oid src_dboid)
{
	YBCPgStatement handle;

	char	   *src_dbname = get_database_name(src_dboid);

	YBCLogWarning("Ignoring source database '%s' when creating database '%s'", src_dbname, dbname);
	PG_TRY();
	{
		HandleYBStatus(YBCPgAllocCreateDatabase(ybc_pg_session, dbname, &handle));
		HandleYBStatus(YBCPgExecCreateDatabase(handle));
	}
	PG_CATCH();
	{
		HandleYBStatus(YBCPgDeleteStatement(handle));
		PG_RE_THROW();
	}
	PG_END_TRY();
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCDropDatabase(Oid dboid, const char *dbname)
{
	YBCPgStatement handle;

	PG_TRY();
	{
		HandleYBStatus(YBCPgAllocDropDatabase(ybc_pg_session,
											  dbname,
											  false,	/* if_exists */
											  &handle));
		HandleYBStatus(YBCPgExecDropDatabase(handle));
	}
	PG_CATCH();
	{
		HandleYBStatus(YBCPgDeleteStatement(handle));
		PG_RE_THROW();
	}
	PG_END_TRY();
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCAlterDatabase(Oid dboid, const char *dbname)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("'ALTER DATABASE' is not supported yet.")));
}

/* -------------------------------------------------------------------- */
/*  Table Functions. */

void
YBCCreateTable(CreateStmt *stmt, char relkind, Oid relationId)
{
	YBCPgStatement handle;
	ListCell   *listptr;
	int			attnum;

	/*
	 * TODO The attnum from pg_attribute has not been created yet, we are
	 * making some assumptions below about how it will be assigned.
	 */

	/* ------------------------------------------------------------------- */
	/* Error checking */

	if (stmt->partspec != NULL)
	{
		/*
		 * Tables with this clause cannot have a primary key so we can maybe
		 * just use this as a hash key internally.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("'PARTITION BY' clause is not yet supported")));
	}

	if (relkind != RELKIND_RELATION)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("Relation kind '%c' not yet supported", relkind)));
	}

	if (stmt->ofTypename != NULL)
	{
		/*
		 * We need to look up the type to get the schema for the referenced
		 * type.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("'OF <type>' clause is not yet supported")));
	}

	if (stmt->inhRelations != NIL)
	{
		/*
		 * We need to look up the referenced type (relation) to get its
		 * schema. Additionally we need to share (interleave?) the data for
		 * the common columns.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("'INHERITS' clause is not yet supported")));
	}

	if (stmt->partbound != NULL)
	{
		/*
		 * Manual partition by giving explicit key ranges -- maybe not
		 * something we want at all.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("'PARTITION OF' clause is not supported yet.")));
	}

	if (stmt->relation->relpersistence == RELPERSISTENCE_TEMP)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("'TEMP' tables are not supported yet.")));
	}

	/*
	 * Features that are not really applicable to YB (e.g. storage-specific
	 * config).
	 */
	if (stmt->oncommit != ONCOMMIT_NOOP &&
		stmt->oncommit != ONCOMMIT_PRESERVE_ROWS)
	{
		YBCLogWarning("Given 'ON COMMIT' option is not supported, will be ignored.");
	}

	if (stmt->tablespacename != NULL)
	{
		YBCLogWarning("'TABLESPACE' option is not supported, will be ignored");
	}

	if (stmt->options != NIL)
	{
		/*
		 * With OID we could probably support but it's not recommended for
		 * non-system tables anyway.
		 */
		YBCLogWarning("'WITH' clause options are not supported yet, they will be ignored");
	}

	char	   *db_name = get_database_name(MyDatabaseId);

	YBCLogInfo("Creating Table %s, %s, %s", db_name, stmt->relation->schemaname,
			   stmt->relation->relname);

	PG_TRY();
	{
		HandleYBStatus(YBCPgAllocCreateTable(ybc_pg_session,
											 db_name,
											 stmt->relation->schemaname,
											 stmt->relation->relname,
											 false, /* if_not_exists */
											 &handle));

		Constraint *primary_key;

		foreach(listptr, stmt->constraints)
		{
			Constraint *constraint = lfirst(listptr);

			if (constraint->contype == CONSTR_PRIMARY)
			{
				primary_key = constraint;
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("Only PRIMARY KEY constraints are currently supported.")));
			}
		}

		if (primary_key == NULL)
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("Tables without a PRIMARY KEY are not yet supported.")));
		}

		/* Process the table columns. */
		attnum = 1;
		foreach(listptr, stmt->tableElts)
		{
			bool		is_primary = false;
			bool		is_first = true;
			ColumnDef  *colDef = lfirst(listptr);
			YBCPgDataType col_type = YBCDataTypeFromName(colDef->typeName);

			ListCell   *cell;

			foreach(cell, primary_key->keys)
			{
				char	   *attname = strVal(lfirst(cell));

				if (strcmp(colDef->colname, attname) == 0)
				{
					is_primary = true;
					break;
				}
				if (is_first)
				{
					is_first = false;
				}
			}

			/* TODO For now, assume the first primary key column is the hash. */
			HandleYBStatus(YBCPgCreateTableAddColumn(handle,
													 colDef->colname,
													 attnum++,
													 col_type,
													 is_primary && is_first,	/* is_hash */
													 is_primary && !is_first /* is_range */ ));
		}

		/* Create the table. */
		HandleYBStatus(YBCPgExecCreateTable(handle));
	}
	PG_CATCH();
	{
		HandleYBStatus(YBCPgDeleteStatement(handle));
		PG_RE_THROW();
	}
	PG_END_TRY();
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCDropTable(Oid relationId,
			 const char *relname,
			 const char *schemaname)
{
	YBCPgStatement handle;

	PG_TRY();
	{
		char	   *db_name = get_database_name(MyDatabaseId);

		HandleYBStatus(YBCPgAllocDropTable(ybc_pg_session,
										   db_name,
										   schemaname,
										   relname,
										   false,	/* if_exists */
										   &handle));
		HandleYBStatus(YBCPgExecDropTable(handle));
	}
	PG_CATCH();
	{
		HandleYBStatus(YBCPgDeleteStatement(handle));
		PG_RE_THROW();
	}
	PG_END_TRY();
	/* TODO Maybe rethink this to avoid the boilerplate of having to */
	/* write the cleanup code twice (in catch and here). */
	HandleYBStatus(YBCPgDeleteStatement(handle));
}

void
YBCAlterTable(Oid dboid)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("'ALTER TABLE' is not yet supported")));
}
