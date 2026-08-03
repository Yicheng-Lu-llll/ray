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
#include <memory>
#include <utility>
#include <vector>

#include "ray/util/logging.h"

namespace ray {
namespace gcs {

namespace {

void FlushPendingApplies(
    std::vector<rpc::ResourcesData> &pending_applies,
    instrumented_io_context &main_io_context,
    const std::function<void(std::vector<rpc::ResourcesData>)> &apply_on_main) {
  if (pending_applies.empty()) {
    return;
  }
  std::vector<rpc::ResourcesData> batch;
  batch.swap(pending_applies);
  main_io_context.post(
      [apply_on_main, batch = std::move(batch)]() mutable {
        apply_on_main(std::move(batch));
      },
      "GcsResourceLoadPuller.apply_batch");
}

}  // namespace

GcsResourceLoadPuller::GcsResourceLoadPuller(
    instrumented_io_context &pull_io_context,
    instrumented_io_context &main_io_context,
    rpc::RayletClientPool &raylet_client_pool,
    PeriodicalRunnerInterface &flush_periodical_runner,
    std::function<void(std::vector<rpc::ResourcesData>)> apply_on_main)
    : pull_io_context_(pull_io_context),
      main_io_context_(main_io_context),
      raylet_client_pool_(raylet_client_pool),
      apply_on_main_(std::move(apply_on_main)),
      pending_applies_(std::make_shared<std::vector<rpc::ResourcesData>>()) {
  flush_periodical_runner.RunFnPeriodically(
      [pending_applies = pending_applies_,
       &pull_io_context = pull_io_context_,
       &main_io_context = main_io_context_,
       apply_on_main = apply_on_main_]() {
        RAY_CHECK(pull_io_context.get_executor().running_in_this_thread());
        FlushPendingApplies(*pending_applies, main_io_context, apply_on_main);
      },
      kFlushPeriodMs,
      "GcsResourceLoadPuller.flush");
}

void GcsResourceLoadPuller::Pull(std::vector<rpc::Address> raylet_addresses) {
  RAY_CHECK(pull_io_context_.get_executor().running_in_this_thread());
  absl::flat_hash_set<NodeID> current_node_ids;
  current_node_ids.reserve(raylet_addresses.size());
  for (const auto &address : raylet_addresses) {
    current_node_ids.insert(NodeID::FromBinary(address.node_id()));
  }
  for (const auto &node_id : pulled_node_ids_) {
    if (!current_node_ids.contains(node_id)) {
      raylet_client_pool_.Disconnect(node_id);
    }
  }
  pulled_node_ids_ = std::move(current_node_ids);

  for (const auto &address : raylet_addresses) {
    auto raylet_client = raylet_client_pool_.GetOrConnectByAddress(address);
    raylet_client->GetResourceLoad(
        [pending_applies = pending_applies_,
         &pull_io_context = pull_io_context_,
         &main_io_context = main_io_context_,
         apply_on_main = apply_on_main_](const Status &status,
                                         rpc::GetResourceLoadReply &&reply) {
          RAY_CHECK(pull_io_context.get_executor().running_in_this_thread());
          if (!status.ok()) {
            RAY_LOG_EVERY_N(WARNING, 10)
                << "Failed to get the resource load: " << status.ToString();
            return;
          }
          pending_applies->push_back(std::move(*reply.mutable_resources()));
          if (pending_applies->size() >= kMaxApplyBatchSize) {
            FlushPendingApplies(*pending_applies, main_io_context, apply_on_main);
          }
        });
  }
}

}  // namespace gcs
}  // namespace ray
