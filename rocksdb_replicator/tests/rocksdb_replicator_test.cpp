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

#include <string>
#include <vector>

#include "gtest/gtest.h"

// we need this hack to use RocksDBReplicator::RocksDBReplicator(), which is
// private
#define private public
#include "rocksdb_replicator/rocksdb_replicator.h"
#include "rocksdb_replicator/thrift/gen-cpp2/Replicator.h"

using folly::SocketAddress;
using replicator::ReplicaRole;
using replicator::ReturnCode;
using replicator::ReplicaRole;
using replicator::RocksDBReplicator;
using rocksdb::DB;
using rocksdb::Options;
using rocksdb::ReadOptions;
using rocksdb::Status;
using rocksdb::WriteBatch;
using rocksdb::WriteOptions;
using std::chrono::milliseconds;
using std::shared_ptr;
using std::string;
using std::this_thread::sleep_for;
using std::to_string;
using std::unique_ptr;
using std::vector;

DECLARE_int32(replicator_pull_delay_on_error_ms);
DECLARE_int32(replicator_max_server_wait_time_ms);
DECLARE_int32(replicator_client_server_timeout_difference_ms);
DECLARE_bool(reset_upstream_on_empty_updates_from_non_leader);
DECLARE_int32(replicator_max_consecutive_no_updates_before_upstream_reset);

DECLARE_int32(rocksdb_replicator_port);
DECLARE_int32(replicator_replication_mode);
DECLARE_uint64(replicator_timeout_ms);
DECLARE_uint64(replicator_timeout_degraded_ms);
DECLARE_uint64(replicator_consecutive_ack_timeout_before_degradation);

shared_ptr<DB> cleanAndOpenDB(const string& path) {
  EXPECT_EQ(system(("rm -rf " + path).c_str()), 0);
  DB* db;
  Options options;
  options.create_if_missing = true;
  EXPECT_TRUE(DB::Open(options, path, &db).ok());
  return shared_ptr<DB>(db);
}

TEST(RocksDBReplicatorTest, Basics) {
  auto replicator = RocksDBReplicator::instance();
  EXPECT_TRUE(replicator != nullptr);
  EXPECT_EQ(replicator->removeDB("non_exist_db"), ReturnCode::DB_NOT_FOUND);
  WriteOptions options;
  EXPECT_EQ(replicator->write("non_exist_db", options, nullptr),
            ReturnCode::DB_NOT_FOUND);

  auto db_master = cleanAndOpenDB("/tmp/db_master");
  auto db_slave = cleanAndOpenDB("/tmp/db_slave");

  RocksDBReplicator::ReplicatedDB* replicated_db_master;
  RocksDBReplicator::ReplicatedDB* replicated_db_slave;
  SocketAddress addr("127.0.0.1", FLAGS_rocksdb_replicator_port);

  EXPECT_EQ(replicator->addDB("master", db_master, ReplicaRole::LEADER,
                              folly::SocketAddress(),
                              &replicated_db_master), ReturnCode::OK);
  EXPECT_EQ(replicator->addDB("master", db_master, ReplicaRole::LEADER,
                              folly::SocketAddress(),
                              &replicated_db_master), ReturnCode::DB_PRE_EXIST);
  EXPECT_EQ(replicator->addDB("slave", db_slave, ReplicaRole::FOLLOWER,
                              addr, &replicated_db_slave), ReturnCode::OK);

  Status status;
  WriteBatch updates;
  updates.Put("key", "value");
  EXPECT_EQ(replicator->write("slave", options, &updates),
            ReturnCode::WRITE_TO_SLAVE);
  EXPECT_THROW(replicated_db_slave->Write(options, &updates), ReturnCode);
  EXPECT_EQ(replicator->write("master", options, &updates),
            ReturnCode::OK);
  EXPECT_NO_THROW(status = replicated_db_master->Write(options, &updates));
  EXPECT_TRUE(status.ok());

  const char* expected_master_state =
"ReplicatedDB:\n\
  name: master\n\
  ReplicaRole: LEADER\n\
  upstream_addr: uninitialized_addr\n\
  cur_seq_no: 2\n\
  current_replicator_timeout_ms_: 2000\n";
  const char* expected_slave_state =
"ReplicatedDB:\n\
  name: slave\n\
  ReplicaRole: FOLLOWER\n\
  upstream_addr: 127.0.0.1\n\
  cur_seq_no: 0\n\
  current_replicator_timeout_ms_: 2000\n";
  EXPECT_EQ(replicated_db_master->Introspect(), std::string(expected_master_state));
  EXPECT_EQ(replicated_db_slave->Introspect(), std::string(expected_slave_state));

  EXPECT_EQ(ReplicaRole::LEADER, replicated_db_master->role_);
  EXPECT_EQ(ReplicaRole::FOLLOWER, replicated_db_slave->role_);

  EXPECT_EQ(0, replicated_db_master->pullFromUpstreamNoUpdates_);
  EXPECT_EQ(0, replicated_db_slave->pullFromUpstreamNoUpdates_);

  EXPECT_EQ(replicator->removeDB("slave"), ReturnCode::OK);
  EXPECT_EQ(replicator->removeDB("master"), ReturnCode::OK);
  EXPECT_EQ(replicator->removeDB("master"), ReturnCode::DB_NOT_FOUND);
  EXPECT_EQ(replicator->write("slave", options, &updates),
            ReturnCode::DB_NOT_FOUND);
  EXPECT_EQ(replicator->write("master", options, &updates),
            ReturnCode::DB_NOT_FOUND);
}

