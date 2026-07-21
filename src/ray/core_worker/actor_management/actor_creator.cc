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

#include "ray/core_worker/actor_management/actor_creator.h"

#include <memory>
#include <utility>
#include <vector>

namespace ray {
namespace core {

Status ActorCreator::RegisterActor(const TaskSpecification &task_spec) const {
  const auto status = actor_client_.SyncRegisterActor(task_spec);
  if (status.IsTimedOut()) {
    std::ostringstream stream;
    stream << "There was timeout in registering an actor. It is probably "
              "because GCS server is dead or there's a high load there.";
    return Status::TimedOut(stream.str());
  }
  return status;
}

void ActorCreator::AsyncRegisterActor(const TaskSpecification &task_spec,
                                      rpc::StatusCallback callback) {
  auto actor_id = task_spec.ActorCreationId();
  (*registering_actors_)[actor_id] = {};
  if (callback != nullptr) {
    (*registering_actors_)[actor_id].emplace_back(std::move(callback));
  }
  actor_client_.AsyncRegisterActor(task_spec, [actor_id, this](Status status) {
    std::vector<rpc::StatusCallback> cbs;
    cbs = std::move((*registering_actors_)[actor_id]);
    registering_actors_->erase(actor_id);
    for (auto &cb : cbs) {
      cb(status);
    }
  });
}

void ActorCreator::AsyncRestartActorForLineageReconstruction(
    const ActorID &actor_id,
    uint64_t num_restarts_due_to_lineage_reconstructions,
    rpc::StatusCallback callback) {
  actor_client_.AsyncRestartActorForLineageReconstruction(
      actor_id, num_restarts_due_to_lineage_reconstructions, callback);
}

void ActorCreator::AsyncReportActorOutOfScope(
    const ActorID &actor_id,
    uint64_t num_restarts_due_to_lineage_reconstruction,
    rpc::StatusCallback callback) {
  actor_client_.AsyncReportActorOutOfScope(
      actor_id, num_restarts_due_to_lineage_reconstruction, callback);
}

bool ActorCreator::IsActorInRegistering(const ActorID &actor_id) const {
  return registering_actors_->find(actor_id) != registering_actors_->end() ||
         lazy_pending_actors_->find(actor_id) != lazy_pending_actors_->end();
}

void ActorCreator::MarkPendingLazyRegistration(const TaskSpecification &task_spec) {
  (*lazy_pending_actors_)[task_spec.ActorCreationId()] = task_spec;
}

void ActorCreator::AsyncWaitForActorRegisterFinish(const ActorID &actor_id,
                                                   rpc::StatusCallback callback) {
  auto lazy_iter = lazy_pending_actors_->find(actor_id);
  if (lazy_iter != lazy_pending_actors_->end()) {
    // First escape of a lazily-registered actor's handle out of the owner:
    // someone else is about to learn about this actor, so the GCS must be told
    // first. AsyncRegisterActor moves the actor into registering_actors_ and
    // resolves the callback when the registration lands.
    TaskSpecification task_spec = std::move(lazy_iter->second);
    lazy_pending_actors_->erase(lazy_iter);
    AsyncRegisterActor(task_spec, std::move(callback));
    return;
  }
  auto iter = registering_actors_->find(actor_id);
  RAY_CHECK(iter != registering_actors_->end());
  iter->second.emplace_back(std::move(callback));
}

void ActorCreator::AsyncCreateActor(
    const TaskSpecification &task_spec,
    const rpc::ClientCallback<rpc::CreateActorReply> &callback) {
  const auto actor_id = task_spec.ActorCreationId();
  if (dead_before_create_actors_->erase(actor_id) > 0) {
    // The actor died on the owner (kill, or its handle went out of scope)
    // while its GCS-side creation had not completed. For a kill the GCS
    // already destroyed the actor and a create sent now would be rejected --
    // or resurrect it once the GCS dead-actor cache evicts the entry; for an
    // out-of-scope handle the pending entry is still held here (nothing
    // escaped), so this branch also frees the retained spec. Fail the
    // creation task locally the same way a GCS-side cancellation would,
    // including the death cause.
    lazy_pending_actors_->erase(actor_id);
    rpc::CreateActorReply reply;
    auto *context = reply.mutable_death_cause()->mutable_actor_died_error_context();
    context->set_reason(rpc::ActorDiedErrorContext::RAY_KILL);
    context->set_error_message(
        "The actor died on its owner before its creation completed.");
    context->set_actor_id(actor_id.Binary());
    callback(Status::SchedulingCancelled("Actor died before creation."),
             std::move(reply));
    return;
  }
  // Lazy registration: the pending entry stays alive until the create reply,
  // so an escape of the handle while the create RPC is in flight still
  // triggers a standalone (idempotent) registration and resolves within one
  // register round trip. Parking escapes on the create reply instead would
  // stretch a millisecond wait into the full creation time -- unbounded for
  // unschedulable actors, and a deadlock for ray.kill, whose cancellation is
  // the only way an unschedulable create ever replies.
  actor_client_.AsyncCreateActor(
      task_spec,
      [this, actor_id, callback](Status status, rpc::CreateActorReply &&reply) {
        lazy_pending_actors_->erase(actor_id);
        dead_before_create_actors_->erase(actor_id);
        callback(status, std::move(reply));
      });
}

bool ActorCreator::MarkDeadBeforeCreate(const ActorID &actor_id) {
  // Tombstone only actors the GCS may not have finished creating: pending
  // lazy registration (create not sent) or with a registration in flight
  // (a kill parks on it and destroys an UNREADY actor).
  if (lazy_pending_actors_->find(actor_id) != lazy_pending_actors_->end() ||
      registering_actors_->find(actor_id) != registering_actors_->end()) {
    dead_before_create_actors_->insert(actor_id);
    return true;
  }
  return false;
}

void ActorCreator::OnRegistrationConfirmedByGcs(const ActorID &actor_id) {
  lazy_pending_actors_->erase(actor_id);
}

void ActorCreator::ClearPendingLazyState(const ActorID &actor_id) {
  lazy_pending_actors_->erase(actor_id);
  dead_before_create_actors_->erase(actor_id);
}

}  // namespace core
}  // namespace ray
