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

#include <atomic>
#include <boost/asio/deadline_timer.hpp>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ray {
namespace core {

namespace {
// Lease ids issued by this submitter share the (worker id, counter) namespace
// with NormalTaskSubmitter's own static counter. Partition the space by
// starting in the upper half so the two submitters on one owner can never
// mint the same LeaseID.
std::atomic<uint32_t> lease_id_counter{1u << 31};
}  // namespace

void ActorCreationSubmitter::SubmitCreation(const TaskSpecification &creation_spec,
                                            const rpc::Address &start_raylet_address,
                                            CreationCallback callback) {
  RAY_CHECK(thread_checker_.IsOnSameThread());
  RAY_CHECK(creation_spec.IsActorCreationTask());
  const ActorID actor_id = creation_spec.ActorCreationId();
  RAY_CHECK(!creations_.contains(actor_id))
      << "Duplicate creation submission for actor " << actor_id
      << "; terminated actors must be forgotten via OnActorTerminated first.";
  CreationEntry entry;
  entry.spec = creation_spec;
  entry.first_raylet_address = start_raylet_address;
  entry.callback = std::move(callback);
  creations_[actor_id] = std::move(entry);
  RequestLease(
      actor_id, start_raylet_address, /*is_spillback=*/false, /*reuse_lease_id=*/false);
}

void ActorCreationSubmitter::SubmitCreation(const TaskSpecification &creation_spec,
                                            CreationCallback callback) {
  RAY_CHECK(lease_policy_ != nullptr)
      << "SubmitCreation without an explicit raylet requires a lease policy.";
  auto [best_node_address, is_selected_based_on_locality] =
      lease_policy_->GetBestNodeForLease(LeaseSpecification(creation_spec.GetMessage()));
  (void)is_selected_based_on_locality;
  SubmitCreation(creation_spec, best_node_address, std::move(callback));
}

void ActorCreationSubmitter::RequestLease(const ActorID &actor_id,
                                          const rpc::Address &raylet_address,
                                          bool is_spillback,
                                          bool reuse_lease_id) {
  auto it = creations_.find(actor_id);
  if (it == creations_.end() || it->second.state != CreationState::kPendingLease) {
    return;
  }
  CreationEntry &entry = it->second;
  if (!reuse_lease_id) {
    // Every attempt (first hop or spillback hop) gets a fresh lease id so a
    // cancel can never hit a lease the raylet has merged with a previous one.
    entry.lease_id = LeaseID::FromWorker(owner_worker_id_, ++lease_id_counter);
  }
  entry.current_raylet_address = raylet_address;

  rpc::RequestWorkerLeaseRequest request;
  request.mutable_lease_spec()->CopyFrom(
      LeaseSpecification(entry.spec.GetMessage()).GetMessage());
  request.mutable_lease_spec()->set_lease_id(entry.lease_id.Binary());
  // Like normal tasks: the first hop may spill us elsewhere; a spillback
  // target must grant or reject rather than forward again.
  request.set_grant_or_reject(is_spillback);

  auto raylet_client = raylet_client_pool_->GetOrConnectByAddress(raylet_address);
  const LeaseID issued_lease_id = entry.lease_id;
  raylet_client->RequestWorkerLease(
      std::move(request),
      [this, actor_id, issued_lease_id, is_spillback](
          const Status &status, rpc::RequestWorkerLeaseReply &&reply) {
        auto entry_it = creations_.find(actor_id);
        if (entry_it == creations_.end() ||
            entry_it->second.state != CreationState::kPendingLease ||
            entry_it->second.lease_id != issued_lease_id) {
          return;
        }
        CreationEntry &e = entry_it->second;
        if (!status.ok()) {
          // The raylet may have granted even though the reply was lost, and
          // leases never expire, so the lease must not be abandoned: retry
          // with the same lease id, which the raylet answers idempotently.
          const rpc::Address retry_address = e.current_raylet_address;
          RetryAfterBackoff(
              [this, actor_id, retry_address, is_spillback, issued_lease_id]() {
                // Reuse only if this is still the same lease generation: the
                // entry may have been cancelled and resubmitted while the
                // retry was pending, and replaying a new generation's lease
                // id at the old raylet would double-grant it.
                auto retry_it = creations_.find(actor_id);
                if (retry_it == creations_.end() ||
                    retry_it->second.lease_id != issued_lease_id) {
                  return;
                }
                RequestLease(
                    actor_id, retry_address, is_spillback, /*reuse_lease_id=*/true);
              });
          return;
        }
        if (reply.canceled()) {
          CompleteCancelled(
              actor_id, reply.failure_type(), reply.scheduling_failure_message());
          return;
        }
        if (!reply.retry_at_raylet_address().node_id().empty()) {
          if (e.cancel_requested) {
            // The reply consumed the previous request and nothing is queued
            // anywhere: converging the cancel here avoids racing a fresh
            // lease request against it.
            CompleteCancelled(actor_id,
                              rpc::RequestWorkerLeaseReply::SCHEDULING_CANCELLED_INTENDED,
                              "Actor creation cancelled by the owner.");
            return;
          }
          RequestLease(actor_id,
                       reply.retry_at_raylet_address(),
                       /*is_spillback=*/true,
                       /*reuse_lease_id=*/false);
          return;
        }
        if (reply.rejected()) {
          if (e.cancel_requested) {
            CompleteCancelled(actor_id,
                              rpc::RequestWorkerLeaseReply::SCHEDULING_CANCELLED_INTENDED,
                              "Actor creation cancelled by the owner.");
            return;
          }
          // The spillback target could not host us: retry from the first
          // hop, whose refreshed view routes the next attempt.
          const rpc::Address first_hop = e.first_raylet_address;
          RequestLease(
              actor_id, first_hop, /*is_spillback=*/false, /*reuse_lease_id=*/false);
          return;
        }
        if (reply.worker_address().worker_id().empty()) {
          // A reply with neither worker, spillback, rejection, nor
          // cancellation is malformed remote input; retry rather than crash.
          const rpc::Address retry_address = e.current_raylet_address;
          RAY_LOG(WARNING).WithField(actor_id)
              << "Lease reply carried no actionable outcome; retrying.";
          RetryAfterBackoff(
              [this, actor_id, retry_address, is_spillback, issued_lease_id]() {
                // Reuse only if this is still the same lease generation: the
                // entry may have been cancelled and resubmitted while the
                // retry was pending, and replaying a new generation's lease
                // id at the old raylet would double-grant it.
                auto retry_it = creations_.find(actor_id);
                if (retry_it == creations_.end() ||
                    retry_it->second.lease_id != issued_lease_id) {
                  return;
                }
                RequestLease(
                    actor_id, retry_address, is_spillback, /*reuse_lease_id=*/true);
              });
          return;
        }
        // Granted. The lease is held for the actor's lifetime: it is never
        // returned, never expires, and its worker is never reused.
        e.actor_address = reply.worker_address();
        e.resource_mapping.assign(reply.resource_mapping().begin(),
                                  reply.resource_mapping().end());
        e.state = CreationState::kPushing;
        if (e.cancel_requested) {
          // Cancel lost the race to the grant: the caller must use the kill
          // path against a live incarnation. Detach the callbacks before
          // invoking anything (callbacks may re-enter this submitter).
          e.cancel_requested = false;
          auto cancel_callbacks = std::move(e.cancel_callbacks);
          e.cancel_callbacks.clear();
          PushCreationTask(actor_id);
          for (auto &cancel_callback : cancel_callbacks) {
            cancel_callback(/*cancelled=*/false);
          }
          return;
        }
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
  for (const auto &mapping : entry.resource_mapping) {
    request->add_resource_mapping()->CopyFrom(mapping);
  }
  auto worker_client = core_worker_client_pool_->GetOrConnect(entry.actor_address);
  worker_client->PushNormalTask(
      std::move(request),
      [this, actor_id](const Status &status, rpc::PushTaskReply &&reply) {
        auto entry_it = creations_.find(actor_id);
        if (entry_it == creations_.end() ||
            entry_it->second.state != CreationState::kPushing) {
          return;
        }
        CreationEntry &e = entry_it->second;
        CreationResult result;
        result.status = status;
        result.lease_id = e.lease_id;
        result.push_task_reply = std::move(reply);
        if (status.ok()) {
          e.state = CreationState::kAlive;
          result.actor_address = e.actor_address;
          // The entry is retained: it records the permanently held lease.
          // Detach the callback before invoking it (it may re-enter).
          auto callback = std::move(e.callback);
          e.callback = nullptr;
          callback(result);
          return;
        }
        // No cancel callbacks can exist here: they are only registered in
        // kPendingLease and are detached at grant time.
        RAY_CHECK(e.cancel_callbacks.empty());
        auto callback = std::move(e.callback);
        creations_.erase(entry_it);
        callback(result);
      });
}

void ActorCreationSubmitter::CancelCreation(const ActorID &actor_id,
                                            std::function<void(bool)> callback) {
  RAY_CHECK(thread_checker_.IsOnSameThread());
  auto it = creations_.find(actor_id);
  if (it == creations_.end()) {
    // No live creation: no worker exists or will exist under this entry.
    callback(/*cancelled=*/true);
    return;
  }
  CreationEntry &entry = it->second;
  if (entry.state != CreationState::kPendingLease) {
    // Already granted (pushing or alive): the caller takes the kill path.
    callback(/*cancelled=*/false);
    return;
  }
  entry.cancel_callbacks.push_back(std::move(callback));
  if (entry.cancel_requested) {
    return;
  }
  entry.cancel_requested = true;
  IssueCancel(actor_id);
}

void ActorCreationSubmitter::IssueCancel(const ActorID &actor_id) {
  auto it = creations_.find(actor_id);
  if (it == creations_.end() || !it->second.cancel_requested ||
      it->second.state != CreationState::kPendingLease) {
    return;
  }
  CreationEntry &entry = it->second;
  const LeaseID issued_lease_id = entry.lease_id;
  auto raylet_client =
      raylet_client_pool_->GetOrConnectByAddress(entry.current_raylet_address);
  raylet_client->CancelWorkerLease(
      entry.lease_id,
      [this, actor_id, issued_lease_id](const Status &status,
                                        rpc::CancelWorkerLeaseReply &&reply) {
        auto entry_it = creations_.find(actor_id);
        if (entry_it == creations_.end() || !entry_it->second.cancel_requested ||
            entry_it->second.state != CreationState::kPendingLease) {
          // Grant won the race and already answered, or the creation
          // completed/failed.
          return;
        }
        if (entry_it->second.lease_id != issued_lease_id) {
          // The lease moved (spillback) while this cancel was in flight;
          // re-aim at the current lease.
          IssueCancel(actor_id);
          return;
        }
        if (status.ok() && reply.success()) {
          CompleteCancelled(actor_id,
                            rpc::RequestWorkerLeaseReply::SCHEDULING_CANCELLED_INTENDED,
                            "Actor creation cancelled by the owner.");
          return;
        }
        // The cancel raced ahead of the lease request, the reply was lost, or
        // the raylet is unreachable: back off and loop until it converges. A
        // grant arriving meanwhile answers through the grant handler.
        RetryAfterBackoff([this, actor_id]() { IssueCancel(actor_id); });
      });
}

void ActorCreationSubmitter::CompleteCancelled(
    const ActorID &actor_id,
    rpc::RequestWorkerLeaseReply::SchedulingFailureType failure_type,
    const std::string &failure_message) {
  auto it = creations_.find(actor_id);
  if (it == creations_.end()) {
    return;
  }
  CreationEntry &e = it->second;
  CreationResult result;
  result.status = Status::SchedulingCancelled(failure_message);
  result.failure_type = failure_type;
  result.scheduling_failure_message = failure_message;
  auto callback = std::move(e.callback);
  auto cancel_callbacks = std::move(e.cancel_callbacks);
  creations_.erase(it);
  for (auto &cancel_callback : cancel_callbacks) {
    cancel_callback(/*cancelled=*/true);
  }
  callback(result);
}

std::optional<rpc::Address> ActorCreationSubmitter::GetActorAddress(
    const ActorID &actor_id) const {
  RAY_CHECK(thread_checker_.IsOnSameThread());
  auto it = creations_.find(actor_id);
  if (it == creations_.end() || it->second.state == CreationState::kPendingLease) {
    return std::nullopt;
  }
  return it->second.actor_address;
}

void ActorCreationSubmitter::OnActorTerminated(const ActorID &actor_id) {
  RAY_CHECK(thread_checker_.IsOnSameThread());
  auto it = creations_.find(actor_id);
  if (it == creations_.end()) {
    return;
  }
  RAY_CHECK(it->second.state == CreationState::kAlive)
      << "Only alive actors can be forgotten; pending creations must be "
         "cancelled first.";
  creations_.erase(it);
}

void ActorCreationSubmitter::RetryAfterBackoff(std::function<void()> fn) {
  auto timer = std::make_shared<boost::asio::deadline_timer>(io_service_);
  timer->expires_from_now(boost::posix_time::milliseconds(retry_backoff_ms_));
  timer->async_wait([timer, fn = std::move(fn)](const boost::system::error_code &ec) {
    if (!ec) {
      fn();
    }
  });
}

std::optional<LeaseID> ActorCreationSubmitter::GetGrantedLease(
    const ActorID &actor_id) const {
  RAY_CHECK(thread_checker_.IsOnSameThread());
  auto it = creations_.find(actor_id);
  if (it == creations_.end() || it->second.state == CreationState::kPendingLease) {
    return std::nullopt;
  }
  return it->second.lease_id;
}

}  // namespace core
}  // namespace ray
