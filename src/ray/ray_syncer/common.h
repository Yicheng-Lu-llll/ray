// Copyright 2024 The Ray Authors.
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
//
// This file defines a few constants on ray syncer.

#pragma once

#include <grpcpp/support/slice.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "ray/common/id.h"
#include "src/ray/protobuf/ray_syncer.grpc.pb.h"
#include "src/ray/protobuf/ray_syncer.pb.h"

namespace ray::syncer {

inline constexpr size_t kComponentArraySize =
    static_cast<size_t>(ray::rpc::syncer::MessageType_ARRAYSIZE);

/// A sync message plus (optionally) its pre-serialized wire frame.
///
/// `frame` holds the length-delimited encoding of `msg` as one element of a
/// `RaySyncMessageBatch` (field 1): tag byte 0x0A + varint(size) + message bytes.
/// A batch's wire form is just the concatenation of such frames, so a fan-out
/// hub can serialize each message ONCE and assemble per-connection batches as
/// zero-copy slice sequences instead of deep-copying + re-serializing the same
/// message for every destination. `frame` may be empty (raylet side never pays
/// the serialization; only raw-wire reactors consume it).
struct CachedSyncMessage {
  std::shared_ptr<const ray::rpc::syncer::RaySyncMessage> msg;
  grpc::Slice frame;
};
using CachedSyncMessagePtr = std::shared_ptr<const CachedSyncMessage>;

/// Append-only log of outbound frames shared by all raw-wire fan-out reactors.
///
/// The hub deduplicates messages globally (NodeState) before appending, so
/// per-connection bookkeeping reduces to a cursor into this log. When a newer
/// version of a (node, type) entry is appended, the previous entry is flagged
/// `superseded` (O(1), no per-connection work), preserving today's coalescing
/// semantics: a connection assembling [cursor, end) skips superseded frames.
struct OutboundLogEntry {
  std::shared_ptr<const ray::rpc::syncer::RaySyncMessage> msg;
  /// Location of this entry's frame inside blocks[block] (arena packing):
  /// adjacent live frames are shipped as ONE zero-copy slice spanning the run.
  size_t block = 0;
  size_t offset = 0;
  size_t length = 0;
  bool superseded = false;
};
struct OutboundLog {
  /// Fixed-capacity arena blocks. A block is never reallocated once created
  /// (capacity reserved up front), so in-flight slices referencing it stay valid.
  static constexpr size_t kBlockSize = 256 * 1024;
  std::vector<std::shared_ptr<std::string>> blocks;
  std::vector<OutboundLogEntry> entries;
  /// (node_id, message_type) -> index of the newest entry for that key.
  absl::flat_hash_map<std::pair<std::string, int>, size_t> latest;

  /// Append a message: serialize once into the arena, supersede the previous
  /// entry of the same key.
  void Append(std::shared_ptr<const ray::rpc::syncer::RaySyncMessage> msg,
              const std::string &frame_bytes) {
    if (blocks.empty() || blocks.back()->size() + frame_bytes.size() >
                              std::max(kBlockSize, frame_bytes.size())) {
      blocks.push_back(std::make_shared<std::string>());
      blocks.back()->reserve(std::max(kBlockSize, frame_bytes.size()));
    }
    auto &block = *blocks.back();
    const size_t off = block.size();
    block.append(frame_bytes);
    const auto key = std::make_pair(msg->node_id(),
                                    static_cast<int>(msg->message_type()));
    entries.push_back(OutboundLogEntry{
        std::move(msg), blocks.size() - 1, off, frame_bytes.size(), false});
    auto [it, inserted] = latest.try_emplace(key, entries.size() - 1);
    if (!inserted) {
      entries[it->second].superseded = true;
      it->second = entries.size() - 1;
    }
  }
};

/// Build the length-delimited frame for one RaySyncMessage (serialize once).
grpc::Slice MakeSyncMessageFrame(const ray::rpc::syncer::RaySyncMessage &msg);

/// Append the length-delimited frame of `msg` to `out` (arena packing).
void AppendSyncMessageFrame(const ray::rpc::syncer::RaySyncMessage &msg,
                            std::string *out);

/// Zero-copy slice referencing [off, off+len) of an arena block; the slice keeps
/// the block alive via a shared_ptr held in its user data.
grpc::Slice MakeBlockSlice(const std::shared_ptr<std::string> &block,
                           size_t off,
                           size_t len);

// TODO(hjiang): As of now, only ray syncer uses it so we put it under `ray_syncer`
// folder, better to place it into other common folders if uses elsewhere.
//
// A callback, which is called whenever a rpc succeeds (at rpc communication level)
// between the current node and the remote node.
using RpcCompletionCallback = std::function<void(const NodeID &)>;

}  // namespace ray::syncer
