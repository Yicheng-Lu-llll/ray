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
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "ray/common/id.h"
#include "ray/common/lease/lease_spec.h"
#include "ray/common/task/task_spec.h"
#include "ray/core_worker_rpc_client/core_worker_client_pool.h"
#include "ray/raylet_rpc_client/raylet_client_pool.h"

namespace ray {
namespace core {

/// Owner-side submitter for actor creation tasks. Unlike the pooled
/// NormalTaskSubmitter, every actor holds its own lease permanently: the lease
/// is never returned, never expires, and its worker is never reused for other
/// work. The wire protocol is the normal-task lease path (RequestWorkerLease
/// with spillback), followed by pushing the creation task to the granted
/// worker.
///
/// Thread-unsafe: all methods must run on the owning io_context thread, like
/// the sibling submitters.
class ActorCreationSubmitter {
 public:
  struct CreationResult {
    Status status;
    /// The granted actor worker's address. Set on OK status.
    rpc::Address actor_address;
    /// The lease this actor holds for its lifetime.
    LeaseID lease_id;
  };
  using CreationCallback = std::function<void(const CreationResult &result)>;

  ActorCreationSubmitter(
      rpc::Address owner_address,
      std::shared_ptr<rpc::RayletClientPool> raylet_client_pool,
      std::shared_ptr<rpc::CoreWorkerClientPool> core_worker_client_pool)
      : owner_address_(std::move(owner_address)),
        owner_worker_id_(WorkerID::FromBinary(owner_address_.worker_id())),
        raylet_client_pool_(std::move(raylet_client_pool)),
        core_worker_client_pool_(std::move(core_worker_client_pool)) {}

  /// Lease a worker for this creation task starting at the given raylet and
  /// push the creation task once granted. The callback fires exactly once,
  /// with OK and the actor address, or with the failure.
  void SubmitCreation(const TaskSpecification &creation_spec,
                      const rpc::Address &start_raylet_address,
                      CreationCallback callback);

  /// Cancel a creation that has not been granted yet, looping the cancel until
  /// it converges: a cancel racing ahead of the lease request is retried, and
  /// a grant that wins the race flips the answer to false so the caller can
  /// take the kill path instead. The callback receives true iff the creation
  /// was cancelled before a worker was granted.
  void CancelCreation(const ActorID &actor_id,
                      std::function<void(bool cancelled)> callback);

  /// The permanently held lease for an actor, if granted.
  std::optional<LeaseID> GetGrantedLease(const ActorID &actor_id) const;

 private:
  enum class CreationState {
    kPendingLease,
    kPushing,
    kAlive,
    kCancelled,
  };

  struct CreationEntry {
    CreationState state = CreationState::kPendingLease;
    TaskSpecification spec;
    LeaseID lease_id;
    rpc::Address current_raylet_address;
    rpc::Address actor_address;
    CreationCallback callback;
    bool cancel_requested = false;
    std::function<void(bool)> cancel_callback;
  };

  void RequestLease(const ActorID &actor_id,
                    const rpc::Address &raylet_address,
                    bool is_spillback);
  void PushCreationTask(const ActorID &actor_id);
  void IssueCancel(const ActorID &actor_id);

  const rpc::Address owner_address_;
  const WorkerID owner_worker_id_;
  std::shared_ptr<rpc::RayletClientPool> raylet_client_pool_;
  std::shared_ptr<rpc::CoreWorkerClientPool> core_worker_client_pool_;
  absl::flat_hash_map<ActorID, CreationEntry> creations_;
  uint64_t lease_id_counter_ = 0;
};

}  // namespace core
}  // namespace ray
