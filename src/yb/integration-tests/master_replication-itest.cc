// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
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

#include <vector>

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include "yb/client/client.h"
#include "yb/common/schema.h"
#include "yb/gutil/strings/substitute.h"
#include "yb/integration-tests/mini_cluster.h"
#include "yb/integration-tests/yb_mini_cluster_test_base.h"
#include "yb/master/catalog_manager.h"
#include "yb/master/master.h"
#include "yb/master/mini_master.h"
#include "yb/util/test_util.h"

using std::vector;

namespace yb {
namespace master {

using client::YBClient;
using client::YBClientBuilder;
using client::YBColumnSchema;
using client::YBSchema;
using client::YBSchemaBuilder;
using client::YBTable;
using client::YBTableCreator;
using client::YBTableName;
using client::YBTableType;
using std::shared_ptr;

const std::string kKeyspaceName("my_keyspace");
const YBTableName kTableName1(kKeyspaceName, "testMasterReplication-1");
const YBTableName kTableName2(kKeyspaceName, "testMasterReplication-2");

const int kNumTabletServerReplicas = 3;

class MasterReplicationTest : public YBMiniClusterTestBase<MiniCluster> {
 public:
  MasterReplicationTest() {
    opts_.num_masters = num_masters_ = 3;
    opts_.num_tablet_servers = kNumTabletServerReplicas;
  }

  void SetUp() override {
    YBMiniClusterTestBase::SetUp();
    cluster_.reset(new MiniCluster(env_.get(), opts_));
    ASSERT_OK(cluster_->Start());
    ASSERT_OK(cluster_->WaitForTabletServerCount(kNumTabletServerReplicas));
  }

  void DoTearDown() override {
    if (cluster_) {
      cluster_->Shutdown();
      cluster_.reset();
    }
    YBMiniClusterTestBase::DoTearDown();
  }

  // This method is meant to be run in a separate thread.
  void StartClusterDelayed(int64_t micros) {
    LOG(INFO) << "Sleeping for "  << micros << " micro seconds...";
    SleepFor(MonoDelta::FromMicroseconds(micros));
    LOG(INFO) << "Attempting to start the cluster...";
    CHECK_OK(cluster_->Start());
    CHECK_OK(cluster_->WaitForTabletServerCount(kNumTabletServerReplicas));
  }

  void ListMasterServerAddrs(vector<string>* out) {
    for (int i = 0; i < num_masters_; i++) {
      out->push_back(cluster_->mini_master(i)->bound_rpc_addr_str());
    }
  }

  Status CreateClient(shared_ptr<YBClient>* out) {
    YBClientBuilder builder;
    for (int i = 0; i < num_masters_; i++) {
      if (!cluster_->mini_master(i)->master()->IsShutdown()) {
        builder.add_master_server_addr(cluster_->mini_master(i)->bound_rpc_addr_str());
      }
    }

    RETURN_NOT_OK(builder.Build(out));
    return (*out)->CreateNamespaceIfNotExists(kKeyspaceName);
  }

  Status CreateTable(const shared_ptr<YBClient>& client,
                     const YBTableName& table_name) {
    YBSchema schema;
    YBSchemaBuilder b;
    b.AddColumn("key")->Type(INT32)->NotNull()->PrimaryKey();
    b.AddColumn("int_val")->Type(INT32)->NotNull();
    b.AddColumn("string_val")->Type(STRING)->NotNull();
    CHECK_OK(b.Build(&schema));
    gscoped_ptr<YBTableCreator> table_creator(client->NewTableCreator());
    return table_creator->table_name(table_name)
        .schema(&schema)
        .Create();
  }

  void VerifyTableExists(const shared_ptr<YBClient>& client,
                         const YBTableName& table_name) {
    LOG(INFO) << "Verifying that " << table_name.ToString() << " exists on leader..";
    vector<YBTableName> tables;
    ASSERT_OK(client->ListTables(&tables));
    ASSERT_TRUE(::util::gtl::contains(tables.begin(), tables.end(), table_name));
  }

