// Copyright 2025 The Ray Authors.
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

#include "ray/core_worker/task_submission/actor_task_submitter.h"

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "mock/ray/core_worker/task_manager_interface.h"
#include "mock/ray/gcs_client/gcs_client.h"
#include "ray/common/protobuf_utils.h"
#include "ray/common/test_utils.h"
#include "ray/core_worker/actor_management/fake_actor_creator.h"
#include "ray/core_worker/lease_policy.h"
#include "ray/core_worker/reference_counter.h"
#include "ray/core_worker/reference_counter_interface.h"
#include "ray/core_worker_rpc_client/fake_core_worker_client.h"
#include "ray/observability/fake_metric.h"
#include "ray/pubsub/fake_publisher.h"
#include "ray/pubsub/fake_subscriber.h"
#include "ray/raylet_rpc_client/fake_raylet_client.h"
#include "ray/raylet_rpc_client/raylet_client_pool.h"
#include "ray/util/clock.h"

namespace ray::core {

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;
rpc::ActorDeathCause CreateMockDeathCause() {
  ray::rpc::ActorDeathCause death_cause;
  death_cause.mutable_runtime_env_failed_context()->set_error_message("failed");
  return death_cause;
}

TaskSpecification CreateActorTaskHelper(ActorID actor_id,
                                        WorkerID caller_worker_id,
                                        int64_t counter,
                                        TaskID caller_id = TaskID::Nil()) {
  TaskSpecification task;
  task.GetMutableMessage().set_task_id(TaskID::FromRandom(actor_id.JobId()).Binary());
  task.GetMutableMessage().set_attempt_number(0);
  task.GetMutableMessage().set_caller_id(caller_id.Binary());
  task.GetMutableMessage().set_type(TaskType::ACTOR_TASK);
  task.GetMutableMessage().mutable_caller_address()->set_worker_id(
      caller_worker_id.Binary());
  task.GetMutableMessage().mutable_actor_task_spec()->set_actor_id(actor_id.Binary());
  task.GetMutableMessage()
      .mutable_actor_task_spec()
      ->set_concurrency_group_sequence_number(counter);
  task.GetMutableMessage().set_num_returns(0);
  return task;
}

class MockWorkerClient : public rpc::FakeCoreWorkerClient {
 public:
  const rpc::Address &Addr() const override { return addr; }

  void PushActorTask(std::unique_ptr<rpc::PushTaskRequest> request,
                     bool skip_queue,
                     rpc::ClientCallback<rpc::PushTaskReply> &&callback) override {
    received_seq_nos.push_back(request->sequence_number());
    callbacks.emplace(std::make_pair(TaskID::FromBinary(request->task_spec().task_id()),
                                     request->task_spec().attempt_number()),
                      callback);
  }

  bool ReplyPushTask(TaskAttempt task_attempt, Status status) {
    if (callbacks.size() == 0 || callbacks.find(task_attempt) == callbacks.end()) {
      return false;
    }
    auto &callback = callbacks[task_attempt];
    callback(status, rpc::PushTaskReply());
    callbacks.erase(task_attempt);
    return true;
  }

  rpc::Address addr;
  absl::flat_hash_map<TaskAttempt, rpc::ClientCallback<rpc::PushTaskReply>> callbacks;
  std::vector<int64_t> received_seq_nos;
  int64_t acked_seqno = 0;
};

class ActorTaskSubmitterTest : public ::testing::TestWithParam<bool> {
 public:
  ActorTaskSubmitterTest()
      : io_work(io_context.get_executor()),
        client_pool_(std::make_shared<rpc::CoreWorkerClientPool>(
            [&](const rpc::Address &addr) { return worker_client_; })),
        raylet_client_pool_(std::make_shared<rpc::RayletClientPool>(
            [](const rpc::Address &) -> std::shared_ptr<RayletClientInterface> {
              return nullptr;
            })),
        worker_client_(std::make_shared<MockWorkerClient>()),
        store_(std::make_shared<CoreWorkerMemoryStore>(io_context, clock_)),
        task_manager_(std::make_shared<MockTaskManagerInterface>()),
        mock_gcs_client_(std::make_shared<gcs::MockGcsClient>()),
        publisher_(std::make_unique<pubsub::FakePublisher>()),
        subscriber_(std::make_unique<pubsub::FakeSubscriber>()),
        fake_owned_object_count_gauge_(),
        fake_owned_object_size_gauge_(),
        reference_counter_(std::make_shared<ReferenceCounter>(
            rpc::Address(),
            publisher_.get(),
            subscriber_.get(),
            /*is_node_dead=*/[](const NodeID &) { return false; },
            /*free_object_on_nodes_async=*/
            [](const ObjectID &, const absl::flat_hash_set<NodeID> &) {},
            fake_owned_object_count_gauge_,
            fake_owned_object_size_gauge_,
            /*lineage_pinning_enabled=*/false)),
        submitter_(
            *client_pool_,
            *raylet_client_pool_,
            mock_gcs_client_,
            *store_,
            *task_manager_,
            actor_creator_,
            [](const ObjectID &object_id) { return std::nullopt; },
            [this](const ActorID &actor_id, const std::string &, int64_t num_queued) {
              last_queue_warning_ = num_queued;
            },
            io_context,
            reference_counter_,
            clock_) {}

  void TearDown() override { io_context.stop(); }

  int64_t last_queue_warning_ = 0;
  FakeActorCreator actor_creator_;
  Clock clock_;
  instrumented_io_context io_context;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> io_work;
  std::shared_ptr<rpc::CoreWorkerClientPool> client_pool_;
  std::shared_ptr<rpc::RayletClientPool> raylet_client_pool_;
  std::shared_ptr<MockWorkerClient> worker_client_;
  std::shared_ptr<CoreWorkerMemoryStore> store_;
  std::shared_ptr<MockTaskManagerInterface> task_manager_;
  std::shared_ptr<gcs::MockGcsClient> mock_gcs_client_;
  std::unique_ptr<pubsub::FakePublisher> publisher_;
  std::unique_ptr<pubsub::FakeSubscriber> subscriber_;
  ray::observability::FakeGauge fake_owned_object_count_gauge_;
  ray::observability::FakeGauge fake_owned_object_size_gauge_;
  std::shared_ptr<ReferenceCounterInterface> reference_counter_;
  ActorTaskSubmitter submitter_;
};

TEST_P(ActorTaskSubmitterTest, TestSubmitTask) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);

  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  submitter_.SubmitTask(task1);
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 1);

  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 2);

  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _))
      .Times(worker_client_->callbacks.size());
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(_, _, _, _, _, _)).Times(0);
  worker_client_->ReplyPushTask(task1.GetTaskAttempt(), Status::OK());
  worker_client_->ReplyPushTask(task2.GetTaskAttempt(), Status::OK());
  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1));

  // Connect to the actor again.
  // Because the IP and port of address are not modified, it will skip directly and will
  // not reset `received_seq_nos`.
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1));
}

TEST_P(ActorTaskSubmitterTest, TestQueueingWarning) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);

  for (int i = 0; i < 7500; i++) {
    auto task = CreateActorTaskHelper(actor_id, worker_id, i);
    submitter_.SubmitTask(task);
    ASSERT_EQ(io_context.poll_one(), 1);
    ASSERT_TRUE(worker_client_->ReplyPushTask(task.GetTaskAttempt(), Status::OK()));
  }
  ASSERT_EQ(last_queue_warning_, 0);

  for (int i = 7500; i < 15000; i++) {
    auto task = CreateActorTaskHelper(actor_id, worker_id, i);
    submitter_.SubmitTask(task);
    ASSERT_EQ(io_context.poll_one(), 1);
    /* no ack */
  }
  ASSERT_EQ(last_queue_warning_, 5000);

  for (int i = 15000; i < 35000; i++) {
    auto task = CreateActorTaskHelper(actor_id, worker_id, i);
    submitter_.SubmitTask(task);
    ASSERT_EQ(io_context.poll_one(), 1);
    /* no ack */
  }
  ASSERT_EQ(last_queue_warning_, 20000);
}

TEST_P(ActorTaskSubmitterTest, TestDependencies) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create two tasks for the actor with different arguments.
  ObjectID obj1 = ObjectID::FromRandom();
  ObjectID obj2 = ObjectID::FromRandom();
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  task1.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj1.Binary());
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  task2.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj2.Binary());
  reference_counter_->AddOwnedObject(
      obj1, {}, addr, "", 0, LineageReconstructionEligibility::INELIGIBLE_PUT, true);
  reference_counter_->AddOwnedObject(
      obj2, {}, addr, "", 0, LineageReconstructionEligibility::INELIGIBLE_PUT, true);

  // Neither task can be submitted yet because they are still waiting on
  // dependencies.
  submitter_.SubmitTask(task1);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Put the dependencies in the store in the same order as task submission.
  auto data = GenerateRandomObject();

  // Each Put schedules a callback onto io_context, and let's run it.
  store_->Put(*data, obj1, reference_counter_->HasReference(obj1));
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 1);

  store_->Put(*data, obj2, reference_counter_->HasReference(obj2));
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 2);

  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1));
}

