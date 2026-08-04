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

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "ray/asio/instrumented_io_context.h"
#include "ray/common/id.h"
#include "ray/common/lease/lease_spec.h"
#include "ray/common/status.h"
#include "ray/common/task/task_spec.h"
#include "ray/core_worker/lease_policy.h"
#include "ray/core_worker_rpc_client/core_worker_client_pool.h"
#include "ray/raylet_rpc_client/raylet_client_pool.h"
#include "ray/util/thread_checker.h"

namespace ray {
namespace core {

/// Owner-side submitter for actor creation tasks. Unlike the pooled
/// NormalTaskSubmitter, every actor holds its own lease permanently: the lease
/// is never returned, never expires, and its worker is never reused for other
/// work. The wire protocol is the normal-task lease path (RequestWorkerLease
/// with spillback), followed by pushing the creation task to the granted
/// worker. Because leases never expire, a lost lease reply must never abandon
/// the lease: transport failures are retried with the same lease id, which
/// the raylet answers idempotently.
///
/// Thread-unsafe: all methods and callbacks run on the owning io_context
/// thread, like the sibling submitters; the submitter must outlive its
/// in-flight RPCs (it is owned by the CoreWorker alongside the io_context).
class ActorCreationSubmitter {
 public:
  struct CreationResult {
    Status status;
    /// The granted actor worker's address. Set on OK status.
    rpc::Address actor_address;
    /// The creation task's reply from the actor worker (borrowed refs,
    /// application error). Set when the push completed, OK or not.
    rpc::PushTaskReply push_task_reply;
    /// The lease this actor holds for its lifetime.
    LeaseID lease_id;
    /// On cancellation by the raylet, why scheduling failed.
    rpc::RequestWorkerLeaseReply::SchedulingFailureType failure_type =
        rpc::RequestWorkerLeaseReply::NOT_FAILED;
    std::string scheduling_failure_message;
  };
  using CreationCallback = std::function<void(const CreationResult &result)>;

  ActorCreationSubmitter(
      rpc::Address owner_address,
      std::shared_ptr<rpc::RayletClientPool> raylet_client_pool,
      std::shared_ptr<rpc::CoreWorkerClientPool> core_worker_client_pool,
      instrumented_io_context &io_service,
      std::unique_ptr<LeasePolicyInterface> lease_policy = nullptr,
      int64_t retry_backoff_ms = kDefaultRetryBackoffMs,
      std::function<bool(const NodeID &)> is_node_dead = nullptr)
      : owner_address_(std::move(owner_address)),
        owner_worker_id_(WorkerID::FromBinary(owner_address_.worker_id())),
        raylet_client_pool_(std::move(raylet_client_pool)),
        core_worker_client_pool_(std::move(core_worker_client_pool)),
        io_service_(io_service),
        lease_policy_(std::move(lease_policy)),
        retry_backoff_ms_(retry_backoff_ms),
        is_node_dead_(std::move(is_node_dead)) {}

  static constexpr int64_t kDefaultRetryBackoffMs = 100;

  /// Lease a worker for this creation task starting at the given raylet and
  /// push the creation task once granted. The callback fires exactly once,
  /// with OK and the actor address, or with the failure.
  void SubmitCreation(const TaskSpecification &creation_spec,
                      const rpc::Address &start_raylet_address,
                      CreationCallback callback);

  /// As above, with the starting raylet chosen by the injected lease policy
  /// (locality-aware when constructed with one).
  void SubmitCreation(const TaskSpecification &creation_spec, CreationCallback callback);

  /// Cancel a creation that has not been granted yet, looping the cancel
  /// until it converges. When a creation ends cancelled, cancel callbacks
  /// fire before the creation callback. Convergence: a cancel racing ahead of the lease
  /// request is retried, and a grant that wins the race flips the answer to false so the
  /// caller can take the kill path instead. The callback receives true iff no worker was
  /// (or will be) granted for this creation.
  void CancelCreation(const ActorID &actor_id,
                      std::function<void(bool cancelled)> callback);

  /// Forget a terminated actor, releasing its entry so the actor id can be
  /// resubmitted (resurrection re-runs the full creation path).
  void OnActorTerminated(const ActorID &actor_id);

  /// The permanently held lease for an actor, if granted.
  std::optional<LeaseID> GetGrantedLease(const ActorID &actor_id) const;

  /// The granted actor worker's address, once a worker was granted.
  std::optional<rpc::Address> GetActorAddress(const ActorID &actor_id) const;

  /// True while the creation task push has not completed yet: a worker was
  /// granted but the creation callback has not fired, so termination must
  /// wait for it (the push-completion is the cancel/kill convergence point).
  bool IsCreationPushInFlight(const ActorID &actor_id) const;

 private:
  enum class CreationState {
    kPendingLease,
    kPushing,
    kAlive,
  };

  struct CreationEntry {
    CreationState state = CreationState::kPendingLease;
    TaskSpecification spec;
    LeaseID lease_id;
    /// First-hop raylet: rejected spillbacks retry here, where the resource
    /// view refreshes.
    rpc::Address first_raylet_address;
    rpc::Address current_raylet_address;
    rpc::Address actor_address;
    std::vector<rpc::ResourceMapEntry> resource_mapping;
    CreationCallback callback;
    bool cancel_requested = false;
    std::vector<std::function<void(bool)>> cancel_callbacks;
  };

  void RequestLease(const ActorID &actor_id,
                    const rpc::Address &raylet_address,
                    bool is_spillback,
                    bool reuse_lease_id);
  void PushCreationTask(const ActorID &actor_id);
  void IssueCancel(const ActorID &actor_id);
  void RetryAfterBackoff(std::function<void()> fn);
  /// Complete the creation as cancelled: fire cancel callbacks (true) and the
  /// creation callback, then erase the entry.
  void CompleteCancelled(const ActorID &actor_id,
                         rpc::RequestWorkerLeaseReply::SchedulingFailureType failure_type,
                         const std::string &failure_message);

  const rpc::Address owner_address_;
  const WorkerID owner_worker_id_;
  std::shared_ptr<rpc::RayletClientPool> raylet_client_pool_;
  std::shared_ptr<rpc::CoreWorkerClientPool> core_worker_client_pool_;
  instrumented_io_context &io_service_;
  std::unique_ptr<LeasePolicyInterface> lease_policy_;
  absl::flat_hash_map<ActorID, CreationEntry> creations_;
  ThreadChecker thread_checker_;
  const int64_t retry_backoff_ms_;
  /// Terminal node-death oracle; a dead raylet's lease (and any grant) died
  /// with it, so the creation restarts from the first hop. Nullptr disables
  /// the check (transport failures then retry the same address forever).
  std::function<bool(const NodeID &)> is_node_dead_;
};

}  // namespace core
}  // namespace ray
