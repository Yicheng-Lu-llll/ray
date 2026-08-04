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

#include "ray/core_worker/task_submission/actor_creation_submitter.h"

#include <deque>
#include <memory>
#include <string>
#include <utility>

#include "gtest/gtest.h"
#include "ray/common/task/task_util.h"
#include "ray/core_worker_rpc_client/fake_core_worker_client.h"
#include "ray/raylet_rpc_client/fake_raylet_client.h"

namespace ray {
namespace core {

namespace {

class PushRecordingWorkerClient : public rpc::FakeCoreWorkerClient {
 public:
  void PushNormalTask(std::unique_ptr<rpc::PushTaskRequest> request,
                      const rpc::ClientCallback<rpc::PushTaskReply> &callback) override {
    push_callbacks.push_back(callback);
  }

  bool ReplyPushTask(Status status = Status::OK()) {
    if (push_callbacks.empty()) {
      return false;
    }
    auto callback = push_callbacks.front();
    push_callbacks.pop_front();
    callback(status, rpc::PushTaskReply());
    return true;
  }

  std::deque<rpc::ClientCallback<rpc::PushTaskReply>> push_callbacks;
};

TaskSpecification BuildCreationTaskSpec(const ActorID &actor_id) {
  rpc::TaskSpec spec;
  spec.set_type(rpc::TaskType::ACTOR_CREATION_TASK);
  spec.set_task_id(TaskID::ForActorCreationTask(actor_id).Binary());
  spec.set_job_id(actor_id.JobId().Binary());
  spec.set_num_returns(1);
  spec.mutable_actor_creation_task_spec()->set_actor_id(actor_id.Binary());
  return TaskSpecification(std::move(spec));
}

rpc::Address RayletAddress(const NodeID &node_id) {
  rpc::Address address;
  address.set_node_id(node_id.Binary());
  address.set_ip_address("127.0.0.1");
  address.set_port(7000);
  return address;
}

class ActorCreationSubmitterTest : public ::testing::Test {
 protected:
  ActorCreationSubmitterTest()
      : raylet_client_(std::make_shared<rpc::FakeRayletClient>()),
        worker_client_(std::make_shared<PushRecordingWorkerClient>()),
        raylet_client_pool_(std::make_shared<rpc::RayletClientPool>(
            [this](const rpc::Address &) { return raylet_client_; })),
        core_worker_client_pool_(std::make_shared<rpc::CoreWorkerClientPool>(
            [this](const rpc::Address &) { return worker_client_; })),
        submitter_(OwnerAddress(),
                   raylet_client_pool_,
                   core_worker_client_pool_,
                   io_service_,
                   /*lease_policy=*/nullptr,
                   /*retry_backoff_ms=*/0) {}

  /// Run deferred backoff retries to completion.
  void PumpBackoff() {
    for (int i = 0; i < 10; i++) {
      io_service_.restart();
      io_service_.poll();
    }
  }

  static rpc::Address OwnerAddress() {
    rpc::Address address;
    address.set_worker_id(WorkerID::FromRandom().Binary());
    address.set_node_id(NodeID::FromRandom().Binary());
    return address;
  }

