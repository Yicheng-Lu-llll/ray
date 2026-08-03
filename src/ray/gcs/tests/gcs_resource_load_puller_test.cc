// Copyright 2026 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ray/gcs/gcs_resource_load_puller.h"

#include <functional>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/synchronization/mutex.h"
#include "gtest/gtest.h"
#include "ray/asio/asio_util.h"
#include "ray/asio/fake_periodical_runner.h"
#include "ray/raylet_rpc_client/fake_raylet_client.h"

namespace ray {
namespace gcs {

namespace {

class MockRayletClient : public rpc::FakeRayletClient {
 public:
  void GetResourceLoad(
      const rpc::ClientCallback<rpc::GetResourceLoadReply> &callback) override {
    absl::MutexLock lock(&mutex_);
    num_calls_++;
    pending_callbacks_.push_back(callback);
  }

  int NumCalls() {
    absl::MutexLock lock(&mutex_);
    return num_calls_;
  }

  std::vector<rpc::ClientCallback<rpc::GetResourceLoadReply>> TakePendingCallbacks() {
    absl::MutexLock lock(&mutex_);
    return std::move(pending_callbacks_);
  }

 private:
  absl::Mutex mutex_;
  int num_calls_ = 0;
  std::vector<rpc::ClientCallback<rpc::GetResourceLoadReply>> pending_callbacks_;
};

/// Captures the flush fn the puller registers so tests can run it on demand,
/// exactly as the periodic timer would.
class SpyPeriodicalRunner : public FakePeriodicalRunner {
 public:
  void RunFnPeriodically(std::function<void()> fn,
                         uint64_t period_ms,
                         std::string name) override {
    fns_.push_back(std::move(fn));
  }

  const std::vector<std::function<void()>> &fns() const { return fns_; }

 private:
  std::vector<std::function<void()>> fns_;
};

rpc::Address AddressOf(const NodeID &node_id) {
  rpc::Address address;
  address.set_node_id(node_id.Binary());
  address.set_ip_address("127.0.0.1");
  address.set_port(1234);
  return address;
}

rpc::GetResourceLoadReply ReplyFor(const NodeID &node_id) {
  rpc::GetResourceLoadReply reply;
  reply.mutable_resources()->set_node_id(node_id.Binary());
  return reply;
}

}  // namespace

class GcsResourceLoadPullerTest : public ::testing::Test {
 protected:
  GcsResourceLoadPullerTest()
      : pull_io_thread_("test_pull",
                        /*enable_lag_probe=*/false,
                        /*used_for_health_check=*/false),
        main_io_thread_("test_main",
                        /*enable_lag_probe=*/false,
                        /*used_for_health_check=*/false) {}

  std::shared_ptr<MockRayletClient> ClientFor(const NodeID &node_id) {
    absl::MutexLock lock(&mutex_);
    auto &client = clients_[node_id];
    if (client == nullptr) {
      client = std::make_shared<MockRayletClient>();
    }
    return client;
  }

  int FactoryCalls(const NodeID &node_id) {
    absl::MutexLock lock(&mutex_);
    return factory_calls_[node_id];
  }

  std::unique_ptr<GcsResourceLoadPuller> MakePuller() {
    pool_ = std::make_unique<rpc::RayletClientPool>([this](const rpc::Address &address) {
      const auto node_id = NodeID::FromBinary(address.node_id());
      {
        absl::MutexLock lock(&mutex_);
        factory_calls_[node_id]++;
      }
      return ClientFor(node_id);
    });
    return std::make_unique<GcsResourceLoadPuller>(
        pull_io_thread_.GetIoService(),
        main_io_thread_.GetIoService(),
        *pool_,
        flush_runner_,
        [this](std::vector<rpc::ResourcesData> batch) {
          EXPECT_TRUE(
              main_io_thread_.GetIoService().get_executor().running_in_this_thread());
          std::vector<NodeID> node_ids;
          node_ids.reserve(batch.size());
          for (const auto &resources : batch) {
            node_ids.push_back(NodeID::FromBinary(resources.node_id()));
          }
          absl::MutexLock lock(&mutex_);
          applied_batches_.push_back(std::move(node_ids));
        });
  }

  /// Run `fn` on the pull thread and wait until every handler `fn` posted to
  /// the pull io_context has been consumed too. The sentinel resolving the
  /// promise is posted from within the same handler as `fn`, so it lands
  /// behind everything `fn` posted to the pull context; a sentinel posted from
  /// the test thread instead could overtake those handlers (they sit in the io
  /// thread's private queue until the running handler returns). Posts `fn`
  /// makes to other io_contexts (the apply batches on main) are not covered;
  /// use DrainMainThread for those.
  void RunOnPullThreadAndDrain(std::function<void()> fn) {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    pull_io_thread_.GetIoService().post(
        [this, fn = std::move(fn), done]() {
          fn();
          pull_io_thread_.GetIoService().post([done]() { done->set_value(); },
                                              "GcsResourceLoadPullerTest.sentinel");
        },
        "GcsResourceLoadPullerTest.run");
    future.wait();
  }

  /// Wait until every apply batch already posted to the main io_context has
  /// been consumed. Safe to call from the test thread: by the time the
  /// pull-side drain returns, the apply posts are fully enqueued on main, so
  /// this sentinel lands behind them.
  void DrainMainThread() {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    main_io_thread_.GetIoService().post([done]() { done->set_value(); },
                                        "GcsResourceLoadPullerTest.main_sentinel");
    future.wait();
  }