TEST_P(ActorTaskSubmitterTest, TestOutOfOrderDependencies) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create two tasks for the actor with different arguments.
  ObjectID obj1 = ObjectID::FromRandom();
  ObjectID obj2 = ObjectID::FromRandom();
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  task1.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj1.Binary());
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  task2.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj2.Binary());
  reference_counter_->AddOwnedObject(
      obj1, {}, addr, "", 0, LineageReconstructionEligibility::INELIGIBLE_PUT, true);
  reference_counter_->AddOwnedObject(
      obj2, {}, addr, "", 0, LineageReconstructionEligibility::INELIGIBLE_PUT, true);

  // Neither task can be submitted yet because they are still waiting on
  // dependencies.
  submitter_.SubmitTask(task1);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  if (allow_out_of_order_execution) {
    // Put the dependencies in the store in the opposite order of task
    // submission.
    auto data = GenerateRandomObject();
    // task2 is submitted first as we allow out of order execution.
    store_->Put(*data, obj2, reference_counter_->HasReference(obj2));
    ASSERT_EQ(io_context.poll_one(), 1);
    ASSERT_EQ(worker_client_->callbacks.size(), 1);
    ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(1));
    // then task1 is submitted
    store_->Put(*data, obj1, reference_counter_->HasReference(obj1));
    ASSERT_EQ(io_context.poll_one(), 1);
    ASSERT_EQ(worker_client_->callbacks.size(), 2);
    ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(1, 0));
  } else {
    // Put the dependencies in the store in the opposite order of task
    // submission.
    auto data = GenerateRandomObject();
    store_->Put(*data, obj2, reference_counter_->HasReference(obj2));
    ASSERT_EQ(io_context.poll_one(), 1);
    ASSERT_EQ(worker_client_->callbacks.size(), 0);
    store_->Put(*data, obj1, reference_counter_->HasReference(obj1));
    ASSERT_EQ(io_context.poll_one(), 1);
    ASSERT_EQ(worker_client_->callbacks.size(), 2);
    ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1));
  }
}

