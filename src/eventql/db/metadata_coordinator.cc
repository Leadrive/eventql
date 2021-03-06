/**
 * Copyright (c) 2016 DeepCortex GmbH <legal@eventql.io>
 * Authors:
 *   - Paul Asmuth <paul@eventql.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include "eventql/db/metadata_coordinator.h"
#include <eventql/util/logging.h>

namespace eventql {

MetadataCoordinator::MetadataCoordinator(ConfigDirectory* cdir) : cdir_(cdir) {}

Status MetadataCoordinator::performAndCommitOperation(
    const String& ns,
    const String& table_name,
    MetadataOperation op) {
  auto table_config = cdir_->getTableConfig(ns, table_name);
  SHA1Hash input_txid(
      table_config.metadata_txnid().data(),
      table_config.metadata_txnid().size());

  if (!(input_txid == op.getInputTransactionID())) {
    return Status(eConcurrentModificationError, "concurrent modification");
  }

  Vector<String> servers;
  for (const auto& s : table_config.metadata_servers()) {
    servers.emplace_back(s);
  }

  auto rc = performOperation(ns, table_name, op, servers);
  if (!rc.isSuccess()) {
    return rc;
  }

  auto output_txid = op.getOutputTransactionID();
  table_config.set_metadata_txnid(output_txid.data(), output_txid.size());
  table_config.set_metadata_txnseq(table_config.metadata_txnseq() + 1);
  cdir_->updateTableConfig(table_config);
  return Status::success();
}

Status MetadataCoordinator::performOperation(
    const String& ns,
    const String& table_name,
    MetadataOperation op,
    const Vector<String>& servers) {
  size_t num_servers = servers.size();
  if (num_servers == 0) {
    return Status(eIllegalArgumentError, "server list can't be empty");
  }

  size_t failures = 0;
  Set<SHA1Hash> metadata_file_checksums;
  for (const auto& s : servers) {
    MetadataOperationResult result;
    auto rc = performOperation(ns, table_name, op, s, &result);
    if (rc.isSuccess()) {
      metadata_file_checksums.emplace(
          SHA1Hash(
              result.metadata_file_checksum().data(),
              result.metadata_file_checksum().size()));
    } else {
      logDebug(
          "evqld",
          "error while performing metadata operation: $0",
          rc.message());
      ++failures;
    }
  }

  if (metadata_file_checksums.size() > 1) {
    return Status(eRuntimeError, "metadata operation would corrupt file");
  }

  size_t max_failures = 0;
  if (num_servers > 1) {
    max_failures = (num_servers - 1) / 2;
  }

  if (failures <= max_failures) {
    return Status::success();
  } else {
    return Status(eRuntimeError, "error while performing metadata operation");
  }
}

Status MetadataCoordinator::performOperation(
    const String& ns,
    const String& table_name,
    MetadataOperation op,
    const String& server,
    MetadataOperationResult* result) {
  auto server_cfg = cdir_->getServerConfig(server);
  if (server_cfg.server_addr().empty()) {
    return Status(eRuntimeError, "server is offline");
  }

  logDebug(
      "evqld",
      "Performing metadata operation on: $0/$1 ($2->$3) on $4 ($5)",
      ns,
      table_name,
      op.getInputTransactionID().toString(),
      op.getOutputTransactionID().toString(),
      server,
      server_cfg.server_addr());

  auto url = StringUtil::format(
      "http://$0/rpc/perform_metadata_operation?namespace=$1&table=$2",
      server_cfg.server_addr(),
      URI::urlEncode(ns),
      URI::urlEncode(table_name));

  Buffer req_body;
  {
    auto os = BufferOutputStream::fromBuffer(&req_body);
    auto rc = op.encode(os.get());
    if (!rc.isSuccess()) {
      return rc;
    }
  }

  auto req = http::HTTPRequest::mkPost(url, req_body);
  //auth_->signRequest(static_cast<Session*>(txn_->getUserData()), &req);

  http::HTTPClient http_client;
  http::HTTPResponse res;
  auto rc = http_client.executeRequest(req, &res);
  if (!rc.isSuccess()) {
    return rc;
  }

  if (res.statusCode() == 201) {
    *result = msg::decode<MetadataOperationResult>(res.body());
    return Status::success();
  } else {
    return Status(eIOError, res.body().toString());
  }
}

Status MetadataCoordinator::createFile(
    const String& ns,
    const String& table_name,
    const MetadataFile& file,
    const Vector<String>& servers) {
  size_t num_servers = servers.size();
  if (num_servers == 0) {
    return Status(eIllegalArgumentError, "server list can't be empty");
  }

  size_t failures = 0;
  for (const auto& s : servers) {
    auto rc = createFile(ns, table_name, file, s);
    if (!rc.isSuccess()) {
      logDebug("evqld", "error while creating metadata file: $0", rc.message());
      ++failures;
    }
  }

  size_t max_failures = 0;
  if (num_servers > 1) {
    max_failures = (num_servers - 1) / 2;
  }

  if (failures <= max_failures) {
    return Status::success();
  } else {
    return Status(eRuntimeError, "error while creating metadata file");
  }
}

Status MetadataCoordinator::createFile(
    const String& ns,
    const String& table_name,
    const MetadataFile& file,
    const String& server) {
  auto server_cfg = cdir_->getServerConfig(server);
  if (server_cfg.server_addr().empty()) {
    return Status(eRuntimeError, "server is offline");
  }

  logDebug(
      "evqld",
      "Creating metadata file: $0/$1/$2 on $3 ($4)",
      ns,
      table_name,
      file.getTransactionID(),
      server,
      server_cfg.server_addr());

  auto url = StringUtil::format(
      "http://$0/rpc/create_metadata_file?namespace=$1&table=$2",
      server_cfg.server_addr(),
      URI::urlEncode(ns),
      URI::urlEncode(table_name));

  Buffer req_body;
  {
    auto os = BufferOutputStream::fromBuffer(&req_body);
    auto rc = file.encode(os.get());
    if (!rc.isSuccess()) {
      return rc;
    }
  }

  auto req = http::HTTPRequest::mkPost(url, req_body);
  //auth_->signRequest(static_cast<Session*>(txn_->getUserData()), &req);

  http::HTTPClient http_client;
  http::HTTPResponse res;
  auto rc = http_client.executeRequest(req, &res);
  if (!rc.isSuccess()) {
    return rc;
  }

  if (res.statusCode() == 201) {
    return Status::success();
  } else {
    return Status(eIOError, res.body().toString());
  }
}

Status MetadataCoordinator::discoverPartition(
    PartitionDiscoveryRequest request,
    PartitionDiscoveryResponse* response) {
  auto table_cfg = cdir_->getTableConfig(
      request.db_namespace(),
      request.table_id());

  if (table_cfg.metadata_txnseq() < request.min_txnseq()) {
    return Status(eConcurrentModificationError, "concurrent modification");
  }

  request.set_requester_id(cdir_->getServerID());

  http::HTTPClient http_client;
  for (const auto& s : table_cfg.metadata_servers()) {
    auto server = cdir_->getServerConfig(s);
    if (server.server_status() != SERVER_UP) {
      continue;
    }

    auto url = StringUtil::format(
        "http://$0/rpc/discover_partition_metadata",
        server.server_addr());

    Buffer req_body;
    auto req = http::HTTPRequest::mkPost(url, *msg::encode(request));
    //auth_->signRequest(static_cast<Session*>(txn_->getUserData()), &req);

    http::HTTPResponse res;
    auto rc = http_client.executeRequest(req, &res);
    if (!rc.isSuccess()) {
      logDebug("evqld", "metadata discovery failed: $0", rc.message());
      continue;
    }

    if (res.statusCode() == 200) {
      *response = msg::decode<PartitionDiscoveryResponse>(res.body());
      return Status::success();
    } else {
      logDebug(
          "evqld",
          "metadata discovery failed: $0",
          res.body().toString());
    }
  }

  return Status(eIOError, "no metadata server has the request transaction");
}

} // namespace eventql