  void VerifyMasterRestart() {
    LOG(INFO) << "Check that all " << num_masters_ << " masters are up first.";
    for (int i = 0; i < num_masters_; ++i) {
      LOG(INFO) << "Checking master " << i;
      auto* master = cluster_->mini_master(i)->master();
      ASSERT_FALSE(master->IsShutdown());
      ASSERT_OK(master->WaitForCatalogManagerInit());
    }

    LOG(INFO) << "Restart the first master -- expected to succeed.";
    auto* first_master = cluster_->mini_master(0);
    ASSERT_OK(first_master->Restart());

    LOG(INFO) << "Shutdown the master.";
    first_master->Shutdown();

    LOG(INFO) << "Normal start call should also work fine (and just reload the sys catalog).";
    ASSERT_OK(first_master->Start());
  }

 protected:
  int num_masters_;
  MiniClusterOptions opts_;
};

TEST_F(MasterReplicationTest, TestMasterClusterCreate) {
  DontVerifyClusterBeforeNextTearDown();

  // We want to confirm that the cluster starts properly and fails if you restart it.
  ASSERT_NO_FATAL_FAILURE(VerifyMasterRestart());
}

// Basic test. Verify that:
//
// 1) We can start multiple masters in a distributed configuration and
// that the clients and tablet servers can connect to the leader
// master.
//
// 2) We can create a table (using the standard client APIs) on the
// the leader and ensure that the appropriate table/tablet info is
// replicated to the newly elected leader.
TEST_F(MasterReplicationTest, TestSysTablesReplication) {
  shared_ptr<YBClient> client;

  // Create the first table.
  ASSERT_OK(CreateClient(&client));
  ASSERT_OK(CreateTable(client, kTableName1));

  // TODO: once fault tolerant DDL is in, remove the line below.
  ASSERT_OK(CreateClient(&client));

  ASSERT_OK(cluster_->WaitForTabletServerCount(kNumTabletServerReplicas));

  // Repeat the same for the second table.
  ASSERT_OK(CreateTable(client, kTableName2));
  ASSERT_NO_FATALS(VerifyTableExists(client, kTableName2));
}

// When all masters are down, test that we can timeout the connection
// attempts after a specified deadline.
TEST_F(MasterReplicationTest, TestTimeoutWhenAllMastersAreDown) {
  vector<string> master_addrs;
  ListMasterServerAddrs(&master_addrs);

  cluster_->Shutdown();

  shared_ptr<YBClient> client;
  YBClientBuilder builder;
  builder.master_server_addrs(master_addrs);
  builder.default_rpc_timeout(MonoDelta::FromMilliseconds(100));
  Status s = builder.Build(&client);
  EXPECT_TRUE(!s.ok());
  EXPECT_TRUE(s.IsTimedOut());

  // We need to reset 'cluster_' so that TearDown() can run correctly.
  cluster_.reset();
}

// Shut the cluster down, start initializing the client, and then
// bring the cluster back up during the initialization (but before the
// timeout can elapse).
TEST_F(MasterReplicationTest, TestCycleThroughAllMasters) {
  DontVerifyClusterBeforeNextTearDown();
  vector<string> master_addrs;
  ListMasterServerAddrs(&master_addrs);

  // Shut the cluster down and ...
  cluster_->Shutdown();
  // ... start the cluster after a delay.
  scoped_refptr<yb::Thread> start_thread;
  ASSERT_OK(Thread::Create(
      "TestCycleThroughAllMasters", "start_thread",
      &MasterReplicationTest::StartClusterDelayed,
      this,
      100 * 1000, // start after 100 millis.
      &start_thread));

  // Verify that the client doesn't give up even though the entire
  // cluster is down for 100 milliseconds.
  shared_ptr<YBClient> client;
  YBClientBuilder builder;
  builder.master_server_addrs(master_addrs);
  // Bumped up timeout from 15 sec to 30 sec because master election can take longer than 15 sec.
  // https://yugabyte.atlassian.net/browse/ENG-51
  // Test log: https://gist.githubusercontent.com/mbautin/9f4269292e6ecb5b9a2fc644e2ee4398/raw
  builder.default_admin_operation_timeout(MonoDelta::FromSeconds(30));
  EXPECT_OK(builder.Build(&client));

  ASSERT_OK(ThreadJoiner(start_thread.get()).Join());
}

}  // namespace master
}  // namespace yb