TEST_P(ActorTaskSubmitterTest, TestActorDead) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create two tasks for the actor. One depends on an object that is not yet available.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  ObjectID obj = ObjectID::FromRandom();
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  task2.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(obj.Binary());
  submitter_.SubmitTask(task1);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 1);

  // Simulate the actor dying. All in-flight tasks should get failed.
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task1.TaskId(), _, _, _, _, _))
      .Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(0);
  ASSERT_TRUE(worker_client_->ReplyPushTask(task1.GetTaskAttempt(), Status::IOError("")));

  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(_, _, _, _, _, _)).Times(0);
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);
  // Actor marked as dead. All queued tasks should get failed.
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1);
  submitter_.DisconnectActor(
      actor_id, 2, /*dead=*/true, death_cause, /*is_restartable=*/false);
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartNoRetry) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create four tasks for the actor.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  auto task3 = CreateActorTaskHelper(actor_id, worker_id, 2);
  auto task4 = CreateActorTaskHelper(actor_id, worker_id, 3);
  // Submit three tasks.
  submitter_.SubmitTask(task1);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task3);
  ASSERT_EQ(io_context.poll_one(), 1);

  EXPECT_CALL(*task_manager_, CompletePendingTask(task1.TaskId(), _, _, _)).Times(1);
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1);
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task3.TaskId(), _, _, _, _, _))
      .Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(task4.TaskId(), _, _, _)).Times(1);
  // First task finishes. Second task fails.
  ASSERT_TRUE(worker_client_->ReplyPushTask(task1.GetTaskAttempt(), Status::OK()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(task2.GetTaskAttempt(), Status::IOError("")));

  // Simulate the actor failing.
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, /*num_restarts=*/1, /*dead=*/false, death_cause, /*is_restartable=*/true);
  // Third task fails after the actor is disconnected. It should not get
  // retried.
  ASSERT_TRUE(worker_client_->ReplyPushTask(task3.GetTaskAttempt(), Status::IOError("")));

  // Actor gets restarted.
  addr.set_port(1);
  submitter_.ConnectActor(actor_id, addr, 1);
  submitter_.SubmitTask(task4);
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(task4.GetTaskAttempt(), Status::OK()));
  ASSERT_TRUE(worker_client_->callbacks.empty());
  // task1, task2 failed, task3 failed, task4
  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1, 2, 3));
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartRetry) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create four tasks for the actor.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  auto task3 = CreateActorTaskHelper(actor_id, worker_id, 2);
  auto task4 = CreateActorTaskHelper(actor_id, worker_id, 3);
  // Submit three tasks.
  submitter_.SubmitTask(task1);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task3);
  ASSERT_EQ(io_context.poll_one(), 1);

  // All tasks will eventually finish.
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(4);
  // Tasks 2 and 3 will be retried.
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task3.TaskId(), _, _, _, _, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  // First task finishes. Second task fails.
  ASSERT_TRUE(worker_client_->ReplyPushTask(task1.GetTaskAttempt(), Status::OK()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(task2.GetTaskAttempt(), Status::IOError("")));

  // Simulate the actor failing.
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, /*num_restarts=*/1, /*dead=*/false, death_cause, /*is_restartable=*/true);
  // Third task fails after the actor is disconnected.
  ASSERT_TRUE(worker_client_->ReplyPushTask(task3.GetTaskAttempt(), Status::IOError("")));

  // Actor gets restarted.
  addr.set_port(1);
  submitter_.ConnectActor(actor_id, addr, 1);
  // A new task is submitted.
  submitter_.SubmitTask(task4);
  ASSERT_EQ(io_context.poll_one(), 1);
  // Tasks 2 and 3 get retried. In the real world, the seq_no of these two tasks should be
  // updated to 4 and 5 by `CoreWorker::InternalHeartbeat`.
  task2.GetMutableMessage().set_attempt_number(task2.AttemptNumber() + 1);
  task2.GetMutableMessage()
      .mutable_actor_task_spec()
      ->set_concurrency_group_sequence_number(4);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  task3.GetMutableMessage().set_attempt_number(task2.AttemptNumber() + 1);
  task3.GetMutableMessage()
      .mutable_actor_task_spec()
      ->set_concurrency_group_sequence_number(5);
  submitter_.SubmitTask(task3);
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(task4.GetTaskAttempt(), Status::OK()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(task2.GetTaskAttempt(), Status::OK()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(task3.GetTaskAttempt(), Status::OK()));
  // task1, task2 failed, task3 failed, task4, task2 retry, task3 retry
  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1, 2, 3, 4, 5));
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartOutOfOrderRetry) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create four tasks for the actor.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  auto task3 = CreateActorTaskHelper(actor_id, worker_id, 2);
  // Submit three tasks.
  submitter_.SubmitTask(task1);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task3);
  ASSERT_EQ(io_context.poll_one(), 1);
  // All tasks will eventually finish.
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(3);

  // Tasks 2 will be retried
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  // First task finishes. Second task hang. Third task finishes.
  ASSERT_TRUE(worker_client_->ReplyPushTask(task1.GetTaskAttempt(), Status::OK()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(task3.GetTaskAttempt(), Status::OK()));
  // Simulate the actor failing.
  ASSERT_TRUE(worker_client_->ReplyPushTask(task2.GetTaskAttempt(), Status::IOError("")));
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);

  // Actor gets restarted.
  addr.set_port(1);
  submitter_.ConnectActor(actor_id, addr, 1);

  // Upon re-connect, task 2 (failed) should be retried.
  // Retry task 2 manually (simulating task_manager and SendPendingTask's behavior)
  task2.GetMutableMessage().set_attempt_number(task2.AttemptNumber() + 1);
  task2.GetMutableMessage()
      .mutable_actor_task_spec()
      ->set_concurrency_group_sequence_number(3);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);

  // Only task2 should be submitted. task 3 (completed) should not be retried.
  ASSERT_EQ(worker_client_->callbacks.size(), 1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(task2.GetTaskAttempt(), Status::OK()));
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartOutOfOrderGcs) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create four tasks for the actor.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  // Submit a task.
  submitter_.SubmitTask(task1);
  ASSERT_EQ(io_context.poll_one(), 1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(task1.TaskId(), _, _, _)).Times(1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(task1.GetTaskAttempt(), Status::OK()));

  // Actor restarts, but we don't receive the disconnect message until later.
  addr.set_port(1);
  submitter_.ConnectActor(actor_id, addr, 1);
  // Submit a task.
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(task2.TaskId(), _, _, _)).Times(1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(task2.GetTaskAttempt(), Status::OK()));

  // We receive the RESTART message late. Nothing happens.
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);
  // Submit a task.
  auto task3 = CreateActorTaskHelper(actor_id, worker_id, 2);
  submitter_.SubmitTask(task3);
  ASSERT_EQ(io_context.poll_one(), 1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(task3.TaskId(), _, _, _)).Times(1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(task3.GetTaskAttempt(), Status::OK()));

  // The actor dies twice. We receive the last RESTART message first.
  submitter_.DisconnectActor(
      actor_id, 3, /*dead=*/false, death_cause, /*is_restartable=*/true);
  // Submit a task.
  auto task4 = CreateActorTaskHelper(actor_id, worker_id, 3);
  submitter_.SubmitTask(task4);
  ASSERT_EQ(io_context.poll_one(), 1);
  // Tasks submitted when the actor is in RESTARTING state will fail immediately.
  // This happens in an io_service.post. Search `SendPendingTasks_ForceFail` to locate
  // the code.
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task4.TaskId(), _, _, _, _, _))
      .Times(1);
  ASSERT_EQ(io_context.poll_one(), 1);

  // We receive the late messages. Nothing happens.
  addr.set_port(2);
  submitter_.ConnectActor(actor_id, addr, 2);
  submitter_.DisconnectActor(
      actor_id, 2, /*dead=*/false, death_cause, /*is_restartable=*/true);

  // The actor dies permanently.
  submitter_.DisconnectActor(
      actor_id, 3, /*dead=*/true, death_cause, /*is_restartable=*/false);

  // We receive more late messages. Nothing happens because the actor is dead.
  submitter_.DisconnectActor(
      actor_id, 4, /*dead=*/false, death_cause, /*is_restartable=*/true);
  addr.set_port(3);
  submitter_.ConnectActor(actor_id, addr, 4);
  // Submit a task.
  auto task5 = CreateActorTaskHelper(actor_id, worker_id, 4);
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task5.TaskId(), _, _, _, _, _))
      .Times(1);
  submitter_.SubmitTask(task5);
  ASSERT_EQ(io_context.poll_one(), 0);
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartFailInflightTasks) {
  const auto allow_out_of_order_execution = GetParam();
  const auto caller_worker_id = WorkerID::FromRandom();
  rpc::Address actor_addr1;
  actor_addr1.set_worker_id(WorkerID::FromRandom().Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ false,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, actor_addr1, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create 3 tasks for the actor.
  auto task1_first_attempt = CreateActorTaskHelper(actor_id, caller_worker_id, 0);
  auto task2_first_attempt = CreateActorTaskHelper(actor_id, caller_worker_id, 1);
  auto task3_first_attempt = CreateActorTaskHelper(actor_id, caller_worker_id, 2);
  // Submit a task.
  submitter_.SubmitTask(task1_first_attempt);
  ASSERT_EQ(io_context.poll_one(), 1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(task1_first_attempt.TaskId(), _, _, _))
      .Times(1);
  ASSERT_TRUE(
      worker_client_->ReplyPushTask(task1_first_attempt.GetTaskAttempt(), Status::OK()));
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Submit 2 tasks.
  submitter_.SubmitTask(task2_first_attempt);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task3_first_attempt);
  ASSERT_EQ(io_context.poll_one(), 1);
  // Actor failed, but the task replies are delayed (or in some scenarios, lost).
  // We should still be able to fail the inflight tasks.
  EXPECT_CALL(*task_manager_,
              FailOrRetryPendingTask(task2_first_attempt.TaskId(), _, _, _, _, _))
      .Times(1);
  EXPECT_CALL(*task_manager_,
              FailOrRetryPendingTask(task3_first_attempt.TaskId(), _, _, _, _, _))
      .Times(1);
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);
  // We haven't called the RPC callback yet, mimicking the situation
  // where they might be delayed by gRPC or the network.
  ASSERT_EQ(worker_client_->callbacks.size(), 2);

  // Submit retries for task2 and task3.
  auto task2_second_attempt = CreateActorTaskHelper(actor_id, caller_worker_id, 3);
  task2_second_attempt.GetMutableMessage().set_task_id(
      task2_first_attempt.TaskIdBinary());
  task2_second_attempt.GetMutableMessage().set_attempt_number(
      task2_first_attempt.AttemptNumber() + 1);
  auto task3_second_attempt = CreateActorTaskHelper(actor_id, caller_worker_id, 4);
  task3_second_attempt.GetMutableMessage().set_task_id(
      task3_first_attempt.TaskIdBinary());
  task3_second_attempt.GetMutableMessage().set_attempt_number(
      task3_first_attempt.AttemptNumber() + 1);
  submitter_.SubmitTask(task2_second_attempt);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task3_second_attempt);
  ASSERT_EQ(io_context.poll_one(), 1);

  // Restart the actor.
  rpc::Address actor_addr2;
  actor_addr2.set_worker_id(WorkerID::FromRandom().Binary());
  submitter_.ConnectActor(actor_id, actor_addr2, 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 4);

  // The task reply of the first attempt of task2 is now received.
  // Since the first attempt is already failed, it will not
  // be marked as failed or finished again.
  EXPECT_CALL(*task_manager_, CompletePendingTask(task2_first_attempt.TaskId(), _, _, _))
      .Times(0);
  EXPECT_CALL(*task_manager_,
              FailOrRetryPendingTask(task2_first_attempt.TaskId(), _, _, _, _, _))
      .Times(0);
  // First attempt of task2 replied with OK.
  ASSERT_TRUE(
      worker_client_->ReplyPushTask(task2_first_attempt.GetTaskAttempt(), Status::OK()));
  // Still have RPC callbacks for the first attempt of task3 and second attempts of task2
  // and task3.
  ASSERT_EQ(worker_client_->callbacks.size(), 3);

  EXPECT_CALL(*task_manager_, CompletePendingTask(task2_second_attempt.TaskId(), _, _, _))
      .Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(task3_second_attempt.TaskId(), _, _, _))
      .Times(1);
  // Second attempt of task2 replied with OK.
  ASSERT_TRUE(
      worker_client_->ReplyPushTask(task2_second_attempt.GetTaskAttempt(), Status::OK()));
  // Second attempt of task3 replied with OK.
  ASSERT_TRUE(
      worker_client_->ReplyPushTask(task3_second_attempt.GetTaskAttempt(), Status::OK()));
  // Still have RPC callbacks for the first attempt of task3.
  ASSERT_EQ(worker_client_->callbacks.size(), 1);

  // The task reply of the first attempt of task3 is now received.
  // Since the first attempt is already failed, it will not
  // be marked as failed or finished again.
  EXPECT_CALL(*task_manager_, CompletePendingTask(task3_first_attempt.TaskId(), _, _, _))
      .Times(0);
  EXPECT_CALL(*task_manager_,
              FailOrRetryPendingTask(task3_first_attempt.TaskId(), _, _, _, _, _))
      .Times(0);
  // First attempt of task3 replied with error.
  ASSERT_TRUE(worker_client_->ReplyPushTask(task3_first_attempt.GetTaskAttempt(),
                                            Status::IOError("")));
  ASSERT_EQ(worker_client_->callbacks.size(), 0);
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartFastFail) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  // Submit a task.
  submitter_.SubmitTask(task1);
  ASSERT_EQ(io_context.poll_one(), 1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(task1.TaskId(), _, _, _)).Times(1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(task1.GetTaskAttempt(), Status::OK()));

  // Actor failed and is now restarting.
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);

  // Submit a new task. This task should fail immediately because "max_task_retries" is 0.
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  submitter_.SubmitTask(task2);
  ASSERT_EQ(io_context.poll_one(), 1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(task2.TaskId(), _, _, _)).Times(0);
  EXPECT_CALL(*task_manager_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1);
  ASSERT_EQ(io_context.poll_one(), 1);
}