struct Host {
  explicit Host(int16_t port) {
    FLAGS_rocksdb_replicator_port = port;
    replicator_.reset(new RocksDBReplicator);
  }

  unique_ptr<RocksDBReplicator> replicator_;
};

TEST(RocksDBReplicatorTest, 1_master_1_slave) {
  int16_t master_port = 9092;
  int16_t slave_port = 9093;
  Host master(master_port);
  Host slave(slave_port);

  auto db_master = cleanAndOpenDB("/tmp/db_master");
  auto db_slave = cleanAndOpenDB("/tmp/db_slave");

  EXPECT_EQ(master.replicator_->addDB("shard1", db_master, ReplicaRole::LEADER),
            ReturnCode::OK);
  SocketAddress addr_master("127.0.0.1", master_port);
  EXPECT_EQ(slave.replicator_->addDB("shard1", db_slave, ReplicaRole::FOLLOWER,
                                     addr_master),
            ReturnCode::OK);

  EXPECT_EQ(db_master->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave->GetLatestSequenceNumber(), 0);

  WriteOptions options;
  uint32_t n_keys = 100;
  for (uint32_t i = 0; i < n_keys; ++i) {
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "key", str + "value");
    updates.Put(str + "key2", str + "value2");
    EXPECT_EQ(master.replicator_->write("shard1", options, &updates),
              ReturnCode::OK);
    EXPECT_EQ(db_master->GetLatestSequenceNumber(), i * 2 + 2);
  }

  while (db_slave->GetLatestSequenceNumber() < n_keys * 2) {
    sleep_for(milliseconds(100));
  }

  EXPECT_EQ(db_slave->GetLatestSequenceNumber(), n_keys * 2);
  ReadOptions read_options;
  for (uint32_t i = 0; i < n_keys; ++i) {
    auto str = to_string(i);
    string value;
    auto status = db_slave->Get(read_options, str + "key", &value);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(value, str + "value");

    status = db_slave->Get(read_options, str + "key2", &value);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(value, str + "value2");
  }
  EXPECT_EQ(db_slave->GetLatestSequenceNumber(), n_keys * 2);

  // remove the master db from the replication library, write more keys to the
  // master db. the slave doesn't have the new keys
  EXPECT_EQ(master.replicator_->removeDB("shard1"), ReturnCode::OK);
  for (uint32_t i = 0; i < n_keys; ++i) {
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "new_key", str + "new_value");
    auto status = db_master->Write(options, &updates);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(db_master->GetLatestSequenceNumber(), i + 1 + n_keys * 2);
  }
  EXPECT_EQ(db_slave->GetLatestSequenceNumber(), n_keys * 2);
}

