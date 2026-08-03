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
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "ray/asio/instrumented_io_context.h"
#include "ray/asio/periodical_runner_interface.h"
#include "ray/common/id.h"
#include "ray/raylet_rpc_client/raylet_client_pool.h"
#include "src/ray/protobuf/gcs.pb.h"

namespace ray {
namespace gcs {

/// Pulls every raylet's pending resource requests (autoscaler bookkeeping) on
/// a dedicated io_context, and applies the replies on the main io_context,
/// where the consumers live.
///
/// Replies are buffered on the pull io_context and posted to the main
/// io_context in batches instead of one post per reply. The apply work itself
/// is tiny (~4us per reply measured in a 10k-node instrumented run); the
/// dominant cost of per-reply posting is waking up the main thread once per
/// reply, which at 10k nodes means ~10k wakeups per 1s pull round. Batching
/// collapses that to one wakeup per flush period (~20/s) while keeping each
/// apply burst bounded by kMaxApplyBatchSize.
class GcsResourceLoadPuller {
 public:
  /// How long a reply may sit in the buffer before it is flushed to the main
  /// io_context. Anything comfortably below the 1s pull period works: the
  /// wakeup savings saturate above ~10ms, and the added staleness stays
  /// negligible against the 1s sampling age the pull period already imposes.
  static constexpr uint64_t kFlushPeriodMs = 50;
  /// Flush immediately once this many replies are buffered, so that a reply
  /// wave right after a pull round cannot accumulate into one long
  /// non-yielding apply burst on the main thread (1024 replies is a burst of
  /// a few milliseconds at the ~4us per-reply apply cost).
  static constexpr size_t kMaxApplyBatchSize = 1024;

  /// \param raylet_client_pool Pool whose reply callbacks must be dispatched
  /// on `pull_io_context` (i.e. its ClientCallManager is bound to it); the
  /// reply path checks this.
  /// \param flush_periodical_runner Runner bound to `pull_io_context`; used to
  /// register the periodic flush of buffered replies.
  GcsResourceLoadPuller(
      instrumented_io_context &pull_io_context,
      instrumented_io_context &main_io_context,
      rpc::RayletClientPool &raylet_client_pool,
      PeriodicalRunnerInterface &flush_periodical_runner,
      std::function<void(std::vector<rpc::ResourcesData>)> apply_on_main);

  void Pull(std::vector<rpc::Address> raylet_addresses);

 private:
  instrumented_io_context &pull_io_context_;
  instrumented_io_context &main_io_context_;
  rpc::RayletClientPool &raylet_client_pool_;
  std::function<void(std::vector<rpc::ResourcesData>)> apply_on_main_;
  /// Replies waiting for the next flush. Only touched on the pull io_context
  /// (reply callbacks and the flush timer both run there, and RAY_CHECK it),
  /// so no lock. Held through a shared_ptr so that the reply callbacks and
  /// the registered flush fn stay self-contained instead of capturing `this`:
  /// neither is cancelled when the puller is destroyed, so a `this` capture
  /// would be a use-after-free if one ran late during shutdown. (They still
  /// hold copies of `apply_on_main`, which captures the GcsServer; that is
  /// safe because the GCS stops and joins the pull thread before destroying
  /// any member.)
  std::shared_ptr<std::vector<rpc::ResourcesData>> pending_applies_;
  /// Node ids from the last Pull(), diffed against the next snapshot to remove
  /// dead raylets' pooled clients.
  absl::flat_hash_set<NodeID> pulled_node_ids_;
};

}  // namespace gcs
}  // namespace ray