TEST_P(ActorTaskSubmitterTest, TestPendingTasks) {
  auto allow_out_of_order_execution = GetParam();
  int32_t max_pending_calls = 10;
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      max_pending_calls,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);

  std::vector<TaskSpecification> tasks;
  // Submit number of `max_pending_calls` tasks would be OK.
  for (int32_t i = 0; i < max_pending_calls; i++) {
    ASSERT_FALSE(submitter_.PendingTasksFull(actor_id));
    auto task = CreateActorTaskHelper(actor_id, worker_id, i);
    tasks.push_back(task);
    submitter_.SubmitTask(task);
    ASSERT_EQ(io_context.poll_one(), 1);
  }

  // Then the queue should be full.
  ASSERT_TRUE(submitter_.PendingTasksFull(actor_id));

  ASSERT_EQ(worker_client_->callbacks.size(), 0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 10);

  // After task 0 reply comes, the queue turn to not full.
  ASSERT_TRUE(worker_client_->ReplyPushTask(tasks[0].GetTaskAttempt(), Status::OK()));
  tasks.erase(tasks.begin());
  ASSERT_FALSE(submitter_.PendingTasksFull(actor_id));

  // We can submit task 10, but after that the queue is full.
  auto task = CreateActorTaskHelper(actor_id, worker_id, 10);
  tasks.push_back(task);
  submitter_.SubmitTask(task);
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_TRUE(submitter_.PendingTasksFull(actor_id));

  // All the replies comes, the queue shouble be empty.
  for (auto &task_spec : tasks) {
    ASSERT_TRUE(worker_client_->ReplyPushTask(task_spec.GetTaskAttempt(), Status::OK()));
  }
  ASSERT_FALSE(submitter_.PendingTasksFull(actor_id));
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartResubmit) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);

  // Generator is pushed to worker -> generator queued for resubmit -> comes back from
  // worker -> resubmit happens.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  submitter_.SubmitTask(task1);
  io_context.run_one();
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 1);
  ASSERT_TRUE(submitter_.QueueGeneratorForResubmit(task1));
  EXPECT_CALL(*task_manager_, MarkGeneratorFailedAndResubmit(task1.TaskId())).Times(1);
  worker_client_->ReplyPushTask(task1.GetTaskAttempt(), Status::OK());
}

// Test that when the head task of an actor's queue is cancelled,
// subsequent tasks with resolved dependencies can proceed.
//
// Scenario:
// - task_a has an unresolved dependency
// - task_b has no dependencies (resolved immediately)
// - In sequential mode, task_b is queued behind task_a
// - Cancel task_a
// - task_b should now execute
TEST_P(ActorTaskSubmitterTest, TestCancelHeadUnblocksQueue) {
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  ObjectID obj1 = ObjectID::FromRandom();
  auto task_a = CreateActorTaskHelper(actor_id, worker_id, 0);
  task_a.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj1.Binary());
  auto task_b = CreateActorTaskHelper(actor_id, worker_id, 1);

  reference_counter_->AddOwnedObject(
      obj1, {}, addr, "", 0, LineageReconstructionEligibility::INELIGIBLE_PUT, true);

  submitter_.SubmitTask(task_a);
  ASSERT_EQ(io_context.poll_one(), 1);
  submitter_.SubmitTask(task_b);
  ASSERT_EQ(io_context.poll_one(), 1);

  if (allow_out_of_order_execution) {
    // In out-of-order mode, task_b is sent immediately after its dependencies
    // resolve, regardless of task_a's state.
    ASSERT_EQ(worker_client_->callbacks.size(), 1);
    ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(1));

    EXPECT_CALL(*task_manager_, IsTaskPending(task_a.TaskId())).WillOnce(Return(true));
    submitter_.CancelTask(task_a, /*recursive=*/false);
    ASSERT_EQ(worker_client_->callbacks.size(), 1);
  } else {
    // In sequential mode, task_b is blocked by task_a even though task_b's
    // dependencies are already resolved.
    ASSERT_EQ(worker_client_->callbacks.size(), 0);

    // At this point, task_b has already resolved its dependencies and will not
    // trigger SendPendingTasks again. If CancelTask does not call SendPendingTasks
    // and handle correctly, task_b will be stuck forever.
    EXPECT_CALL(*task_manager_, IsTaskPending(task_a.TaskId())).WillOnce(Return(true));
    submitter_.CancelTask(task_a, /*recursive=*/false);

    ASSERT_EQ(worker_client_->callbacks.size(), 1);
    ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(1));
  }
}

TEST_P(ActorTaskSubmitterTest, TestPerConcurrencyGroupSequencing) {
  // Test that tasks in different concurrency groups have independent sequencing
  // and do not block each other. When group_a's first task is blocked on a dependency,
  // group_b's tasks should still be sent.
  auto allow_out_of_order_execution = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      allow_out_of_order_execution,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  auto make_task = [actor_id, worker_id](int seq_no, const std::string &group_name) {
    auto task = CreateActorTaskHelper(actor_id, worker_id, seq_no);
    task.GetMutableMessage().set_concurrency_group_name(group_name);
    return task;
  };

  // group_a task 0 has an unresolved dependency, the rest have no deps.
  ObjectID obj_a = ObjectID::FromRandom();
  auto task_a0 = make_task(0, "group_a");
  task_a0.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj_a.Binary());
  reference_counter_->AddOwnedObject(
      obj_a, {}, addr, "", 0, LineageReconstructionEligibility::INELIGIBLE_PUT, true);
  auto task_a1 = make_task(1, "group_a");
  auto task_b0 = make_task(0, "group_b");
  auto task_b1 = make_task(1, "group_b");

  submitter_.SubmitTask(task_a0);
  io_context.run_one();
  submitter_.SubmitTask(task_b0);
  submitter_.SubmitTask(task_b1);
  io_context.run_one();
  io_context.run_one();
  ASSERT_EQ(worker_client_->callbacks.size(), 2);

  submitter_.SubmitTask(task_a1);
  io_context.run_one();
  if (allow_out_of_order_execution) {
    ASSERT_EQ(worker_client_->callbacks.size(), 3);
  } else {
    ASSERT_EQ(worker_client_->callbacks.size(), 2);
  }

  auto data = GenerateRandomObject();
  store_->Put(*data, obj_a, true);
  io_context.run_one();
  ASSERT_EQ(worker_client_->callbacks.size(), 4);

  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(4);
  while (!worker_client_->callbacks.empty()) {
    auto it = worker_client_->callbacks.begin();
    worker_client_->ReplyPushTask(it->first, Status::OK());
  }

  ASSERT_EQ(worker_client_->received_seq_nos.size(), 4);
}

class OwnerManagedWorkerClient : public rpc::FakeCoreWorkerClient {
 public:
  void PushNormalTask(std::unique_ptr<rpc::PushTaskRequest> request,
                      const rpc::ClientCallback<rpc::PushTaskReply> &callback) override {
    push_callbacks.push_back(callback);
  }

  void KillActor(const rpc::KillActorRequest &request,
                 const rpc::ClientCallback<rpc::KillActorReply> &callback) override {
    kill_requests.push_back(request);
    kill_callbacks.push_back(callback);
  }

  bool ReplyPushTask(Status status = Status::OK(),
                     bool is_application_error = false,
                     const std::string &task_execution_error = "") {
    if (push_callbacks.empty()) {
      return false;
    }
    auto callback = push_callbacks.front();
    push_callbacks.pop_front();
    rpc::PushTaskReply reply;
    reply.set_is_application_error(is_application_error);
    reply.set_task_execution_error(task_execution_error);
    callback(status, std::move(reply));
    return true;
  }

  bool ReplyKillActor(Status status = Status::OK()) {
    if (kill_callbacks.empty()) {
      return false;
    }
    auto callback = kill_callbacks.front();
    kill_callbacks.pop_front();
    callback(status, rpc::KillActorReply());
    return true;
  }

  std::deque<rpc::ClientCallback<rpc::PushTaskReply>> push_callbacks;
  std::vector<rpc::KillActorRequest> kill_requests;
  std::deque<rpc::ClientCallback<rpc::KillActorReply>> kill_callbacks;
};

class OwnerManagedActorTest : public ::testing::Test {
 protected:
  OwnerManagedActorTest()
      : io_work(io_context.get_executor()),
        raylet_client_(std::make_shared<rpc::FakeRayletClient>()),
        worker_client_(std::make_shared<OwnerManagedWorkerClient>()),
        client_pool_(std::make_shared<rpc::CoreWorkerClientPool>(
            [this](const rpc::Address &) { return worker_client_; })),
        raylet_client_pool_(std::make_shared<rpc::RayletClientPool>(
            [this](const rpc::Address &) { return raylet_client_; })),
        store_(std::make_shared<CoreWorkerMemoryStore>(io_context, clock_)),
        task_manager_(std::make_shared<MockTaskManagerInterface>()),
        mock_gcs_client_(std::make_shared<gcs::MockGcsClient>()),
        publisher_(std::make_unique<pubsub::FakePublisher>()),
        subscriber_(std::make_unique<pubsub::FakeSubscriber>()),
        reference_counter_(std::make_shared<ReferenceCounter>(
            rpc::Address(),
            publisher_.get(),
            subscriber_.get(),
            /*is_node_dead=*/[](const NodeID &) { return false; },
            /*free_object_on_nodes_async=*/
            [](const ObjectID &, const absl::flat_hash_set<NodeID> &) {},
            fake_owned_object_count_gauge_,
            fake_owned_object_size_gauge_,
            /*lineage_pinning_enabled=*/true)),
        submitter_(
            *client_pool_,
            *raylet_client_pool_,
            mock_gcs_client_,
            *store_,
            *task_manager_,
            actor_creator_,
            [](const ObjectID &object_id) { return std::nullopt; },
            [](const ActorID &, const std::string &, int64_t) {},
            io_context,
            reference_counter_,
            clock_,
            std::make_unique<ActorCreationSubmitter>(
                rpc::Address(),
                raylet_client_pool_,
                client_pool_,
                io_context,
                std::make_unique<LocalLeasePolicy>(LocalRayletAddress()),
                /*retry_backoff_ms=*/0),
            [this](const ActorID &actor_id, const rpc::ActorTableData &actor_data) {
              notified_states.emplace_back(actor_id, actor_data);
            },
            /*kill_retry_delay_ms=*/5) {
    // The raylet-mediated kill path resolves the node on every use; default
    // to an alive node so tests only override what they exercise.
    rpc::GcsNodeAddressAndLiveness default_node;
    default_node.set_node_manager_address("127.0.0.1");
    default_node.set_node_manager_port(7000);
    EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
        .Times(testing::AnyNumber())
        .WillRepeatedly(Return(default_node));
    EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
        .Times(testing::AnyNumber())
        .WillRepeatedly(Return(false));
  }