TEST(RocksDBReplicatorTest, 1_master_2_slaves_tree) {
  int16_t master_port = 9094;
  int16_t slave_port_1 = 9095;
  int16_t slave_port_2 = 9096;
  Host master(master_port);
  Host slave_1(slave_port_1);
  Host slave_2(slave_port_2);

  auto db_master = cleanAndOpenDB("/tmp/db_master");
  auto db_slave_1 = cleanAndOpenDB("/tmp/db_slave_1");
  auto db_slave_2 = cleanAndOpenDB("/tmp/db_slave_2");

  EXPECT_EQ(master.replicator_->addDB("shard1", db_master, ReplicaRole::LEADER),
            ReturnCode::OK);
  SocketAddress addr_master("127.0.0.1", master_port);
  EXPECT_EQ(slave_1.replicator_->addDB("shard1", db_slave_1, ReplicaRole::FOLLOWER,
                                       addr_master),
            ReturnCode::OK);
  EXPECT_EQ(slave_2.replicator_->addDB("shard1", db_slave_2, ReplicaRole::FOLLOWER,
                                       addr_master),
            ReturnCode::OK);

  EXPECT_EQ(db_master->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), 0);

  WriteOptions options;
  uint32_t n_keys = 100;
  for (uint32_t i = 0; i < n_keys; ++i) {
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "key", str + "value");
    EXPECT_EQ(master.replicator_->write("shard1", options, &updates),
              ReturnCode::OK);
    EXPECT_EQ(db_master->GetLatestSequenceNumber(), i + 1);
  }

  while (db_slave_1->GetLatestSequenceNumber() < n_keys ||
         db_slave_2->GetLatestSequenceNumber() < n_keys) {
    sleep_for(milliseconds(100));
  }

  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), n_keys);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), n_keys);
  ReadOptions read_options;
  for (uint32_t i = 0; i < n_keys; ++i) {
    auto str = to_string(i);
    string value;
    auto status = db_slave_1->Get(read_options, str + "key", &value);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(value, str + "value");

    status = db_slave_2->Get(read_options, str + "key", &value);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(value, str + "value");
  }
  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), n_keys);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), n_keys);
}

TEST(RocksDBReplicatorTest, 1_master_2_slaves_chain) {
  int16_t master_port = 9097;
  int16_t slave_port_1 = 9098;
  int16_t slave_port_2 = 9099;
  Host master(master_port);
  Host slave_1(slave_port_1);
  Host slave_2(slave_port_2);

  auto db_master = cleanAndOpenDB("/tmp/db_master");
  auto db_slave_1 = cleanAndOpenDB("/tmp/db_slave_1");
  auto db_slave_2 = cleanAndOpenDB("/tmp/db_slave_2");

  EXPECT_EQ(master.replicator_->addDB("shard1", db_master, ReplicaRole::LEADER),
            ReturnCode::OK);
  SocketAddress addr_master("127.0.0.1", master_port);
  EXPECT_EQ(slave_1.replicator_->addDB("shard1", db_slave_1, ReplicaRole::FOLLOWER,
                                       addr_master),
            ReturnCode::OK);
  SocketAddress addr_slave_1("127.0.0.1", slave_port_1);
  EXPECT_EQ(slave_2.replicator_->addDB("shard1", db_slave_2, ReplicaRole::FOLLOWER,
                                       addr_slave_1),
            ReturnCode::OK);

  EXPECT_EQ(db_master->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), 0);

  WriteOptions options;
  uint32_t n_keys = 100;
  for (uint32_t i = 0; i < n_keys; ++i) {
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "key", str + "value");
    EXPECT_EQ(master.replicator_->write("shard1", options, &updates),
              ReturnCode::OK);
    EXPECT_EQ(db_master->GetLatestSequenceNumber(), i + 1);
  }

  while (db_slave_2->GetLatestSequenceNumber() < n_keys) {
    sleep_for(milliseconds(100));
  }

  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), n_keys);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), n_keys);
  ReadOptions read_options;
  for (uint32_t i = 0; i < n_keys; ++i) {
    auto str = to_string(i);
    string value;
    auto status = db_slave_1->Get(read_options, str + "key", &value);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(value, str + "value");

    status = db_slave_2->Get(read_options, str + "key", &value);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(value, str + "value");
  }
  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), n_keys);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), n_keys);

  // remove the middle node, and write some more keys to the master
  EXPECT_EQ(slave_1.replicator_->removeDB("shard1"), ReturnCode::OK);
  for (uint32_t i = 0; i < n_keys; ++i) {
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "new_key", str + "new_value");
    EXPECT_EQ(master.replicator_->write("shard1", options, &updates),
              ReturnCode::OK);
    EXPECT_EQ(db_master->GetLatestSequenceNumber(), i + n_keys + 1);
  }

  // non of slaves got them
  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), n_keys);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), n_keys);

  // add the middle node back
  EXPECT_EQ(slave_1.replicator_->addDB("shard1", db_slave_1, ReplicaRole::FOLLOWER,
                                       addr_master),
            ReturnCode::OK);

  while (db_slave_2->GetLatestSequenceNumber() < 2 * n_keys) {
    sleep_for(milliseconds(100));
  }

  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), 2 * n_keys);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), 2 * n_keys);
  for (uint32_t i = 0; i < n_keys; ++i) {
    auto str = to_string(i);
    string value;
    auto status = db_slave_1->Get(read_options, str + "new_key", &value);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(value, str + "new_value");

    status = db_slave_2->Get(read_options, str + "new_key", &value);
    EXPECT_TRUE(status.ok());
    EXPECT_EQ(value, str + "new_value");
  }
  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), 2 * n_keys);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), 2 * n_keys);
}

