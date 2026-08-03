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

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ray {
namespace core {

void ActorCreationSubmitter::SubmitCreation(const TaskSpecification &creation_spec,
                                            const rpc::Address &start_raylet_address,
                                            CreationCallback callback) {
  RAY_CHECK(creation_spec.IsActorCreationTask());
  const ActorID actor_id = creation_spec.ActorCreationId();
  RAY_CHECK(!creations_.contains(actor_id))
      << "Duplicate creation submission for actor " << actor_id;
  CreationEntry entry;
  entry.spec = creation_spec;
  entry.callback = std::move(callback);
  creations_[actor_id] = std::move(entry);
  RequestLease(actor_id, start_raylet_address, /*is_spillback=*/false);
}

void ActorCreationSubmitter::RequestLease(const ActorID &actor_id,
                                          const rpc::Address &raylet_address,
                                          bool is_spillback) {
  auto it = creations_.find(actor_id);
  if (it == creations_.end() || it->second.state == CreationState::kCancelled) {
    return;
  }
  CreationEntry &entry = it->second;
  // Every attempt (first hop or spillback hop) gets a fresh lease id so a
  // cancel can never hit a lease the raylet has merged with a previous one.
  entry.lease_id = LeaseID::FromWorker(owner_worker_id_, ++lease_id_counter_);
  entry.current_raylet_address = raylet_address;

  rpc::RequestWorkerLeaseRequest request;
  request.mutable_lease_spec()->CopyFrom(
      LeaseSpecification(entry.spec.GetMessage()).GetMessage());
  request.mutable_lease_spec()->set_lease_id(entry.lease_id.Binary());
  // Like normal tasks: the first hop may spill us elsewhere; a spillback
  // target must grant or reject rather than forward again.
  request.set_grant_or_reject(is_spillback);

  auto raylet_client = raylet_client_pool_->GetOrConnectByAddress(raylet_address);
  raylet_client->RequestWorkerLease(
      std::move(request),
      [this, actor_id](const Status &status, rpc::RequestWorkerLeaseReply &&reply) {
        auto entry_it = creations_.find(actor_id);
        if (entry_it == creations_.end() ||
            entry_it->second.state == CreationState::kCancelled) {
          return;
        }
        CreationEntry &e = entry_it->second;
        if (!status.ok()) {
          CreationResult result;
          result.status = status;
          auto cb = std::move(e.callback);
          creations_.erase(entry_it);
          cb(result);
          return;
        }
        if (e.cancel_requested && !reply.canceled() &&
            reply.worker_address().worker_id().empty() && !reply.rejected() &&
            reply.retry_at_raylet_address().node_id().empty()) {
          // Nothing actionable in the reply; fall through to normal handling.
        }
        if (reply.canceled()) {
          e.state = CreationState::kCancelled;
          if (e.cancel_callback) {
            e.cancel_callback(/*cancelled=*/true);
          }
          CreationResult result;
          result.status = Status::SchedulingCancelled("Actor creation cancelled.");
          auto cb = std::move(e.callback);
          creations_.erase(entry_it);
          cb(result);
          return;
        }
        if (!reply.retry_at_raylet_address().node_id().empty()) {
          // Spillback: follow to the suggested raylet.
          RequestLease(actor_id, reply.retry_at_raylet_address(), /*is_spillback=*/true);
          return;
        }
        if (reply.rejected()) {
          // The spillback target could not host us after all; retry from the
          // node it reported so its fresher view routes the next attempt.
          RequestLease(actor_id, e.current_raylet_address, /*is_spillback=*/false);
          return;
        }
        RAY_CHECK(!reply.worker_address().worker_id().empty())
            << "Lease reply with neither worker, spillback, rejection, nor "
               "cancellation.";
        // Granted. The lease is held for the actor's lifetime: it is never
        // returned, never expires, and its worker is never reused.
        e.actor_address = reply.worker_address();
        if (e.cancel_requested) {
          // Cancel lost the race to the grant: the caller must use the kill
          // path against a live incarnation.
          e.cancel_requested = false;
          if (e.cancel_callback) {
            auto ccb = std::move(e.cancel_callback);
            ccb(/*cancelled=*/false);
          }
        }
        e.state = CreationState::kPushing;
        PushCreationTask(actor_id);
      });
}

void ActorCreationSubmitter::PushCreationTask(const ActorID &actor_id) {
  auto it = creations_.find(actor_id);
  if (it == creations_.end()) {
    return;
  }
  CreationEntry &entry = it->second;
  auto request = std::make_unique<rpc::PushTaskRequest>();
  request->mutable_task_spec()->CopyFrom(entry.spec.GetMessage());
  request->set_intended_worker_id(entry.actor_address.worker_id());
  auto worker_client = core_worker_client_pool_->GetOrConnect(entry.actor_address);
  worker_client->PushNormalTask(
      std::move(request),
      [this, actor_id](const Status &status, rpc::PushTaskReply &&reply) {
        auto entry_it = creations_.find(actor_id);
        if (entry_it == creations_.end()) {
          return;
        }
        CreationEntry &e = entry_it->second;
        CreationResult result;
        result.status = status;
        result.lease_id = e.lease_id;
        if (status.ok()) {
          e.state = CreationState::kAlive;
          result.actor_address = e.actor_address;
          // The entry is retained: it records the permanently held lease.
          e.callback(result);
          e.callback = nullptr;
          return;
        }
        auto cb = std::move(e.callback);
        creations_.erase(entry_it);
        cb(result);
      });
}

void ActorCreationSubmitter::CancelCreation(const ActorID &actor_id,
                                            std::function<void(bool)> callback) {
  auto it = creations_.find(actor_id);
  if (it == creations_.end() || it->second.state == CreationState::kCancelled) {
    callback(/*cancelled=*/true);
    return;
  }
  CreationEntry &entry = it->second;
  if (entry.state != CreationState::kPendingLease) {
    // Already granted (pushing or alive): the caller takes the kill path.
    callback(/*cancelled=*/false);
    return;
  }
  entry.cancel_requested = true;
  entry.cancel_callback = std::move(callback);
  IssueCancel(actor_id);
}

void ActorCreationSubmitter::IssueCancel(const ActorID &actor_id) {
  auto it = creations_.find(actor_id);
  if (it == creations_.end() || !it->second.cancel_requested) {
    return;
  }
  CreationEntry &entry = it->second;
  auto raylet_client =
      raylet_client_pool_->GetOrConnectByAddress(entry.current_raylet_address);
  raylet_client->CancelWorkerLease(
      entry.lease_id,
      [this, actor_id](const Status &status, rpc::CancelWorkerLeaseReply &&reply) {
        auto entry_it = creations_.find(actor_id);
        if (entry_it == creations_.end() || !entry_it->second.cancel_requested) {
          // Grant won the race and already answered the cancel callback, or
          // the creation completed/failed.
          return;
        }
        CreationEntry &e = entry_it->second;
        if (status.ok() && reply.success()) {
          e.state = CreationState::kCancelled;
          e.cancel_requested = false;
          auto ccb = std::move(e.cancel_callback);
          if (ccb) {
            ccb(/*cancelled=*/true);
          }
          CreationResult result;
          result.status = Status::SchedulingCancelled("Actor creation cancelled.");
          auto cb = std::move(e.callback);
          creations_.erase(entry_it);
          cb(result);
          return;
        }
        // The cancel raced ahead of the lease request (or a stale reply):
        // loop until it converges. A grant arriving meanwhile flips the
        // answer through the grant handler.
        IssueCancel(actor_id);
      });
}

std::optional<LeaseID> ActorCreationSubmitter::GetGrantedLease(
    const ActorID &actor_id) const {
  auto it = creations_.find(actor_id);
  if (it == creations_.end() || it->second.state == CreationState::kPendingLease ||
      it->second.state == CreationState::kCancelled) {
    return std::nullopt;
  }
  return it->second.lease_id;
}

}  // namespace core
}  // namespace ray