  void TearDown() override { io_context.stop(); }

  static rpc::Address LocalRayletAddress() {
    rpc::Address address;
    address.set_node_id(NodeID::FromRandom().Binary());
    address.set_ip_address("127.0.0.1");
    address.set_port(7000);
    return address;
  }

  TaskSpecification BuildCreationTaskSpec(const ActorID &actor_id) {
    rpc::TaskSpec spec;
    spec.set_type(rpc::TaskType::ACTOR_CREATION_TASK);
    spec.set_task_id(TaskID::ForActorCreationTask(actor_id).Binary());
    spec.set_job_id(actor_id.JobId().Binary());
    spec.set_num_returns(1);
    // The task display name is never empty in production (the routing
    // predicate must read the actor name, not this).
    spec.set_name("Actor.__init__");
    spec.mutable_actor_creation_task_spec()->set_actor_id(actor_id.Binary());
    return TaskSpecification(std::move(spec));
  }

  /// Register the actor handle's return object like ActorManager does in
  /// production before submission (the owner owns it with a local ref).
  void OwnHandle(const ActorID &actor_id) {
    reference_counter_->AddOwnedObject(ObjectID::ForActorHandle(actor_id),
                                       /*contained_ids=*/{},
                                       rpc::Address(),
                                       "test",
                                       /*object_size*/ -1,
                                       LineageReconstructionEligibility::ELIGIBLE,
                                       /*add_local_ref=*/true);
  }

  void Pump() {
    for (int i = 0; i < 10; i++) {
      io_context.restart();
      io_context.poll();
    }
  }

  Clock clock_;
  instrumented_io_context io_context;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> io_work;
  std::shared_ptr<rpc::FakeRayletClient> raylet_client_;
  std::shared_ptr<OwnerManagedWorkerClient> worker_client_;
  std::shared_ptr<rpc::CoreWorkerClientPool> client_pool_;
  std::shared_ptr<rpc::RayletClientPool> raylet_client_pool_;
  std::shared_ptr<CoreWorkerMemoryStore> store_;
  std::shared_ptr<MockTaskManagerInterface> task_manager_;
  std::shared_ptr<gcs::MockGcsClient> mock_gcs_client_;
  std::unique_ptr<pubsub::FakePublisher> publisher_;
  std::unique_ptr<pubsub::FakeSubscriber> subscriber_;
  ray::observability::FakeGauge fake_owned_object_count_gauge_;
  ray::observability::FakeGauge fake_owned_object_size_gauge_;
  std::shared_ptr<ReferenceCounterInterface> reference_counter_;
  FakeActorCreator actor_creator_;
  std::vector<std::pair<ActorID, rpc::ActorTableData>> notified_states;
  ActorTaskSubmitter submitter_;
};

TEST_F(OwnerManagedActorTest, CreationHappyPathPublishesAliveWithoutGCS) {
  // An unnamed non-detached actor is created from the owner: lease, push,
  // then the owner authors ALIVE. The GCS is never asked to create it.
  ActorID actor_id = ActorID::Of(JobID::FromInt(1), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_,
              CompletePendingTask(_, _, _, /*is_application_error=*/false))
      .Times(1);

  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  ASSERT_EQ(notified_states.size(), 1u);
  EXPECT_EQ(notified_states[0].first, actor_id);
  EXPECT_EQ(notified_states[0].second.state(), rpc::ActorTableData::ALIVE);
  EXPECT_EQ(notified_states[0].second.address().port(), 9998);
  EXPECT_EQ(notified_states[0].second.num_restarts(), 0u);
}

TEST_F(OwnerManagedActorTest, OutOfScopeKillsGrantedActorAndPublishesDead) {
  // When the handle goes out of scope the owner kills its actor directly and
  // authors DEAD(OUT_OF_SCOPE); no GCS report is sent.
  ActorID actor_id = ActorID::Of(JobID::FromInt(2), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(notified_states.size(), 1u);

  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();

  ASSERT_EQ(raylet_client_->kill_local_requests.size(), 1u);
  EXPECT_EQ(raylet_client_->kill_local_requests[0].intended_actor_id(),
            actor_id.Binary());
  EXPECT_FALSE(raylet_client_->kill_local_requests[0].force_kill());
  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(notified_states[1].second.death_cause().actor_died_error_context().reason(),
            rpc::ActorDiedErrorContext::OUT_OF_SCOPE);
}

TEST_F(OwnerManagedActorTest, OutOfScopeDuringPushDefersTermination) {
  // Out-of-scope while the creation push is in flight must not touch the
  // entry (that used to abort the owner); the termination runs after the
  // push completes.
  ActorID actor_id = ActorID::Of(JobID::FromInt(3), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  // Push in flight: drop the handle now.
  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();
  EXPECT_TRUE(raylet_client_->kill_local_requests.empty());

  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(raylet_client_->kill_local_requests.size(), 1u);
  ASSERT_TRUE(raylet_client_->ReplyKillLocalActor());
  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[0].second.state(), rpc::ActorTableData::ALIVE);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::DEAD);
}

TEST_F(OwnerManagedActorTest, InitErrorAuthorsDeadWithCreationError) {
  // A failed __init__ completes the creation as an application error and the
  // owner authors DEAD carrying the error, so queued tasks fail instead of
  // waiting for ALIVE forever.
  ActorID actor_id = ActorID::Of(JobID::FromInt(4), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, /*is_application_error=*/true))
      .Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK(),
                                            /*is_application_error=*/true,
                                            "User exception: boom"));

  ASSERT_EQ(notified_states.size(), 1u);
  EXPECT_EQ(notified_states[0].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(
      notified_states[0].second.death_cause().actor_died_error_context().error_message(),
      "User exception: boom");
  EXPECT_TRUE(raylet_client_->kill_local_requests.empty());
}

TEST_F(OwnerManagedActorTest, InitErrorThenOutOfScopeKeepsDeathCause) {
  // After a failed __init__ authored DEAD(WORKER_DIED), the handle going out
  // of scope must not overwrite the death cause with OUT_OF_SCOPE.
  ActorID actor_id = ActorID::Of(JobID::FromInt(6), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      /*max_pending_calls=*/-1,
                                      /*allow_out_of_order_execution=*/false,
                                      /*fail_if_actor_unreachable=*/false,
                                      /*owned=*/true);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK(),
                                            /*is_application_error=*/true,
                                            "User exception: boom"));
  ASSERT_EQ(notified_states.size(), 1u);
  // Mirror ActorManager's dispatch of the authored DEAD.
  submitter_.DisconnectActor(actor_id,
                             /*num_restarts=*/0,
                             /*dead=*/true,
                             notified_states[0].second.death_cause(),
                             /*is_restartable=*/false);

  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();
  EXPECT_EQ(notified_states.size(), 1u);
  EXPECT_TRUE(raylet_client_->kill_local_requests.empty());
}

TEST_F(OwnerManagedActorTest, OutOfScopeKillRetriesTransientFailure) {
  // A transient transport failure of the kill must not leak a live actor:
  // the kill is retried.
  ActorID actor_id = ActorID::Of(JobID::FromInt(5), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();
  ASSERT_EQ(raylet_client_->kill_local_requests.size(), 1u);
  ASSERT_TRUE(raylet_client_->ReplyKillLocalActor(Status::IOError("transient")));
  // The retry is scheduled with a backoff timer; run the io context until it
  // fires.
  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(200));
  ASSERT_EQ(raylet_client_->kill_local_requests.size(), 2u);
  ASSERT_TRUE(raylet_client_->ReplyKillLocalActor(Status::OK()));
}