// This tests the case when there is one leader and one follower for the shard,
// and the follower sets itself as the upstream, which should trigger upstream reset.
TEST(RocksDBReplicatorTest, 1_leader_1_follower_upstream_itself) {
  FLAGS_replicator_max_server_wait_time_ms = 100;
  FLAGS_replicator_client_server_timeout_difference_ms = 100;
  FLAGS_reset_upstream_on_empty_updates_from_non_leader = true;
  FLAGS_replicator_max_consecutive_no_updates_before_upstream_reset = 1;

  int16_t master_port = 9092;
  int16_t slave_port = 9093;
  Host master(master_port);
  Host slave(slave_port);

  auto db_master = cleanAndOpenDB("/tmp/db_master");
  auto db_slave = cleanAndOpenDB("/tmp/db_slave");

  RocksDBReplicator::ReplicatedDB* replicated_db_master;
  RocksDBReplicator::ReplicatedDB* replicated_db_slave;

  EXPECT_EQ(master.replicator_->addDB("shard1", db_master, ReplicaRole::LEADER, folly::SocketAddress(),
                              &replicated_db_master),
            ReturnCode::OK);

  // follower setting itself as the upstream, hence it won't receive updates from leader,
  // unless the upstream is reset to be the leader.
  SocketAddress addr_slave("127.0.0.1", slave_port);
  EXPECT_EQ(slave.replicator_->addDB("shard1", db_slave, ReplicaRole::FOLLOWER,
                                     addr_slave, &replicated_db_slave),
            ReturnCode::OK);

  EXPECT_EQ(db_master->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave->GetLatestSequenceNumber(), 0);

  WriteOptions options;
  uint32_t n_keys = 100;
  for (uint32_t i = 0; i < n_keys; ++i) {
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "key", str + "value");
    updates.Put(str + "key2", str + "value2");
    EXPECT_EQ(master.replicator_->write("shard1", options, &updates),
              ReturnCode::OK);
    EXPECT_EQ(db_master->GetLatestSequenceNumber(), i * 2 + 2);
  }

  // expect follower reset upstream to be triggered
  auto wait_check = 0;
  while (replicated_db_slave->resetUpstreamAttempts_ == 0 && wait_check < 10) {
    sleep_for(milliseconds(100));
    wait_check++;
  }
  EXPECT_NE(replicated_db_slave->resetUpstreamAttempts_, 0);
  EXPECT_EQ(replicated_db_master->resetUpstreamAttempts_, 0);

  // we don't have helix setup in unit test, so reset won't succeed
  EXPECT_EQ(0, db_slave->GetLatestSequenceNumber());
}

