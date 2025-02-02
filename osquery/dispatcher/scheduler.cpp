/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#include <ctime>

#include <osquery/config.h>
#include <osquery/core.h>
#include <osquery/database.h>
#include <osquery/flags.h>
#include <osquery/logger.h>

#include "osquery/database/query.h"
#include "osquery/dispatcher/scheduler.h"
#include "osquery/sql/sqlite_util.h"

namespace osquery {

FLAG(bool, enable_monitor, false, "Enable the schedule monitor");

FLAG(uint64, schedule_timeout, 0, "Limit the schedule, 0 for no limit")

inline SQL monitor(const std::string& name, const ScheduledQuery& query) {
  // Snapshot the performance and times for the worker before running.
  auto pid = std::to_string(getpid());
  auto r0 = SQL::selectAllFrom("processes", "pid", EQUALS, pid);
  auto t0 = time(nullptr);
  auto sql = SQLInternal(query.query);
  // Snapshot the performance after, and compare.
  auto t1 = time(nullptr);
  auto r1 = SQL::selectAllFrom("processes", "pid", EQUALS, pid);
  if (r0.size() > 0 && r1.size() > 0) {
    size_t size = 0;
    for (const auto& row : sql.rows()) {
      for (const auto& column : row) {
        size += column.first.size();
        size += column.second.size();
      }
    }
    Config::recordQueryPerformance(name, t1 - t0, size, r0[0], r1[0]);
  }
  return sql;
}

void launchQuery(const std::string& name, const ScheduledQuery& query) {
  // Execute the scheduled query and create a named query object.
  VLOG(1) << "Executing query: " << query.query;
  auto sql =
      (FLAGS_enable_monitor) ? monitor(name, query) : SQLInternal(query.query);

  if (!sql.ok()) {
    LOG(ERROR) << "Error executing query (" << query.query
               << "): " << sql.getMessageString();
    return;
  }

  // Fill in a host identifier fields based on configuration or availability.
  std::string ident = getHostIdentifier();

  // A query log item contains an optional set of differential results or
  // a copy of the most-recent execution alongside some query metadata.
  QueryLogItem item;
  item.name = name;
  item.identifier = ident;
  item.time = osquery::getUnixTime();
  item.calendar_time = osquery::getAsciiTime();

  if (query.options.count("snapshot") && query.options.at("snapshot")) {
    // This is a snapshot query, emit results with a differential or state.
    item.snapshot_results = std::move(sql.rows());
    logSnapshotQuery(item);
    return;
  }

  // Create a database-backed set of query results.
  auto dbQuery = Query(name, query);
  DiffResults diff_results;
  // Add this execution's set of results to the database-tracked named query.
  // We can then ask for a differential from the last time this named query
  // was executed by exact matching each row.
  auto status = dbQuery.addNewResults(sql.rows(), diff_results);
  if (!status.ok()) {
    LOG(ERROR) << "Error adding new results to database: " << status.what();
    return;
  }

  if (diff_results.added.size() == 0 && diff_results.removed.size() == 0) {
    // No diff results or events to emit.
    return;
  }

  VLOG(1) << "Found results for query (" << name << ") for host: " << ident;
  item.results = diff_results;
  if (query.options.count("removed") && !query.options.at("removed")) {
    item.results.removed.clear();
  }

  status = logQueryLogItem(item);
  if (!status.ok()) {
    LOG(ERROR) << "Error logging the results of query (" << query.query
               << "): " << status.toString();
  }
}

void SchedulerRunner::start() {
  time_t t = std::time(nullptr);
  struct tm* local = std::localtime(&t);
  unsigned long int i = local->tm_sec;
  for (; (timeout_ == 0) || (i <= timeout_); ++i) {
    {
      ConfigDataInstance config;
      for (const auto& query : config.schedule()) {
        if (i % query.second.splayed_interval == 0) {
          launchQuery(query.first, query.second);
        }
      }
    }
    // Put the thread into an interruptible sleep without a config instance.
    osquery::interruptableSleep(interval_ * 1000);
  }
}

Status startScheduler() {
  if (startScheduler(FLAGS_schedule_timeout, 1).ok()) {
    Dispatcher::joinServices();
    return Status(0, "OK");
  }
  return Status(1, "Could not start scheduler");
}

Status startScheduler(unsigned long int timeout, size_t interval) {
  Dispatcher::addService(std::make_shared<SchedulerRunner>(timeout, interval));
  return Status(0, "OK");
}
}