  std::shared_ptr<rpc::FakeRayletClient> raylet_client_;
  std::shared_ptr<PushRecordingWorkerClient> worker_client_;
  std::shared_ptr<rpc::RayletClientPool> raylet_client_pool_;
  std::shared_ptr<rpc::CoreWorkerClientPool> core_worker_client_pool_;
  instrumented_io_context io_service_;
  ActorCreationSubmitter submitter_;
};

TEST_F(ActorCreationSubmitterTest, GrantThenPushBecomesAliveAndLeaseIsKept) {
  const ActorID actor_id =
      ActorID::Of(JobID::FromInt(1), TaskID::Nil(), /*parent_task_counter=*/1);
  bool done = false;
  ActorCreationSubmitter::CreationResult final_result;
  submitter_.SubmitCreation(BuildCreationTaskSpec(actor_id),
                            RayletAddress(NodeID::FromRandom()),
                            [&](const ActorCreationSubmitter::CreationResult &result) {
                              done = true;
                              final_result = result;
                            });
  EXPECT_EQ(raylet_client_->num_workers_requested, 1);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "1.2.3.4", 1234, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  EXPECT_FALSE(done);
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  EXPECT_TRUE(done);
  EXPECT_TRUE(final_result.status.ok());
  EXPECT_EQ(final_result.actor_address.ip_address(), "1.2.3.4");
  // The lease is retained permanently and never returned.
  EXPECT_TRUE(submitter_.GetGrantedLease(actor_id).has_value());
  EXPECT_EQ(raylet_client_->num_workers_returned, 0);
  EXPECT_EQ(raylet_client_->num_workers_disconnected, 0);
}

TEST_F(ActorCreationSubmitterTest, SpillbackIsFollowedWithFreshLease) {
  const ActorID actor_id =
      ActorID::Of(JobID::FromInt(2), TaskID::Nil(), /*parent_task_counter=*/1);
  bool done = false;
  submitter_.SubmitCreation(BuildCreationTaskSpec(actor_id),
                            RayletAddress(NodeID::FromRandom()),
                            [&](const ActorCreationSubmitter::CreationResult &result) {
                              done = true;
                              EXPECT_TRUE(result.status.ok());
                            });
  // First hop spills to another node.
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "5.6.7.8", 5678, WorkerID::Nil(), NodeID::Nil(), NodeID::FromRandom()));
  EXPECT_EQ(raylet_client_->num_workers_requested, 2);
  // The spillback target grants.
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "5.6.7.8", 5678, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  EXPECT_TRUE(done);
}

TEST_F(ActorCreationSubmitterTest, CancelBeforeGrantConverges) {
  const ActorID actor_id =
      ActorID::Of(JobID::FromInt(3), TaskID::Nil(), /*parent_task_counter=*/1);
  bool creation_done = false;
  submitter_.SubmitCreation(BuildCreationTaskSpec(actor_id),
                            RayletAddress(NodeID::FromRandom()),
                            [&](const ActorCreationSubmitter::CreationResult &result) {
                              creation_done = true;
                              EXPECT_TRUE(result.status.IsSchedulingCancelled());
                            });
  bool cancel_done = false;
  bool cancelled = false;
  submitter_.CancelCreation(actor_id, [&](bool c) {
    cancel_done = true;
    cancelled = c;
  });
  EXPECT_EQ(raylet_client_->num_leases_canceled, 1);
  // First cancel raced ahead of the lease: not yet queued -> retried after
  // the (zero) backoff.
  ASSERT_TRUE(raylet_client_->ReplyCancelWorkerLease(/*success=*/false));
  PumpBackoff();
  EXPECT_EQ(raylet_client_->num_leases_canceled, 2);
  ASSERT_TRUE(raylet_client_->ReplyCancelWorkerLease(/*success=*/true));
  EXPECT_TRUE(cancel_done);
  EXPECT_TRUE(cancelled);
  EXPECT_TRUE(creation_done);
  EXPECT_FALSE(submitter_.GetGrantedLease(actor_id).has_value());
}

TEST_F(ActorCreationSubmitterTest, GrantWinningTheCancelRaceFlipsTheAnswer) {
  const ActorID actor_id =
      ActorID::Of(JobID::FromInt(4), TaskID::Nil(), /*parent_task_counter=*/1);
  submitter_.SubmitCreation(BuildCreationTaskSpec(actor_id),
                            RayletAddress(NodeID::FromRandom()),
                            [](const ActorCreationSubmitter::CreationResult &) {});
  bool cancel_done = false;
  bool cancelled = true;
  submitter_.CancelCreation(actor_id, [&](bool c) {
    cancel_done = true;
    cancelled = c;
  });
  // The grant arrives before any cancel reply.
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "1.2.3.4", 1234, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  EXPECT_TRUE(cancel_done);
  EXPECT_FALSE(cancelled);
}

TEST_F(ActorCreationSubmitterTest, RejectedSpillbackRetriesAtFirstHop) {
  const ActorID actor_id =
      ActorID::Of(JobID::FromInt(5), TaskID::Nil(), /*parent_task_counter=*/1);
  submitter_.SubmitCreation(BuildCreationTaskSpec(actor_id),
                            RayletAddress(NodeID::FromRandom()),
                            [](const ActorCreationSubmitter::CreationResult &) {});
  // Spill to a target, which then rejects; the retry goes back through the
  // first hop (request #3).
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "5.6.7.8", 5678, WorkerID::Nil(), NodeID::Nil(), NodeID::FromRandom()));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease("5.6.7.8",
                                               5678,
                                               WorkerID::FromRandom(),
                                               NodeID::FromRandom(),
                                               NodeID::Nil(),
                                               Status::OK(),
                                               /*rejected=*/true));
  EXPECT_EQ(raylet_client_->num_workers_requested, 3);
}