// This tests the case when there is one leader and two followers for the shard,
// and the followers setting each other as the upstream, which should trigger upstream reset.
TEST(RocksDBReplicatorTest, 1_leader_2_followers_deadlock) {
  int16_t master_port = 9097;
  int16_t slave_port_1 = 9098;
  int16_t slave_port_2 = 9099;
  Host master(master_port);
  Host slave_1(slave_port_1);
  Host slave_2(slave_port_2);

  auto db_master = cleanAndOpenDB("/tmp/db_master");
  auto db_slave_1 = cleanAndOpenDB("/tmp/db_slave_1");
  auto db_slave_2 = cleanAndOpenDB("/tmp/db_slave_2");

  RocksDBReplicator::ReplicatedDB* replicated_db_master;
  RocksDBReplicator::ReplicatedDB* replicated_db_slave_1;
  RocksDBReplicator::ReplicatedDB* replicated_db_slave_2;

  EXPECT_EQ(master.replicator_->addDB("shard1", db_master, ReplicaRole::LEADER, folly::SocketAddress(),
                              &replicated_db_master),
            ReturnCode::OK);
  SocketAddress addr_slave2("127.0.0.1", slave_port_2);
  EXPECT_EQ(slave_1.replicator_->addDB("shard1", db_slave_1, ReplicaRole::FOLLOWER,
                                       addr_slave2, &replicated_db_slave_1),
            ReturnCode::OK);
  SocketAddress addr_slave_1("127.0.0.1", slave_port_1);
  EXPECT_EQ(slave_2.replicator_->addDB("shard1", db_slave_2, ReplicaRole::FOLLOWER,
                                       addr_slave_1, &replicated_db_slave_2),
            ReturnCode::OK);

  EXPECT_EQ(db_master->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), 0);

  WriteOptions options;
  uint32_t n_keys = 100;
  for (uint32_t i = 0; i < n_keys; ++i) {
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "key", str + "value");
    EXPECT_EQ(master.replicator_->write("shard1", options, &updates),
              ReturnCode::OK);
    EXPECT_EQ(db_master->GetLatestSequenceNumber(), i + 1);
  }

  EXPECT_EQ(db_slave_1->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave_2->GetLatestSequenceNumber(), 0);

  // expect followers reset upstream to be triggered
  auto wait_check = 0;
  while ((replicated_db_slave_1->resetUpstreamAttempts_ == 0 || replicated_db_slave_2->resetUpstreamAttempts_ == 0)
        && wait_check < 10) {
    sleep_for(milliseconds(100));
    wait_check++;
  }
  EXPECT_NE(replicated_db_slave_1->resetUpstreamAttempts_, 0);
  EXPECT_NE(replicated_db_slave_2->resetUpstreamAttempts_, 0);

  // no upstream reset for the leader
  EXPECT_EQ(replicated_db_master->resetUpstreamAttempts_, 0);

  // we don't have helix setup in unit test, so reset won't succeed
  EXPECT_EQ(0, db_slave_1->GetLatestSequenceNumber());
  EXPECT_EQ(0, db_slave_2->GetLatestSequenceNumber());
}