TEST_F(OwnerManagedActorTest, WorkerDeathRestartsWithinBudget) {
  // Confirmed worker death restarts the actor: RESTARTING then a fresh
  // creation, then ALIVE with the bumped restart count.
  ActorID actor_id = ActorID::Of(JobID::FromInt(7), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  auto spec = BuildCreationTaskSpec(actor_id);
  spec.GetMutableMessage().mutable_actor_creation_task_spec()->set_max_actor_restarts(1);
  submitter_.SubmitActorCreationTask(spec);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(notified_states.size(), 1u);

  // The probe asks the actor node's raylet; the node is alive.
  rpc::GcsNodeAddressAndLiveness node_info;
  node_info.set_node_manager_address("127.0.0.1");
  node_info.set_node_manager_port(7000);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillRepeatedly(Return(node_info));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillRepeatedly(Return(false));

  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(rpc::GetWorkerLivenessReply::DEAD,
                                                     rpc::GetWorkerLivenessReply::EXIT));

  // RESTARTING authored, then the new lease request is granted and pushed.
  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::RESTARTING);
  EXPECT_EQ(notified_states[1].second.num_restarts(), 1u);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9999, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(notified_states.size(), 3u);
  EXPECT_EQ(notified_states[2].second.state(), rpc::ActorTableData::ALIVE);
  EXPECT_EQ(notified_states[2].second.num_restarts(), 1u);
  EXPECT_EQ(notified_states[2].second.address().port(), 9999);
}

TEST_F(OwnerManagedActorTest, WorkerDeathBeyondBudgetIsFinalDead) {
  // With the restart budget exhausted (max_restarts=0), a confirmed death is
  // final: the owner authors DEAD with the probe's cause.
  ActorID actor_id = ActorID::Of(JobID::FromInt(8), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  rpc::GcsNodeAddressAndLiveness node_info;
  node_info.set_node_manager_address("127.0.0.1");
  node_info.set_node_manager_port(7000);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillRepeatedly(Return(node_info));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillRepeatedly(Return(false));

  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(rpc::GetWorkerLivenessReply::DEAD,
                                                     rpc::GetWorkerLivenessReply::EXIT));

  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(notified_states[1].second.death_cause().actor_died_error_context().reason(),
            rpc::ActorDiedErrorContext::WORKER_DIED);
}

TEST_F(OwnerManagedActorTest, AliveProbeVerdictStopsProbing) {
  // A transient push failure against a live worker must not restart it.
  ActorID actor_id = ActorID::Of(JobID::FromInt(9), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  rpc::GcsNodeAddressAndLiveness node_info;
  node_info.set_node_manager_address("127.0.0.1");
  node_info.set_node_manager_port(7000);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillRepeatedly(Return(node_info));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillRepeatedly(Return(false));

  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(rpc::GetWorkerLivenessReply::ALIVE));
  EXPECT_EQ(notified_states.size(), 1u);
}

TEST_F(OwnerManagedActorTest, UnknownFromWrongRayletIsInconclusive) {
  // UNKNOWN answered by a different raylet incarnation than the granting one
  // is never a death verdict: the probe holds and re-probes; UNKNOWN from
  // the granting raylet (registered-then-evicted) is.
  ActorID actor_id = ActorID::Of(JobID::FromInt(10), TaskID::Nil(), 1);
  const NodeID actor_node = NodeID::FromRandom();
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), actor_node, NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  rpc::GcsNodeAddressAndLiveness node_info;
  node_info.set_node_manager_address("127.0.0.1");
  node_info.set_node_manager_port(7000);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillRepeatedly(Return(node_info));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillRepeatedly(Return(false));

  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  // Wrong raylet incarnation: inconclusive, no death authored.
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(
      rpc::GetWorkerLivenessReply::UNKNOWN,
      rpc::GetWorkerLivenessReply::GRADE_UNSPECIFIED,
      Status::OK(),
      NodeID::FromRandom()));
  EXPECT_EQ(notified_states.size(), 1u);
  // The re-probe fires on the backoff timer; the granting raylet answers
  // UNKNOWN, which is a verdict.
  io_context.restart();
  io_context.run_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(
      rpc::GetWorkerLivenessReply::UNKNOWN,
      rpc::GetWorkerLivenessReply::GRADE_UNSPECIFIED,
      Status::OK(),
      actor_node));
  ASSERT_GE(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::DEAD);
}

TEST_F(OwnerManagedActorTest, TerminateDuringRestartConverges) {
  // Out-of-scope racing a restart's in-flight push defers to the restart
  // callback and then kills the fresh worker (no leak, no death-cause
  // overwrite).
  ActorID actor_id = ActorID::Of(JobID::FromInt(11), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  auto spec = BuildCreationTaskSpec(actor_id);
  spec.GetMutableMessage().mutable_actor_creation_task_spec()->set_max_actor_restarts(1);
  submitter_.SubmitActorCreationTask(spec);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  rpc::GcsNodeAddressAndLiveness node_info;
  node_info.set_node_manager_address("127.0.0.1");
  node_info.set_node_manager_port(7000);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillRepeatedly(Return(node_info));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillRepeatedly(Return(false));
  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(rpc::GetWorkerLivenessReply::DEAD,
                                                     rpc::GetWorkerLivenessReply::EXIT));
  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::RESTARTING);
  // Restart lease granted; push is in flight when the handle goes away.
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9999, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();
  EXPECT_TRUE(raylet_client_->kill_local_requests.empty());

  ASSERT_TRUE(worker_client_->ReplyPushTask());
  // ALIVE(1) authored, then the deferred termination kills the new worker.
  ASSERT_EQ(raylet_client_->kill_local_requests.size(), 1u);
  ASSERT_TRUE(raylet_client_->ReplyKillLocalActor());
  ASSERT_EQ(notified_states.size(), 4u);
  EXPECT_EQ(notified_states[2].second.state(), rpc::ActorTableData::ALIVE);
  EXPECT_EQ(notified_states[3].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(notified_states[3].second.death_cause().actor_died_error_context().reason(),
            rpc::ActorDiedErrorContext::OUT_OF_SCOPE);
}

TEST_F(OwnerManagedActorTest, RayletCancelDuringRestartAuthorsFinalDead) {
  // A raylet-issued scheduling cancel of the restart lease (unschedulable,
  // runtime env failure) is not an owner cancel: the actor must converge to
  // a final DEAD instead of hanging in RESTARTING.
  ActorID actor_id = ActorID::Of(JobID::FromInt(12), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  auto spec = BuildCreationTaskSpec(actor_id);
  spec.GetMutableMessage().mutable_actor_creation_task_spec()->set_max_actor_restarts(1);
  submitter_.SubmitActorCreationTask(spec);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  rpc::GcsNodeAddressAndLiveness node_info;
  node_info.set_node_manager_address("127.0.0.1");
  node_info.set_node_manager_port(7000);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillRepeatedly(Return(node_info));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillRepeatedly(Return(false));
  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(rpc::GetWorkerLivenessReply::DEAD,
                                                     rpc::GetWorkerLivenessReply::EXIT));
  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::RESTARTING);

  ASSERT_TRUE(raylet_client_->ReplyCanceledWorkerLease(
      rpc::RequestWorkerLeaseReply::SCHEDULING_CANCELLED_UNSCHEDULABLE,
      "no feasible node"));
  ASSERT_EQ(notified_states.size(), 3u);
  EXPECT_EQ(notified_states[2].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(
      notified_states[2].second.death_cause().actor_died_error_context().error_message(),
      "no feasible node");
}

TEST_F(OwnerManagedActorTest, InitialPushTransportFailureAuthorsDead) {
  // A transport failure pushing the creation task fails the creation AND
  // authors DEAD: without it, queued method calls would wait for ALIVE
  // forever (there is no GCS to notice the death).
  ActorID actor_id = ActorID::Of(JobID::FromInt(13), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, FailPendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::IOError("worker gone")));

  ASSERT_EQ(notified_states.size(), 1u);
  EXPECT_EQ(notified_states[0].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(notified_states[0].second.death_cause().actor_died_error_context().reason(),
            rpc::ActorDiedErrorContext::WORKER_DIED);
}

TEST_F(OwnerManagedActorTest, GrantWinningCancelRaceDefersTermination) {
  // The handle drops while the lease request is in flight; the grant wins
  // the cancel race (push already sent). The termination must defer to push
  // completion instead of touching the non-terminable entry.
  ActorID actor_id = ActorID::Of(JobID::FromInt(14), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  // Drop the handle before any grant.
  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();
  // The grant wins the race against the in-flight cancel.
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  EXPECT_TRUE(raylet_client_->kill_local_requests.empty());

  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(raylet_client_->kill_local_requests.size(), 1u);
  ASSERT_TRUE(raylet_client_->ReplyKillLocalActor());
  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::DEAD);
}