TEST_F(ActorCreationSubmitterTest, LeaseTransportFailureRetriesSameLease) {
  const ActorID actor_id =
      ActorID::Of(JobID::FromInt(6), TaskID::Nil(), /*parent_task_counter=*/1);
  bool done = false;
  submitter_.SubmitCreation(
      BuildCreationTaskSpec(actor_id),
      RayletAddress(NodeID::FromRandom()),
      [&](const ActorCreationSubmitter::CreationResult &) { done = true; });
  // A transport failure must not abandon the (never-expiring) lease.
  ASSERT_TRUE(raylet_client_->GrantWorkerLease("",
                                               0,
                                               WorkerID::Nil(),
                                               NodeID::Nil(),
                                               NodeID::Nil(),
                                               Status::IOError("lost reply")));
  EXPECT_FALSE(done);
  PumpBackoff();
  EXPECT_EQ(raylet_client_->num_workers_requested, 2);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "1.2.3.4", 1234, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  EXPECT_TRUE(done);
}

TEST_F(ActorCreationSubmitterTest, DoubleCancelBothCallbacksFire) {
  const ActorID actor_id =
      ActorID::Of(JobID::FromInt(7), TaskID::Nil(), /*parent_task_counter=*/1);
  submitter_.SubmitCreation(BuildCreationTaskSpec(actor_id),
                            RayletAddress(NodeID::FromRandom()),
                            [](const ActorCreationSubmitter::CreationResult &) {});
  int cancel_replies = 0;
  submitter_.CancelCreation(actor_id, [&](bool c) { cancel_replies += c ? 1 : 0; });
  submitter_.CancelCreation(actor_id, [&](bool c) { cancel_replies += c ? 1 : 0; });
  // Only one cancel RPC is in flight for both callers.
  EXPECT_EQ(raylet_client_->num_leases_canceled, 1);
  ASSERT_TRUE(raylet_client_->ReplyCancelWorkerLease(/*success=*/true));
  EXPECT_EQ(cancel_replies, 2);
}

TEST_F(ActorCreationSubmitterTest, TerminatedActorCanBeResubmitted) {
  const ActorID actor_id =
      ActorID::Of(JobID::FromInt(8), TaskID::Nil(), /*parent_task_counter=*/1);
  submitter_.SubmitCreation(BuildCreationTaskSpec(actor_id),
                            RayletAddress(NodeID::FromRandom()),
                            [](const ActorCreationSubmitter::CreationResult &) {});
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "1.2.3.4", 1234, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_TRUE(submitter_.GetGrantedLease(actor_id).has_value());

  submitter_.OnActorTerminated(actor_id);
  EXPECT_FALSE(submitter_.GetGrantedLease(actor_id).has_value());
  // Resurrection re-runs the full creation path with a fresh lease.
  submitter_.SubmitCreation(BuildCreationTaskSpec(actor_id),
                            RayletAddress(NodeID::FromRandom()),
                            [](const ActorCreationSubmitter::CreationResult &) {});
  EXPECT_EQ(raylet_client_->num_workers_requested, 2);
}

class RecordingLeasePolicy : public LeasePolicyInterface {
 public:
  explicit RecordingLeasePolicy(rpc::Address address) : address_(std::move(address)) {}
  std::pair<rpc::Address, bool> GetBestNodeForLease(const LeaseSpecification &) override {
    calls++;
    return {address_, true};
  }
  int calls = 0;

 private:
  rpc::Address address_;
};

TEST_F(ActorCreationSubmitterTest, PolicyChoosesStartRaylet) {
  auto policy_address = RayletAddress(NodeID::FromRandom());
  auto policy = std::make_unique<RecordingLeasePolicy>(policy_address);
  auto *policy_ptr = policy.get();
  ActorCreationSubmitter submitter(OwnerAddress(),
                                   raylet_client_pool_,
                                   core_worker_client_pool_,
                                   io_service_,
                                   std::move(policy),
                                   /*retry_backoff_ms=*/0);
  const ActorID actor_id =
      ActorID::Of(JobID::FromInt(9), TaskID::Nil(), /*parent_task_counter=*/1);
  submitter.SubmitCreation(BuildCreationTaskSpec(actor_id),
                           [](const ActorCreationSubmitter::CreationResult &) {});
  EXPECT_EQ(policy_ptr->calls, 1);
  EXPECT_EQ(raylet_client_->num_workers_requested, 1);
}

}  // namespace

}  // namespace core
}  // namespace ray