TEST(RocksDBReplicatorTest, 1_master_1_slave_replication_mode_2) {
  // enable 2-ACK mode
  FLAGS_replicator_replication_mode = 2;

  FLAGS_replicator_timeout_ms = 100;
  FLAGS_replicator_timeout_degraded_ms = 5;
  FLAGS_replicator_consecutive_ack_timeout_before_degradation = 30;

  // setup shard1 with 1 master and 1 slave
  int16_t master_port = 9092;
  int16_t slave_port_1 = 9093;
  int16_t slave_port_2 = 9094;
  Host master(master_port);
  Host slave_shard1(slave_port_1);
  Host slave_shard2(slave_port_2);

  auto db_master_shard1 = cleanAndOpenDB("/tmp/db_master_shard1");
  auto db_master_shard2 = cleanAndOpenDB("/tmp/db_master_shard2");
  auto db_slave_shard1 = cleanAndOpenDB("/tmp/db_slave_shard1");
  auto db_slave_shard2 = cleanAndOpenDB("/tmp/db_slave_shard2");

  RocksDBReplicator::ReplicatedDB* replicated_db_master_shard1;
  RocksDBReplicator::ReplicatedDB* replicated_db_master_shard2;
  RocksDBReplicator::ReplicatedDB* replicated_db_slave_shard1;
  RocksDBReplicator::ReplicatedDB* replicated_db_slave_shard2;

  EXPECT_EQ(master.replicator_->addDB("shard1", db_master_shard1, ReplicaRole::LEADER, folly::SocketAddress(),
                              &replicated_db_master_shard1),
            ReturnCode::OK);
  EXPECT_EQ(master.replicator_->addDB("shard2", db_master_shard2, ReplicaRole::LEADER, folly::SocketAddress(),
                              &replicated_db_master_shard2),
            ReturnCode::OK);
  SocketAddress addr_master("127.0.0.1", master_port);
  EXPECT_EQ(slave_shard1.replicator_->addDB("shard1", db_slave_shard1, ReplicaRole::FOLLOWER,
                                     addr_master, &replicated_db_slave_shard1),
            ReturnCode::OK);
   EXPECT_EQ(slave_shard2.replicator_->addDB("shard2", db_slave_shard2, ReplicaRole::FOLLOWER,
                                     addr_master, &replicated_db_slave_shard2),
            ReturnCode::OK);

  EXPECT_EQ(db_master_shard1->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave_shard1->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_master_shard2->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave_shard2->GetLatestSequenceNumber(), 0);

  // successful writes to both shard1 and shard2
  WriteOptions options;
  uint32_t n_keys = 10;
  for (uint32_t i = 0; i < n_keys; ++i) {
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "key", str + "value");
    updates.Put(str + "key2", str + "value2");

    EXPECT_EQ(master.replicator_->write("shard1", options, &updates),
              ReturnCode::OK);
    EXPECT_EQ(db_master_shard1->GetLatestSequenceNumber(), (i+1)*2);

    EXPECT_EQ(master.replicator_->write("shard2", options, &updates),
              ReturnCode::OK);
    EXPECT_EQ(db_master_shard2->GetLatestSequenceNumber(), (i+1)*2);
  }

  // expect follower to catch up (max 5s)
  auto attempts = 50;
  while (db_slave_shard1->GetLatestSequenceNumber() < n_keys * 2 && attempts != 0) {
    sleep_for(milliseconds(100));
    attempts--;
  }
  EXPECT_EQ(db_slave_shard1->GetLatestSequenceNumber(), n_keys * 2);

  auto attempts2 = 50;
  while (db_slave_shard2->GetLatestSequenceNumber() < n_keys * 2 && attempts2 != 0) {
    sleep_for(milliseconds(100));
    attempts2--;
  }
  EXPECT_EQ(db_slave_shard2->GetLatestSequenceNumber(), n_keys * 2);

  // 2-ack mode write timeouts:
  //
  // remove the shard1 slave db from the replication library.
  // write more keys to the shard1 master db and expect requests to fail
  // due to timeout waiting for slave db ACK.
  // The shard1 slave db will not have the new keys.
  EXPECT_EQ(slave_shard1.replicator_->removeDB("shard1"), ReturnCode::OK);
  for (uint32_t i = 0; i < n_keys; ++i) {
    Status status;
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "new_key", str + "new_value");
    EXPECT_NO_THROW(status = replicated_db_master_shard1->Write(options, &updates));
    EXPECT_TRUE(!status.ok());
    EXPECT_EQ(status, Status::TimedOut("Failed to receive ack from follower"));

    EXPECT_EQ(db_master_shard1->GetLatestSequenceNumber(), i + 1 + n_keys * 2);
  }
  EXPECT_EQ(db_slave_shard1->GetLatestSequenceNumber(), n_keys * 2);
  EXPECT_EQ(100 /* FLAGS_replicator_timeout_ms */, replicated_db_master_shard1->current_replicator_timeout_ms_);

  // shard1 enter degradation mode after consecutive timeouts
  for (uint32_t i = 0; i < FLAGS_replicator_consecutive_ack_timeout_before_degradation; ++i) {
    Status status;
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "new_key", str + "new_value");
    EXPECT_NO_THROW(status = replicated_db_master_shard1->Write(options, &updates));
    EXPECT_TRUE(!status.ok());
    EXPECT_EQ(status, Status::TimedOut("Failed to receive ack from follower"));
  }
  EXPECT_EQ(db_slave_shard1->GetLatestSequenceNumber(), n_keys * 2);
  EXPECT_EQ(5 /* FLAGS_replicator_timeout_degraded_ms */, replicated_db_master_shard1->current_replicator_timeout_ms_);

  // shard2 shouldn't be impacted
  Status status_shard2;
  WriteBatch updates_shard2;
  updates_shard2.Put("new_key", "new_value");
  EXPECT_NO_THROW(status_shard2 = replicated_db_master_shard2->Write(options, &updates_shard2));
  EXPECT_TRUE(status_shard2.ok());
  EXPECT_EQ(100 /* FLAGS_replicator_timeout_ms */, replicated_db_master_shard2->current_replicator_timeout_ms_);

  // shard1 back to normal mode after adding the slave db back
  EXPECT_EQ(slave_shard1.replicator_->addDB("shard1", db_slave_shard1, ReplicaRole::FOLLOWER,
                                     addr_master, &replicated_db_slave_shard1),
            ReturnCode::OK);
  Status status_shard1;
  WriteBatch updates_shard1;
  updates_shard1.Put("new_key", "new_value");
  EXPECT_NO_THROW(status_shard1 = replicated_db_master_shard1->Write(options, &updates_shard1));
  EXPECT_TRUE(status_shard1.ok());
  EXPECT_EQ(100 /* FLAGS_replicator_timeout_ms */, replicated_db_master_shard1->current_replicator_timeout_ms_);
}

