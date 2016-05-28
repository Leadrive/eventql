/**
 * Copyright (c) 2016 zScale Technology GmbH <legal@zscale.io>
 * Authors:
 *   - Paul Asmuth <paul@zscale.io>
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

namespace eventql {

MetadataCoordinator::MetadataCoordinator(ConfigDirectory* cdir) : cdir_(cdir) {}

Status MetadataCoordinator::performOperation(
    const String& ns,
    const String& table_name,
    MetadataOperation op) {
  return Status::success();
}

Status MetadataCoordinator::createFile(
    const String& ns,
    const String& table_name,
    const SHA1Hash& transaction_id,
    const Vector<String>& servers) {
  size_t num_servers = servers.size();
  if (num_servers == 0) {
    return Status(eIllegalArgumentError, "server list can't be empty");
  }

  size_t failures = 0;
  for (const auto& s : servers) {
    auto rc = createFile(ns, table_name, transaction_id, s);
    if (!rc.isSuccess()) {
      logWarning("evqld", "error while creating metadata file: $0", rc.message());
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
    const SHA1Hash& transaction_id,
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
      transaction_id.toString(),
      server,
      server_cfg.server_addr());

  auto url = StringUtil::format(
      "http://$0/rpc/create_metadata_file?namespace=$1&table=$2&txid=$3",
      server_cfg.server_addr(),
      URI::urlEncode(ns),
      URI::urlEncode(table_name),
      URI::urlEncode(transaction_id.toString()));

  Buffer req_body;
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

} // namespace eventql
