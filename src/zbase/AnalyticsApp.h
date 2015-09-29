/**
 * Copyright (c) 2015 - The CM Authors <legal@clickmatcher.com>
 *   All Rights Reserved.
 *
 * This file is CONFIDENTIAL -- Distribution or duplication of this material or
 * the information contained herein is strictly forbidden unless prior written
 * permission is obtained.
 */
#ifndef _CM_ANALYTICSAPP_H
#define _CM_ANALYTICSAPP_H
#include "zbase/dproc/Application.h"
#include "zbase/dproc/Task.h"
#include "zbase/dproc/TaskResultFuture.h"
#include "zbase/dproc/DispatchService.h"
#include "stx/protobuf/MessageSchema.h"
#include "zbase/core/TSDBClient.h"
#include "zbase/core/TSDBService.h"
#include "zbase/core/CSTableIndex.h"
#include "zbase/AnalyticsQuery.h"
#include "zbase/AnalyticsQueryResult.h"
#include "zbase/AnalyticsQueryFactory.h"
#include "zbase/ReportFactory.h"
#include "zbase/FeedConfig.pb.h"
#include "zbase/ReportParams.pb.h"
#include "zbase/api/LogfileService.h"
#include "zbase/api/EventsService.h"
#include "zbase/AnalyticsSession.pb.h"
#include "csql/runtime/ExecutionStrategy.h"
#include "zbase/ConfigDirectory.h"

using namespace stx;

namespace zbase {

class AnalyticsApp : public dproc::DefaultApplication {
public:

  AnalyticsApp(
      zbase::TSDBService* tsdb_node,
      zbase::PartitionMap* partition_map,
      zbase::ReplicationScheme* replication_scheme,
      zbase::CSTableIndex* cstable_index,
      ConfigDirectory* cdb,
      AnalyticsAuth* auth,
      csql::Runtime* sql,
      const String& datadir);

  /**
   * Build a "feed" query
   */
  dproc::TaskSpec buildFeedQuery(
      const String& customer,
      const String& feed,
      uint64_t sequence);

  /**
   * Build an "analytics" query
   */
  dproc::TaskSpec buildAnalyticsQuery(
      const AnalyticsSession& session,
      const AnalyticsQuerySpec& query_spec);

  /**
   * Build an "analytics" query
   */
  dproc::TaskSpec buildAnalyticsQuery(
      const AnalyticsSession& session,
      const URI::ParamList& params);

  /**
   * Build a "report" query
   */
  dproc::TaskSpec buildReportQuery(
      const String& customer,
      const String& report,
      const UnixTime& from,
      const UnixTime& until,
      const URI::ParamList& params);

  void configureFeed(const FeedConfig& cfg);

  RefPtr<csql::ExecutionStrategy> getExecutionStrategy(const String& customer);
  RefPtr<csql::TableProvider> getTableProvider(const String& customer) const;

  void insertMetric(
      const String& customer,
      const String& metric,
      const UnixTime& time,
      const String& value);

  // FIXPAUL remove
  void createTable(const TableDefinition& tbl);
  void updateTable(const TableDefinition& tbl, bool force = false);

  LogfileService* logfileService();
  EventsService* eventsService();

protected:

  void configureCustomer(const CustomerConfig& customer);
  void configureTable(const TableDefinition& tbl);

  zbase::TSDBService* tsdb_node_;
  zbase::PartitionMap* partition_map_;
  zbase::CSTableIndex* cstable_index_;
  AnalyticsQueryFactory queries_;
  HashMap<String, FeedConfig> feeds_;
  ConfigDirectory* cdb_;
  String datadir_;

  LogfileService logfile_service_;
  EventsService events_service_;
};

zbase::TableDefinition tableDefinitionToTableConfig(const TableDefinition& tbl);

} // namespace zbase

#endif
