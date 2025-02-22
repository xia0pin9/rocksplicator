/// Copyright 2016 Pinterest Inc.
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
/// http://www.apache.org/licenses/LICENSE-2.0

/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.

//
// @author bol (bol@pinterest.com)
//

#include "rocksdb_replicator/rocksdb_replicator.h"

#include <gflags/gflags.h>

#include <string>

#include "rocksdb_replicator/replicator_handler.h"
#include "rocksdb_replicator/rocksdb_wrapper.h"
#if __GNUC__ >= 8
#include "folly/executors/CPUThreadPoolExecutor.h"
#include "folly/executors/IOThreadPoolExecutor.h"
#include "folly/executors/thread_factory/NamedThreadFactory.h"
#else
#include "wangle/concurrent/CPUThreadPoolExecutor.h"
#endif

DEFINE_int32(rocksdb_replicator_port, 9091,
             "The port # for the internal thrift server.");

DEFINE_int32(num_replicator_io_threads, 8,
             "The number of io threads.");

DEFINE_int32(rocksdb_replicator_executor_threads, 32,
             "The number of rocksplicator executor threads.");

namespace replicator {

RocksDBReplicator::RocksDBReplicator()
    : executor_()
    , client_pool_(FLAGS_num_replicator_io_threads)
    , db_map_()
#if __GNUC__ >= 8
    , server_()
#else
    , server_("disabled", false)
#endif
    , thread_()
    , cleaner_() {
#if __GNUC__ >= 8
  executor_ = std::make_unique<folly::CPUThreadPoolExecutor>(
#else
  executor_ = std::make_unique<wangle::CPUThreadPoolExecutor>(
#endif
    std::max(FLAGS_rocksdb_replicator_executor_threads, 16),
#if __GNUC__ >= 8
    std::make_shared<folly::NamedThreadFactory>("rptor-worker-"));
#else
    std::make_shared<wangle::NamedThreadFactory>("rptor-worker-"));
#endif

  server_.setInterface(std::make_unique<ReplicatorHandler>(&db_map_));
  server_.setPort(FLAGS_rocksdb_replicator_port);
#if __GNUC__ >= 8
  auto io_thread_pool = std::make_shared<folly::IOThreadPoolExecutor>(
    0, std::make_shared<folly::NamedThreadFactory>("rptor-svr-io-"));
#else
  auto io_thread_pool = std::make_shared<wangle::IOThreadPoolExecutor>(
    0, std::make_shared<wangle::NamedThreadFactory>("rptor-svr-io-"));
#endif
  server_.setIOThreadPool(std::move(io_thread_pool));
  // TODO(bol) share io threads between server_ and client_pool_
  server_.setNWorkerThreads(FLAGS_num_replicator_io_threads);

  thread_ = std::thread([this] {
      LOG(INFO) << "Starting replicator server ...";
      this->server_.serve();
      LOG(INFO) << "Stoping replicator server ...";
    });
}

RocksDBReplicator::~RocksDBReplicator() {
  db_map_.clear();
  cleaner_.stopAndWait();
  server_.stop();
  thread_.join();
}

ReturnCode RocksDBReplicator::addDB(const std::string& db_name,
                                    std::shared_ptr<rocksdb::DB> db,
                                    const ReplicaRole role,
                                    const folly::SocketAddress& upstream_addr,
                                    ReplicatedDB** replicated_db) {
  std::shared_ptr<DbWrapper> db_wrapper(
    new RocksDbWrapper(db_name, std::move(db))
  );
  return addDB(db_name, db_wrapper, role, upstream_addr, replicated_db);
}

ReturnCode RocksDBReplicator::addDB(const std::string& db_name,
                                    std::shared_ptr<DbWrapper> db_wrapper,
                                    const ReplicaRole role,
                                    const folly::SocketAddress& upstream_addr,
                                    ReplicatedDB** replicated_db,
                                    const std::string& replicator_zk_cluster,
                                    const std::string& replicator_helix_cluster) {
  std::shared_ptr<ReplicatedDB> new_db(
    new ReplicatedDB(db_name, std::move(db_wrapper), executor_.get(),
                     role, upstream_addr, &client_pool_, replicator_zk_cluster, replicator_helix_cluster));

  if (!db_map_.add(db_name, new_db)) {
    return ReturnCode::DB_PRE_EXIST;
  }

  if (replicated_db) {
    *replicated_db = new_db.get();
  }

  if (role == ReplicaRole::FOLLOWER || role == ReplicaRole::OBSERVER) {
    new_db->pullFromUpstream();
  }

  cleaner_.addDB(new_db);

  return ReturnCode::OK;
}

ReturnCode RocksDBReplicator::removeDB(const std::string& db_name) {
  std::shared_ptr<RocksDBReplicator::ReplicatedDB> db;
  auto exist = db_map_.remove(db_name, &db);
  if (!exist) {
    return ReturnCode::DB_NOT_FOUND;
  }

  std::weak_ptr<RocksDBReplicator::ReplicatedDB> weak_db(db);
  db.reset();

  while (!weak_db.expired()) {
    static const int kRemoveDBRefWaitMilliSec = 200;
    LOG(INFO) << db_name << " is still holding by others, wait "
      << kRemoveDBRefWaitMilliSec << " milliseconds";
    std::this_thread::sleep_for(
      std::chrono::milliseconds(kRemoveDBRefWaitMilliSec));
  }

  return ReturnCode::OK;
}

ReturnCode RocksDBReplicator::write(const std::string& db_name,
                                    const rocksdb::WriteOptions& options,
                                    rocksdb::WriteBatch* updates,
                                    rocksdb::SequenceNumber* seq_no) {
  std::shared_ptr<ReplicatedDB> db;
  if (!db_map_.get(db_name, &db)) {
    return ReturnCode::DB_NOT_FOUND;
  }

  try {
    auto status = db->Write(options, updates, seq_no);
    return status.ok() ? ReturnCode::OK : ReturnCode::WRITE_ERROR;
  } catch (const ReturnCode code) {
    return code;
  }
}

}  // namespace replicator