TEST(RocksDBReplicatorTest, 1_master_1_slave_1_observer_replication_mode_2) {
  gflags::FlagSaver saver;

  // Enable 2-ACK mode.
  FLAGS_replicator_replication_mode = 2;
  FLAGS_replicator_timeout_ms = 100;

  // Setup a shard with 1 master,  1 slave, and 1 observer.
  const int16_t master_port = 9092;
  const int16_t slave_port = 9093;
  const int16_t observer_port = 9094;
  Host master(master_port);
  Host slave(slave_port);
  Host observer(observer_port);

  auto db_master = cleanAndOpenDB("/tmp/db_master");
  auto db_slave = cleanAndOpenDB("/tmp/db_slave");
  auto db_observer = cleanAndOpenDB("/tmp/db_observer");

  RocksDBReplicator::ReplicatedDB* replicated_db_master;
  RocksDBReplicator::ReplicatedDB* replicated_db_slave;
  RocksDBReplicator::ReplicatedDB* replicated_db_observer;

  EXPECT_EQ(master.replicator_->addDB("shard", db_master, ReplicaRole::LEADER, folly::SocketAddress(),
                              &replicated_db_master),
            ReturnCode::OK); 
  SocketAddress addr_master("127.0.0.1", master_port);
  EXPECT_EQ(slave.replicator_->addDB("shard", db_slave, ReplicaRole::FOLLOWER,
                                     addr_master, &replicated_db_slave),
            ReturnCode::OK);
  EXPECT_EQ(observer.replicator_->addDB("shard", db_observer, ReplicaRole::OBSERVER,
                                     addr_master, &replicated_db_observer),
            ReturnCode::OK);

  EXPECT_EQ(db_master->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_slave->GetLatestSequenceNumber(), 0);
  EXPECT_EQ(db_observer->GetLatestSequenceNumber(), 0);

  // Successful writes to the shard.
  WriteOptions options;
  uint32_t n_keys = 10;
  for (uint32_t i = 0; i < n_keys; ++i) {
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "key", str + "value");
    updates.Put(str + "key2", str + "value2");

    EXPECT_EQ(master.replicator_->write("shard", options, &updates),
              ReturnCode::OK);
    EXPECT_EQ(db_master->GetLatestSequenceNumber(), (i+1)*2);
  }

  // Expect follower to catch up (max 5s).
  auto attempts = 50;
  while (db_slave->GetLatestSequenceNumber() < n_keys * 2 && attempts != 0) {
    sleep_for(milliseconds(100));
    attempts--;
  }
  EXPECT_EQ(db_slave->GetLatestSequenceNumber(), n_keys * 2);

  // Expect observer to catch up (max 5s).
  while (db_observer->GetLatestSequenceNumber() < n_keys * 2 && attempts != 0) {
    sleep_for(milliseconds(100));
    attempts--;
  }
  EXPECT_EQ(db_observer->GetLatestSequenceNumber(), n_keys * 2);

  // Remove observer, 2-ack mode write should still succeed.
  EXPECT_EQ(observer.replicator_->removeDB("shard"), ReturnCode::OK);
  for (uint32_t i = 0; i < n_keys; ++i) {
    Status status;
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "new_key", str + "new_value");
    EXPECT_NO_THROW(status = replicated_db_master->Write(options, &updates));
    EXPECT_TRUE(status.ok());

    EXPECT_EQ(db_master->GetLatestSequenceNumber(), i + 1 + n_keys * 2);
  }

  // Now remove follower as well, expect 2-ack mode write to timeout.
  EXPECT_EQ(slave.replicator_->removeDB("shard"), ReturnCode::OK);
  for (uint32_t i = 0; i < n_keys; ++i) {
    Status status;
    WriteBatch updates;
    auto str = to_string(i);
    updates.Put(str + "new_key", str + "new_value");
    ASSERT_NO_THROW(status = replicated_db_master->Write(options, &updates));
    ASSERT_TRUE(!status.ok());
    EXPECT_EQ(status, Status::TimedOut("Failed to receive ack from follower"));
  }

  // Add the observer back, 2-ack mode write still timeout, since observer ACK does not count.
  EXPECT_EQ(observer.replicator_->addDB("shard", db_observer, ReplicaRole::OBSERVER,
                                     addr_master, &replicated_db_observer),
            ReturnCode::OK);
  Status status;
  WriteBatch updates;
  updates.Put("new_key", "new_value");
  ASSERT_NO_THROW(status = replicated_db_master->Write(options, &updates));
  ASSERT_TRUE(!status.ok());
  EXPECT_EQ(status, Status::TimedOut("Failed to receive ack from follower"));

  // Add the follower db back, 2-ack mode writes should now succeed.
  EXPECT_EQ(slave.replicator_->addDB("shard", db_slave, ReplicaRole::FOLLOWER,
                                     addr_master, &replicated_db_slave),
            ReturnCode::OK);
  Status status1;
  WriteBatch updates1;
  updates1.Put("new_key", "new_value");
  EXPECT_NO_THROW(status1 = replicated_db_master->Write(options, &updates1));
  EXPECT_TRUE(status1.ok());
}