TEST_F(OwnerManagedActorTest, RayletIntendedCancelWithoutOwnerCancelAuthorsDead) {
  // Raylets also send SCHEDULING_CANCELLED_INTENDED for cancels the owner
  // never issued (released placement group bundles after a GCS restart):
  // the owner must still author DEAD or method calls hang forever.
  ActorID actor_id = ActorID::Of(JobID::FromInt(15), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, MarkTaskNoRetry(_)).Times(1);
  EXPECT_CALL(*task_manager_, FailPendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  ASSERT_TRUE(raylet_client_->ReplyCanceledWorkerLease(
      rpc::RequestWorkerLeaseReply::SCHEDULING_CANCELLED_INTENDED, "bundle released"));

  ASSERT_EQ(notified_states.size(), 1u);
  EXPECT_EQ(notified_states[0].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(
      notified_states[0].second.death_cause().actor_died_error_context().error_message(),
      "bundle released");
}

TEST_F(OwnerManagedActorTest, OwnerCancelConvergedByRayletCancelAuthorsOneDead) {
  // An in-flight owner cancel converged by a raylet-issued cancel must yield
  // exactly one DEAD (from the terminate path), not a second one that
  // overwrites the death cause.
  ActorID actor_id = ActorID::Of(JobID::FromInt(16), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, MarkTaskNoRetry(_)).Times(1);
  EXPECT_CALL(*task_manager_, FailPendingTask(_, _, _, _)).Times(1);
  submitter_.SubmitActorCreationTask(BuildCreationTaskSpec(actor_id));
  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();
  ASSERT_TRUE(raylet_client_->ReplyCanceledWorkerLease(
      rpc::RequestWorkerLeaseReply::SCHEDULING_CANCELLED_RUNTIME_ENV_SETUP_FAILED,
      "env failed"));

  ASSERT_EQ(notified_states.size(), 1u);
  EXPECT_EQ(notified_states[0].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(notified_states[0].second.death_cause().actor_died_error_context().reason(),
            rpc::ActorDiedErrorContext::OUT_OF_SCOPE);
}

TEST_F(OwnerManagedActorTest, OutOfScopeRestartableActorResurrectsOnNewTask) {
  // An out-of-scope death with restart budget left keeps the actor
  // resurrectable: a later lineage-reconstruction task restarts it from the
  // owner (no GCS involved), doc §5.3.
  ActorID actor_id = ActorID::Of(JobID::FromInt(17), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      /*max_pending_calls=*/-1,
                                      /*allow_out_of_order_execution=*/false,
                                      /*fail_if_actor_unreachable=*/false,
                                      /*owned=*/true);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(testing::AnyNumber());
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  auto spec = BuildCreationTaskSpec(actor_id);
  spec.GetMutableMessage().mutable_actor_creation_task_spec()->set_max_actor_restarts(-1);
  submitter_.SubmitActorCreationTask(spec);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  // A downstream task's lineage references the actor handle (this is what
  // makes resurrection reachable in production: without lineage the retained
  // spec is dropped as soon as the reference is fully deleted).
  reference_counter_->UpdateSubmittedTaskReferences(
      /*return_ids=*/{}, {ObjectID::ForActorHandle(actor_id)});
  // The task finished: its live reference goes away but the lineage
  // reference stays pinned.
  reference_counter_->UpdateFinishedTaskReferences(
      /*return_ids=*/{},
      {ObjectID::ForActorHandle(actor_id)},
      /*release_lineage=*/false,
      rpc::Address(),
      ::google::protobuf::RepeatedPtrField<rpc::ObjectReferenceCount>(),
      nullptr);

  // Out of scope: killed, but the DEAD carries restartability.
  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();
  ASSERT_EQ(raylet_client_->kill_local_requests.size(), 1u);
  ASSERT_TRUE(raylet_client_->ReplyKillLocalActor());
  ASSERT_EQ(notified_states.size(), 2u);
  const auto &dead_state = notified_states[1].second;
  EXPECT_EQ(dead_state.state(), rpc::ActorTableData::DEAD);
  EXPECT_TRUE(gcs::IsActorRestartable(dead_state));
  // Mirror the production dispatch of the DEAD.
  submitter_.DisconnectActor(actor_id,
                             /*num_restarts=*/0,
                             /*dead=*/true,
                             dead_state.death_cause(),
                             /*is_restartable=*/true);

  // A lineage reconstruction task arrives; production restores a reference
  // to the handle before resubmitting (task_manager resubmission), mirrored
  // here so the re-armed termination has something to watch.
  reference_counter_->AddLocalReference(ObjectID::ForActorHandle(actor_id), "test");
  auto task = CreateActorTaskHelper(actor_id, WorkerID::FromRandom(), 0);
  submitter_.SubmitTask(task);
  Pump();
  ASSERT_EQ(notified_states.size(), 3u);
  EXPECT_EQ(notified_states[2].second.state(), rpc::ActorTableData::RESTARTING);
  EXPECT_EQ(notified_states[2].second.num_restarts(), 1u);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9999, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  Pump();
  ASSERT_EQ(notified_states.size(), 4u);
  EXPECT_EQ(notified_states[3].second.state(), rpc::ActorTableData::ALIVE);
  EXPECT_EQ(notified_states[3].second.num_restarts(), 1u);
  // The re-armed termination watches the restored reference; it must not
  // have fired.
  EXPECT_EQ(raylet_client_->kill_local_requests.size(), 1u);

  // Second out-of-scope cycle: the re-armed termination kills the
  // resurrected worker and the DEAD stays restartable (max_restarts=-1).
  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();
  ASSERT_EQ(raylet_client_->kill_local_requests.size(), 2u);
  ASSERT_TRUE(raylet_client_->ReplyKillLocalActor());
  ASSERT_EQ(notified_states.size(), 5u);
  EXPECT_EQ(notified_states[4].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(notified_states[4].second.num_restarts(), 1u);
  EXPECT_TRUE(gcs::IsActorRestartable(notified_states[4].second));
}

TEST_F(OwnerManagedActorTest, PreemptedNodeDeathDoesNotConsumeBudget) {
  // A drain-preempted node death restarts the actor without consuming the
  // max_restarts budget (GCS RestartActor semantics): with max_restarts=1,
  // one preemption restart + one failure restart succeed, the next failure
  // is final.
  ActorID actor_id = ActorID::Of(JobID::FromInt(18), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  auto spec = BuildCreationTaskSpec(actor_id);
  spec.GetMutableMessage().mutable_actor_creation_task_spec()->set_max_actor_restarts(1);
  submitter_.SubmitActorCreationTask(spec);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  // First death: the node was drain-preempted.
  rpc::GcsNodeAddressAndLiveness preempted_node;
  preempted_node.set_node_manager_address("127.0.0.1");
  preempted_node.set_node_manager_port(7000);
  preempted_node.mutable_death_info()->set_reason(
      rpc::NodeDeathInfo::AUTOSCALER_DRAIN_PREEMPTED);
  rpc::GcsNodeAddressAndLiveness alive_node;
  alive_node.set_node_manager_address("127.0.0.1");
  alive_node.set_node_manager_port(7000);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillOnce(Return(preempted_node))
      .WillRepeatedly(Return(alive_node));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillOnce(Return(true))
      .WillRepeatedly(Return(false));

  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::RESTARTING);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9999, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(notified_states.size(), 3u);
  EXPECT_EQ(notified_states[2].second.state(), rpc::ActorTableData::ALIVE);

  // Second death (worker failure): the budget was not consumed by the
  // preemption, so this restart still fits max_restarts=1.
  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(rpc::GetWorkerLivenessReply::DEAD,
                                                     rpc::GetWorkerLivenessReply::EXIT));
  ASSERT_EQ(notified_states.size(), 4u);
  EXPECT_EQ(notified_states[3].second.state(), rpc::ActorTableData::RESTARTING);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9997, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(notified_states.size(), 5u);
  EXPECT_EQ(notified_states[4].second.state(), rpc::ActorTableData::ALIVE);

  // Third death: the budget is now truly exhausted.
  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(rpc::GetWorkerLivenessReply::DEAD,
                                                     rpc::GetWorkerLivenessReply::EXIT));
  ASSERT_EQ(notified_states.size(), 6u);
  EXPECT_EQ(notified_states[5].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(notified_states[5].second.num_restarts(), 2u);
  EXPECT_EQ(notified_states[5].second.num_restarts_due_to_node_preemption(), 1u);
}

TEST_F(OwnerManagedActorTest, PreemptionRestartsBeyondExhaustedBudget) {
  // With max_restarts=1 already exhausted by a failure restart, a
  // drain-preempted node death still restarts the actor (the
  // max_restarts>0 && preempted clause).
  ActorID actor_id = ActorID::Of(JobID::FromInt(19), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  auto spec = BuildCreationTaskSpec(actor_id);
  spec.GetMutableMessage().mutable_actor_creation_task_spec()->set_max_actor_restarts(1);
  submitter_.SubmitActorCreationTask(spec);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  rpc::GcsNodeAddressAndLiveness alive_node;
  alive_node.set_node_manager_address("127.0.0.1");
  alive_node.set_node_manager_port(7000);
  rpc::GcsNodeAddressAndLiveness preempted_node = alive_node;
  preempted_node.mutable_death_info()->set_reason(
      rpc::NodeDeathInfo::AUTOSCALER_DRAIN_PREEMPTED);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillOnce(Return(alive_node))
      .WillRepeatedly(Return(preempted_node));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillOnce(Return(false))
      .WillRepeatedly(Return(true));

  // Failure death exhausts the budget.
  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(rpc::GetWorkerLivenessReply::DEAD,
                                                     rpc::GetWorkerLivenessReply::EXIT));
  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::RESTARTING);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9999, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(notified_states.size(), 3u);

  // Preemption death: only the exemption clause can allow this restart.
  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_EQ(notified_states.size(), 4u);
  EXPECT_EQ(notified_states[3].second.state(), rpc::ActorTableData::RESTARTING);
  EXPECT_EQ(notified_states[3].second.num_restarts(), 2u);
}

TEST_F(OwnerManagedActorTest, PreemptionCountSurvivesResurrection) {
  // The preemption exemption must survive the out-of-scope →
  // resurrection round trip: the DEAD carries both counters and the
  // resurrected actor's budget still excludes the preemption restart.
  ActorID actor_id = ActorID::Of(JobID::FromInt(20), TaskID::Nil(), 1);
  OwnHandle(actor_id);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      /*max_pending_calls=*/-1,
                                      /*allow_out_of_order_execution=*/false,
                                      /*fail_if_actor_unreachable=*/false,
                                      /*owned=*/true);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(testing::AnyNumber());
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  auto spec = BuildCreationTaskSpec(actor_id);
  spec.GetMutableMessage().mutable_actor_creation_task_spec()->set_max_actor_restarts(1);
  submitter_.SubmitActorCreationTask(spec);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());

  // Preemption death and restart: counts {1,1}.
  rpc::GcsNodeAddressAndLiveness preempted_node;
  preempted_node.set_node_manager_address("127.0.0.1");
  preempted_node.set_node_manager_port(7000);
  preempted_node.mutable_death_info()->set_reason(
      rpc::NodeDeathInfo::AUTOSCALER_DRAIN_PREEMPTED);
  rpc::GcsNodeAddressAndLiveness alive_node;
  alive_node.set_node_manager_address("127.0.0.1");
  alive_node.set_node_manager_port(7000);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillOnce(Return(preempted_node))
      .WillRepeatedly(Return(alive_node));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillOnce(Return(true))
      .WillRepeatedly(Return(false));
  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9999, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(notified_states.size(), 3u);

  // Lineage pin, then out of scope: the DEAD carries both counters.
  reference_counter_->UpdateSubmittedTaskReferences(
      /*return_ids=*/{}, {ObjectID::ForActorHandle(actor_id)});
  reference_counter_->UpdateFinishedTaskReferences(
      /*return_ids=*/{},
      {ObjectID::ForActorHandle(actor_id)},
      /*release_lineage=*/false,
      rpc::Address(),
      ::google::protobuf::RepeatedPtrField<rpc::ObjectReferenceCount>(),
      nullptr);
  reference_counter_->RemoveLocalReference(ObjectID::ForActorHandle(actor_id), nullptr);
  Pump();
  ASSERT_TRUE(raylet_client_->ReplyKillLocalActor());
  ASSERT_EQ(notified_states.size(), 4u);
  const auto &dead_state = notified_states[3].second;
  EXPECT_EQ(dead_state.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(dead_state.num_restarts(), 1u);
  EXPECT_EQ(dead_state.num_restarts_due_to_node_preemption(), 1u);
  EXPECT_TRUE(gcs::IsActorRestartable(dead_state));
  submitter_.DisconnectActor(actor_id,
                             /*num_restarts=*/1,
                             /*dead=*/true,
                             dead_state.death_cause(),
                             /*is_restartable=*/true);

  // Resurrect; the exemption carried through (effective = 2-1 = 1 = max
  // would forbid, so budget math must see the preserved preemption count).
  reference_counter_->AddLocalReference(ObjectID::ForActorHandle(actor_id), "test");
  submitter_.SubmitTask(CreateActorTaskHelper(actor_id, WorkerID::FromRandom(), 0));
  Pump();
  ASSERT_EQ(notified_states.size(), 5u);
  EXPECT_EQ(notified_states[4].second.state(), rpc::ActorTableData::RESTARTING);
  EXPECT_EQ(notified_states[4].second.num_restarts(), 2u);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9997, WorkerID::FromRandom(), NodeID::FromRandom(), NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  Pump();
  ASSERT_EQ(notified_states.size(), 6u);
  EXPECT_EQ(notified_states[5].second.state(), rpc::ActorTableData::ALIVE);

  // Final failure death: effective = 2-1 = 1 = max, budget truly gone.
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillRepeatedly(Return(preempted_node));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillRepeatedly(Return(false));
  submitter_.MaybeStartOwnerManagedLivenessProbe(actor_id);
  ASSERT_TRUE(raylet_client_->ReplyGetWorkerLiveness(rpc::GetWorkerLivenessReply::DEAD,
                                                     rpc::GetWorkerLivenessReply::EXIT));
  ASSERT_EQ(notified_states.size(), 7u);
  EXPECT_EQ(notified_states[6].second.state(), rpc::ActorTableData::DEAD);
  EXPECT_EQ(notified_states[6].second.num_restarts(), 2u);
  EXPECT_EQ(notified_states[6].second.num_restarts_due_to_node_preemption(), 1u);
}

TEST_F(OwnerManagedActorTest, NodeDeathProbesActorsOnNode) {
  // A node death must reach idle actors (no push in flight, no raylet to
  // notify): every owner-managed actor granted on the node gets probed.
  ActorID actor_id = ActorID::Of(JobID::FromInt(22), TaskID::Nil(), 1);
  const NodeID actor_node = NodeID::FromRandom();
  OwnHandle(actor_id);
  EXPECT_CALL(*task_manager_, MarkDependenciesResolved(_)).Times(1);
  EXPECT_CALL(*task_manager_, CompletePendingTask(_, _, _, _)).Times(1);
  auto spec = BuildCreationTaskSpec(actor_id);
  spec.GetMutableMessage().mutable_actor_creation_task_spec()->set_max_actor_restarts(1);
  submitter_.SubmitActorCreationTask(spec);
  ASSERT_TRUE(raylet_client_->GrantWorkerLease(
      "127.0.0.1", 9998, WorkerID::FromRandom(), actor_node, NodeID::Nil()));
  ASSERT_TRUE(worker_client_->ReplyPushTask());
  ASSERT_EQ(notified_states.size(), 1u);

  rpc::GcsNodeAddressAndLiveness dead_node;
  dead_node.set_node_manager_address("127.0.0.1");
  dead_node.set_node_manager_port(7000);
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, GetNodeAddressAndLiveness(_, _))
      .WillRepeatedly(Return(dead_node));
  EXPECT_CALL(*mock_gcs_client_->mock_node_accessor, IsNodeDead(_))
      .WillRepeatedly(Return(true));

  // An unrelated node death while the actor is alive probes nothing: this
  // is the assertion that actually discriminates the node filter.
  submitter_.HandleNodeDead(NodeID::FromRandom());
  Pump();
  ASSERT_EQ(notified_states.size(), 1u);

  submitter_.HandleNodeDead(actor_node);
  Pump();
  ASSERT_EQ(notified_states.size(), 2u);
  EXPECT_EQ(notified_states[1].second.state(), rpc::ActorTableData::RESTARTING);
}

TEST_F(OwnerManagedActorTest, ProbeOnUnknownActorIsNoOp) {
  // A death notification for an actor this owner does not manage (e.g. a
  // GCS-managed actor) must be a harmless no-op.
  submitter_.MaybeStartOwnerManagedLivenessProbe(
      ActorID::Of(JobID::FromInt(23), TaskID::Nil(), 1));
  Pump();
  EXPECT_TRUE(notified_states.empty());
}

INSTANTIATE_TEST_SUITE_P(AllowOutOfOrderExecution,
                         ActorTaskSubmitterTest,
                         ::testing::Values(true, false));

}  // namespace ray::core