  void PullOnPullThread(GcsResourceLoadPuller &puller,
                        std::vector<rpc::Address> raylet_addresses) {
    RunOnPullThreadAndDrain(
        [&puller, raylet_addresses = std::move(raylet_addresses)]() mutable {
          puller.Pull(std::move(raylet_addresses));
        });
  }

  /// Invoke the pending reply callbacks of `node_ids` (in order) on the pull
  /// thread, as the client call manager would, then drain any cap-triggered
  /// apply batch.
  void DeliverReplies(const std::vector<NodeID> &node_ids) {
    std::vector<std::pair<rpc::ClientCallback<rpc::GetResourceLoadReply>, NodeID>>
        deliveries;
    for (const auto &node_id : node_ids) {
      for (auto &callback : ClientFor(node_id)->TakePendingCallbacks()) {
        deliveries.emplace_back(std::move(callback), node_id);
      }
    }
    RunOnPullThreadAndDrain([deliveries = std::move(deliveries)]() {
      for (const auto &[callback, node_id] : deliveries) {
        callback(Status::OK(), ReplyFor(node_id));
      }
    });
    DrainMainThread();
  }

  /// Fail `node_id`'s pending reply callbacks on the pull thread, as the
  /// client call manager would on a broken connection.
  void DeliverFailure(const NodeID &node_id) {
    auto callbacks = ClientFor(node_id)->TakePendingCallbacks();
    RunOnPullThreadAndDrain([callbacks = std::move(callbacks)]() {
      for (const auto &callback : callbacks) {
        callback(Status::IOError("connection reset"), rpc::GetResourceLoadReply());
      }
    });
    DrainMainThread();
  }

  /// Run the flush fn the puller registered on the SpyPeriodicalRunner, as the
  /// periodic timer would, and wait until the posted apply batch (if any) has
  /// been consumed.
  void FlushAndDrain() {
    ASSERT_EQ(flush_runner_.fns().size(), 1u);
    RunOnPullThreadAndDrain([this]() { flush_runner_.fns()[0](); });
    DrainMainThread();
  }

  std::vector<std::vector<NodeID>> AppliedBatches() {
    absl::MutexLock lock(&mutex_);
    return applied_batches_;
  }

  absl::Mutex mutex_;
  absl::flat_hash_map<NodeID, std::shared_ptr<MockRayletClient>> clients_;
  absl::flat_hash_map<NodeID, int> factory_calls_;
  std::vector<std::vector<NodeID>> applied_batches_;
  std::unique_ptr<rpc::RayletClientPool> pool_;
  SpyPeriodicalRunner flush_runner_;
  // The io threads are declared last so they are stopped and joined before the
  // state their handlers touch is destroyed.
  InstrumentedIOContextWithThread pull_io_thread_;
  InstrumentedIOContextWithThread main_io_thread_;
};

// Each Pull() receives the latest alive nodes, so a node absent from it is no
// longer alive and must be dropped from the client pool automatically.
TEST_F(GcsResourceLoadPullerTest, DisconnectsRayletsThatLeftTheSnapshot) {
  const NodeID node1 = NodeID::FromRandom();
  const NodeID node2 = NodeID::FromRandom();
  auto puller = MakePuller();

  PullOnPullThread(*puller, {AddressOf(node1), AddressOf(node2)});
  EXPECT_EQ(ClientFor(node1)->NumCalls(), 1);
  EXPECT_EQ(ClientFor(node2)->NumCalls(), 1);

  PullOnPullThread(*puller, {AddressOf(node2)});
  EXPECT_EQ(ClientFor(node2)->NumCalls(), 2);

  PullOnPullThread(*puller, {AddressOf(node1), AddressOf(node2)});
  EXPECT_EQ(ClientFor(node1)->NumCalls(), 2);
  EXPECT_EQ(FactoryCalls(node1), 2);
  EXPECT_EQ(FactoryCalls(node2), 1);
}

// Replies are buffered until the periodic flush, which forwards all of them to
// the consumer on the main io_context in one batch, preserving arrival order.
// A failed pull is only logged; it must not contribute an entry to the batch.
TEST_F(GcsResourceLoadPullerTest, FlushForwardsBufferedRepliesAsOneBatch) {
  const NodeID node1 = NodeID::FromRandom();
  const NodeID node2 = NodeID::FromRandom();
  const NodeID node3 = NodeID::FromRandom();
  const NodeID failed_node = NodeID::FromRandom();
  auto puller = MakePuller();

  PullOnPullThread(
      *puller,
      {AddressOf(node1), AddressOf(node2), AddressOf(node3), AddressOf(failed_node)});
  DeliverFailure(failed_node);
  DeliverReplies({node2, node1, node3});
  EXPECT_TRUE(AppliedBatches().empty());

  FlushAndDrain();
  const auto batches = AppliedBatches();
  ASSERT_EQ(batches.size(), 1u);
  EXPECT_EQ(batches[0], (std::vector<NodeID>{node2, node1, node3}));
}

// The reply callbacks and the registered flush fn are self-contained: they
// must keep working when they outlive the puller, as in-flight RPCs and the
// periodic timer do during shutdown.
TEST_F(GcsResourceLoadPullerTest, LateRepliesAfterPullerDestructionStillApply) {
  const NodeID node = NodeID::FromRandom();
  auto puller = MakePuller();

  PullOnPullThread(*puller, {AddressOf(node)});
  puller.reset();

  DeliverReplies({node});
  FlushAndDrain();
  const auto batches = AppliedBatches();
  ASSERT_EQ(batches.size(), 1u);
  EXPECT_EQ(batches[0], (std::vector<NodeID>{node}));
}

}  // namespace gcs
}  // namespace ray