TEST(RocksDBReplicatorTest, Stress) {
  int16_t port_1 = 8081;
  int16_t port_2 = 8082;
  int16_t port_3 = 8083;
  Host host_1(port_1);
  Host host_2(port_2);
  Host host_3(port_3);
  int n_shards = 20;
  uint32_t n_keys = 100;

  vector<shared_ptr<DB>> db_masters;
  vector<shared_ptr<DB>> db_slaves_1;
  vector<shared_ptr<DB>> db_slaves_2;

  for (int i = 0; i < n_shards; ++i) {
    auto str = to_string(i);
    db_masters.emplace_back(cleanAndOpenDB("/tmp/db_master" + str));
    db_slaves_1.emplace_back(cleanAndOpenDB("/tmp/db_slave_1" + str));
    db_slaves_2.emplace_back(cleanAndOpenDB("/tmp/db_slave_2" + str));
  }

  vector<Host*> hosts { &host_1, &host_2, &host_3 };
  vector<SocketAddress> addresses;
  addresses.emplace_back("127.0.0.1", port_1);
  addresses.emplace_back("127.0.0.1", port_2);
  addresses.emplace_back("127.0.0.1", port_3);
  for (int i = 0; i < n_shards; ++i) {
    auto shard = "shard" + to_string(i);
    int start = i % hosts.size();

    EXPECT_EQ(hosts[start]->replicator_->addDB(shard, db_masters[i],
                                              ReplicaRole::LEADER),
              ReturnCode::OK);
    EXPECT_EQ(hosts[(start + 1) % hosts.size()]->replicator_->addDB(
        shard, db_slaves_1[i], ReplicaRole::FOLLOWER, addresses[start]),
              ReturnCode::OK);
    EXPECT_EQ(hosts[(start + 2) % hosts.size()]->replicator_->addDB(
        shard, db_slaves_2[i], ReplicaRole::FOLLOWER, addresses[start]),
              ReturnCode::OK);
  }

  WriteOptions options;
  for (uint32_t i = 0; i < n_keys; ++i) {
    for (int j = 0; j < n_shards; ++j) {
      auto str = to_string(i);
      auto shard = "shard" + to_string(j);
      WriteBatch updates;
      updates.Put(str + "key", str + "value");

      auto code = host_1.replicator_->write(shard, options, &updates);
      EXPECT_TRUE(code == ReturnCode::OK || code == ReturnCode::WRITE_TO_SLAVE);
      code = host_2.replicator_->write(shard, options, &updates);
      EXPECT_TRUE(code == ReturnCode::OK || code == ReturnCode::WRITE_TO_SLAVE);
      code = host_3.replicator_->write(shard, options, &updates);
      EXPECT_TRUE(code == ReturnCode::OK || code == ReturnCode::WRITE_TO_SLAVE);
    }
  }

  ReadOptions read_options;
  for (int i = 0; i < n_shards; ++i) {
    EXPECT_EQ(db_masters[i]->GetLatestSequenceNumber(), n_keys);
    while (db_slaves_1[i]->GetLatestSequenceNumber() < n_keys ||
           db_slaves_2[i]->GetLatestSequenceNumber() < n_keys) {
      sleep_for(milliseconds(100));
    }
    EXPECT_EQ(db_slaves_1[i]->GetLatestSequenceNumber(), n_keys);
    EXPECT_EQ(db_slaves_2[i]->GetLatestSequenceNumber(), n_keys);

    for (uint32_t j = 0; j < n_keys; ++j) {
      string value;
      auto str = to_string(j);
      auto status = db_masters[i]->Get(read_options, str + "key", &value);
      EXPECT_TRUE(status.ok());
      EXPECT_EQ(value, str + "value");

      status = db_slaves_1[i]->Get(read_options, str + "key", &value);
      EXPECT_TRUE(status.ok());
      EXPECT_EQ(value, str + "value");
      status = db_slaves_2[i]->Get(read_options, str + "key", &value);
      EXPECT_TRUE(status.ok());
      EXPECT_EQ(value, str + "value");
    }
  }
}

int main(int argc, char** argv) {
  FLAGS_replicator_pull_delay_on_error_ms = 100;
  ::testing::InitGoogleTest(&argc, argv);
  auto ret = RUN_ALL_TESTS();
  sleep_for(milliseconds(1000));
  return ret;
}

