//--------------------------------------------------------------------------------------------------
// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
//
// QLEnv represents the environment where SQL statements are being processed.
//--------------------------------------------------------------------------------------------------

#include "yb/yql/cql/ql/util/ql_env.h"
#include "yb/client/client.h"
#include "yb/client/transaction.h"

#include "yb/master/catalog_manager.h"

namespace yb {
namespace ql {

using std::string;
using std::shared_ptr;
using std::weak_ptr;

using client::TransactionManager;
using client::YBClient;
using client::YBSession;
using client::YBSessionPtr;
using client::YBTable;
using client::YBTransaction;
using client::YBTransactionPtr;
using client::YBMetaDataCache;
using client::YBTableCreator;
using client::YBTableAlterer;
using client::YBTableName;

QLEnv::QLEnv(shared_ptr<YBClient> client,
             shared_ptr<YBMetaDataCache> cache,
             const server::ClockPtr& clock,
             TransactionManagerProvider transaction_manager_provider)
    : client_(std::move(client)),
      metadata_cache_(std::move(cache)),
      clock_(std::move(clock)),
      transaction_manager_provider_(std::move(transaction_manager_provider)) {
}

QLEnv::~QLEnv() {}

//------------------------------------------------------------------------------------------------
YBTableCreator *QLEnv::NewTableCreator() {
  return client_->NewTableCreator();
}

YBTableAlterer *QLEnv::NewTableAlterer(const YBTableName& table_name) {
  return client_->NewTableAlterer(table_name);
}

CHECKED_STATUS QLEnv::TruncateTable(const string& table_id) {
  return client_->TruncateTable(table_id);
}

CHECKED_STATUS QLEnv::DeleteTable(const YBTableName& name) {
  return client_->DeleteTable(name);
}

CHECKED_STATUS QLEnv::DeleteIndexTable(const YBTableName& name, YBTableName* indexed_table_name) {
  return client_->DeleteIndexTable(name, indexed_table_name);
}

//------------------------------------------------------------------------------------------------
YBTransactionPtr QLEnv::NewTransaction(const YBTransactionPtr& transaction,
                                       const IsolationLevel isolation_level) {
  if (transaction) {
    DCHECK(transaction->IsRestartRequired());
    return transaction->CreateRestartedTransaction();
  }
  if (transaction_manager_ == nullptr) {
    transaction_manager_ = transaction_manager_provider_();
  }
  return std::make_shared<YBTransaction>(transaction_manager_, isolation_level);
}

YBSessionPtr QLEnv::NewSession() {
  return std::make_shared<YBSession>(client_, clock_);
}

//------------------------------------------------------------------------------------------------
shared_ptr<YBTable> QLEnv::GetTableDesc(const YBTableName& table_name, bool* cache_used) {
  // Hide tables in system_redis keyspace.
  if (table_name.is_redis_namespace()) {
    return nullptr;
  }
  shared_ptr<YBTable> yb_table;
  Status s = metadata_cache_->GetTable(table_name, &yb_table, cache_used);

  if (!s.ok()) {
    VLOG(3) << "GetTableDesc: Server returns an error: " << s.ToString();
    return nullptr;
  }

  return yb_table;
}

shared_ptr<YBTable> QLEnv::GetTableDesc(const TableId& table_id, bool* cache_used) {
  shared_ptr<YBTable> yb_table;
  Status s = metadata_cache_->GetTable(table_id, &yb_table, cache_used);

  if (!s.ok()) {
    VLOG(3) << "GetTableDesc: Server returns an error: " << s.ToString();
    return nullptr;
  }

  return yb_table;
}

shared_ptr<QLType> QLEnv::GetUDType(const std::string &keyspace_name,
                                      const std::string &type_name,
                                      bool *cache_used) {
  shared_ptr<QLType> ql_type = std::make_shared<QLType>(keyspace_name, type_name);
  Status s = metadata_cache_->GetUDType(keyspace_name, type_name, &ql_type, cache_used);

  if (!s.ok()) {
    VLOG(3) << "GetTypeDesc: Server returned an error: " << s.ToString();
    return nullptr;
  }

  return ql_type;
}

void QLEnv::RemoveCachedTableDesc(const YBTableName& table_name) {
  metadata_cache_->RemoveCachedTable(table_name);
}

void QLEnv::RemoveCachedTableDesc(const TableId& table_id) {
  metadata_cache_->RemoveCachedTable(table_id);
}

void QLEnv::RemoveCachedUDType(const std::string& keyspace_name, const std::string& type_name) {
  metadata_cache_->RemoveCachedUDType(keyspace_name, type_name);
}

//------------------------------------------------------------------------------------------------
Status QLEnv::GrantPermission(const PermissionType& permission, const ResourceType& resource_type,
                              const std::string& canonical_resource, const char* resource_name,
                              const char* namespace_name, const std::string& role_name) {
  return client_->GrantPermission(permission, resource_type, canonical_resource, resource_name,
                                  namespace_name, role_name);
}

//------------------------------------------------------------------------------------------------
Status QLEnv::CreateKeyspace(const std::string& keyspace_name) {
  return client_->CreateNamespace(keyspace_name);
}

Status QLEnv::DeleteKeyspace(const string& keyspace_name) {
  RETURN_NOT_OK(client_->DeleteNamespace(keyspace_name));

  // Reset the current keyspace name if it's dropped.
  if (ql_session()->current_keyspace() == keyspace_name) {
    ql_session()->set_current_keyspace(kUndefinedKeyspace);
  }
  return Status::OK();
}

Status QLEnv::UseKeyspace(const string& keyspace_name) {
  // Check if a keyspace with the specified name exists.
  Result<bool> exists = client_->NamespaceExists(keyspace_name);
  RETURN_NOT_OK(exists);
  if (!exists.get()) {
    return STATUS(NotFound, "Cannot use unknown keyspace");
  }

  ql_session()->set_current_keyspace(keyspace_name);
  return Status::OK();
}

//------------------------------------------------------------------------------------------------
Status QLEnv::CreateRole(const std::string& role_name,
                         const std::string& salted_hash,
                         const bool login, const bool superuser) {
  return client_->CreateRole(role_name, salted_hash, login, superuser);
}

Status QLEnv::AlterRole(const std::string& role_name,
                        const boost::optional<std::string>& salted_hash,
                        const boost::optional<bool> login,
                        const boost::optional<bool> superuser) {
  return client_->AlterRole(role_name, salted_hash, login, superuser);
}

Status QLEnv::DeleteRole(const std::string& role_name) {
  return client_->DeleteRole(role_name);
}

Status QLEnv::GrantRole(const std::string& granted_role_name,
                        const std::string& recipient_role_name) {
  return client_->GrantRole(granted_role_name, recipient_role_name);
}

//------------------------------------------------------------------------------------------------
Status QLEnv::CreateUDType(const std::string &keyspace_name,
                           const std::string &type_name,
                           const std::vector<std::string> &field_names,
                           const std::vector<std::shared_ptr<QLType>> &field_types) {
  return client_->CreateUDType(keyspace_name, type_name, field_names, field_types);
}

Status QLEnv::DeleteUDType(const std::string &keyspace_name, const std::string &type_name) {
  return client_->DeleteUDType(keyspace_name, type_name);
}

}  // namespace ql
}  // namespace yb
