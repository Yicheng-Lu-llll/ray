// Copyright 2017 The Ray Authors.
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

#include <memory>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "ray/gcs_rpc_client/accessors/actor_info_accessor_interface.h"
#include "ray/util/thread_utils.h"

namespace ray {
namespace core {

class ActorCreatorInterface {
 public:
  virtual ~ActorCreatorInterface() = default;
  /// Register actor to GCS synchronously.
  ///
  /// \param task_spec The specification for the actor creation task.
  /// \return Status
  virtual Status RegisterActor(const TaskSpecification &task_spec) const = 0;

  /// Asynchronously request GCS to register the actor.
  /// \param task_spec The specification for the actor creation task.
  /// \param callback Callback that will be called after the actor info is registered to
  /// GCS
  virtual void AsyncRegisterActor(const TaskSpecification &task_spec,
                                  rpc::StatusCallback callback) = 0;

  virtual void AsyncRestartActorForLineageReconstruction(
      const ActorID &actor_id,
      uint64_t num_restarts_due_to_lineage_reconstructions,
      rpc::StatusCallback callback) = 0;

  virtual void AsyncReportActorOutOfScope(
      const ActorID &actor_id,
      uint64_t num_restarts_due_to_lineage_reconstructions,
      rpc::StatusCallback callback) = 0;

  /// Asynchronously request GCS to create the actor.
  ///
  /// \param task_spec The specification for the actor creation task.
  /// \param callback Callback that will be called after the actor info is written to GCS.
  virtual void AsyncCreateActor(
      const TaskSpecification &task_spec,
      const rpc::ClientCallback<rpc::CreateActorReply> &callback) = 0;

  /// Asynchronously wait until actor is registered successfully.
  ///
  /// If the actor is pending lazy registration (see
  /// MarkPendingLazyRegistration), this triggers the actual registration: the
  /// caller is the first escape of the actor's handle out of the owner, so the
  /// GCS must be told about the actor before the wait can resolve.
  ///
  /// \param actor_id The actor id to wait
  /// \param callback The callback that will be called after actor registered
  virtual void AsyncWaitForActorRegisterFinish(const ActorID &actor_id,
                                               rpc::StatusCallback callback) = 0;

  /// Check whether the GCS is not yet guaranteed to know this actor: a
  /// registration is in flight, or the actor is pending lazy registration.
  /// Callers that see true must wait via AsyncWaitForActorRegisterFinish
  /// before letting the actor's handle escape the owner.
  ///
  /// \param actor_id The actor id to check
  /// \return bool Boolean to indicate whether the actor is under registering
  virtual bool IsActorInRegistering(const ActorID &actor_id) const = 0;

  /// Lazy registration: remember this actor as
  /// submitted-but-not-registered. From this point IsActorInRegistering
  /// reports true, AsyncWaitForActorRegisterFinish triggers the actual
  /// registration (the first escape registers), and AsyncCreateActor folds the
  /// registration into the create request (register_if_absent) if nothing
  /// escaped first. Default no-op: fakes treat every actor as instantly
  /// registered.
  ///
  /// \param task_spec The specification for the actor creation task.
  virtual void MarkPendingLazyRegistration(const TaskSpecification &task_spec) {}

  /// Lazy registration: note that the actor died on the owner (ray.kill, or
  /// the handle went out of scope) while the GCS-side creation had not
  /// completed. A later AsyncCreateActor for a tombstoned actor is not sent at
  /// all -- for a kill the GCS already destroyed the actor and a create sent
  /// afterwards would be rejected, or worse, resurrect the actor once the GCS
  /// dead-actor cache evicts the entry; for an out-of-scope handle the GCS
  /// never has to learn about the actor at all. Default no-op: fakes treat
  /// every actor as instantly registered.
  ///
  /// \return Whether the actor was tombstoned, i.e. the GCS may not have
  /// finished creating it (registration pending or in flight).
  virtual bool MarkDeadBeforeCreate(const ActorID &actor_id) { return false; }

  /// Lazy registration: a WaitForActorRefDeleted poll arrived from the GCS.
  /// Such a poll can only originate from a completed registration, so the GCS
  /// provably knows this actor: drop any pending-lazy entry so neither the
  /// poll handler nor a later escape re-sends the registration. Default
  /// no-op.
  virtual void OnRegistrationConfirmedByGcs(const ActorID &actor_id) {}

  /// Lazy registration: the creation task failed before AsyncCreateActor
  /// could run (dependency resolution failed), so no create will consume the
  /// retained spec or tombstone; drop both. Default no-op.
  virtual void ClearPendingLazyState(const ActorID &actor_id) {}
};

class ActorCreator : public ActorCreatorInterface {
 public:
  explicit ActorCreator(gcs::ActorInfoAccessorInterface &actor_client)
      : actor_client_(actor_client) {}

  Status RegisterActor(const TaskSpecification &task_spec) const override;

  void AsyncRegisterActor(const TaskSpecification &task_spec,
                          rpc::StatusCallback callback) override;

  void AsyncRestartActorForLineageReconstruction(
      const ActorID &actor_id,
      uint64_t num_restarts_due_to_lineage_reconstructions,
      rpc::StatusCallback callback) override;

  void AsyncReportActorOutOfScope(const ActorID &actor_id,
                                  uint64_t num_restarts_due_to_lineage_reconstruction,
                                  rpc::StatusCallback callback) override;

  bool IsActorInRegistering(const ActorID &actor_id) const override;

  void AsyncWaitForActorRegisterFinish(const ActorID &actor_id,
                                       rpc::StatusCallback callback) override;

  void AsyncCreateActor(
      const TaskSpecification &task_spec,
      const rpc::ClientCallback<rpc::CreateActorReply> &callback) override;

  void MarkPendingLazyRegistration(const TaskSpecification &task_spec) override;

  bool MarkDeadBeforeCreate(const ActorID &actor_id) override;

  void OnRegistrationConfirmedByGcs(const ActorID &actor_id) override;

  void ClearPendingLazyState(const ActorID &actor_id) override;

 private:
  gcs::ActorInfoAccessorInterface &actor_client_;
  using RegisteringActorType =
      absl::flat_hash_map<ActorID, std::vector<rpc::StatusCallback>>;
  ThreadPrivate<RegisteringActorType> registering_actors_;
  /// Specs of anonymous actors submitted without an eager RegisterActor RPC
  /// (lazy registration). Entries leave this map either at the first
  /// escape of the actor's handle (AsyncWaitForActorRegisterFinish sends the
  /// standalone registration) or at the create reply (the create request
  /// carries the registration via register_if_absent; the entry stays alive
  /// over the RPC so escapes racing the create still trigger a fast standalone
  /// registration instead of waiting out the whole creation). If the creation
  /// task fails dependency resolution, the entry is intentionally kept (a
  /// later escape must still trigger the registration) at the cost of one
  /// retained spec per failed creation.
  ThreadPrivate<absl::flat_hash_map<ActorID, TaskSpecification>> lazy_pending_actors_;
  /// Actors that died on the owner (kill or out of scope) while their
  /// GCS-side creation had not completed (see MarkDeadBeforeCreate). Also
  /// covers eagerly-registering actors whose registration is in flight.
  /// Consulted and consumed by AsyncCreateActor, or dropped by
  /// ClearPendingLazyState when the creation task fails first.
  ThreadPrivate<absl::flat_hash_set<ActorID>> dead_before_create_actors_;
};

}  // namespace core
}  // namespace ray
