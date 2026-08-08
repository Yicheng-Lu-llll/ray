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

#pragma once

#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/proto_buffer_reader.h>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "ray/asio/instrumented_io_context.h"
#include "ray/common/id.h"
#include "ray/ray_syncer/common.h"
#include "ray/ray_syncer/ray_syncer_bidi_reactor.h"
#include "src/ray/protobuf/ray_syncer.grpc.pb.h"

namespace ray::syncer {

/// This class implements the communication between two nodes except the initialization
/// and cleanup.
/// It keeps track of the message received and sent between two nodes and uses that to
/// deduplicate the messages. It also supports the batching for performance purposes.
///
/// \tparam T The grpc reactor base. Its wire message type is either
///     RaySyncMessageBatch (typed wire) or grpc::ByteBuffer (raw wire). With the raw
///     wire, outgoing batches are assembled from the messages' cached frames as
///     zero-copy slice sequences (serialize-once fan-out) and incoming buffers are
///     parsed explicitly; the bytes on the wire are identical in both modes.
template <typename T, typename WireT = RaySyncMessageBatch>
class RaySyncerBidiReactorBase : public RaySyncerBidiReactor, public T {
  static constexpr bool kRawWire = std::is_same_v<WireT, grpc::ByteBuffer>;
 public:
  /// Constructor of RaySyncerBidiReactor.
  ///
  /// \param io_context The io context for the callback.
  /// \param remote_node_id The node id connects to.
  /// \param message_processor The callback for the message received.
  /// \param cleanup_cb When the connection terminates, it'll be called to cleanup
  ///     the environment.
  /// \param max_batch_size The maximum number of messages in a batch.
  /// \param max_batch_delay_ms The maximum delay time to wait before sending a batch.
  RaySyncerBidiReactorBase(
      instrumented_io_context &io_context,
      std::string remote_node_id,
      std::function<void(std::shared_ptr<const RaySyncMessage>)> message_processor,
      size_t max_batch_size,
      uint64_t max_batch_delay_ms)
      : RaySyncerBidiReactor(std::move(remote_node_id)),
        io_context_(io_context),
        message_processor_(std::move(message_processor)),
        max_batch_size_(max_batch_size),
        max_batch_delay_ms_(std::chrono::milliseconds(max_batch_delay_ms)),
        batch_timer_(io_context),
        batch_timer_active_(false) {}

  bool PushToSendingQueue(CachedSyncMessagePtr message) override {
    if (*IsDisconnected()) {
      return false;
    }

    // Try to filter out the messages the target node already has.
    // Usually it'll be the case when the message is generated from the
    // target node or it's sent from the target node.
    // No need to resend the message sent from a node back.
    if (message->msg->node_id() == GetRemoteNodeID()) {
      // Skip the message when it's about the node of this connection.
      return false;
    }

    auto &node_versions = GetNodeComponentVersions(message->msg->node_id());
    if (node_versions[message->msg->message_type()] >= message->msg->version()) {
      RAY_LOG(DEBUG) << "Dropping sync message with stale version. latest version: "
                     << node_versions[message->msg->message_type()]
                     << ", dropped message version: " << message->msg->version();
      return false;
    }

    node_versions[message->msg->message_type()] = message->msg->version();
    sending_buffer_[std::make_pair(message->msg->node_id(),
                                   message->msg->message_type())] = std::move(message);
    // sending_buffer_ size can be greater than max_batch_size_ as previous message batch
    // might be sending in progress, i.e., making sending_ = true.
    if (sending_buffer_.size() >= max_batch_size_ || max_batch_delay_ms_.count() == 0) {
      // Send immediately if batch size limit is reached or delay is 0
      if (batch_timer_active_) {
        batch_timer_.cancel();
        batch_timer_active_ = false;
      }
      StartSend();
    } else {
      // Start or restart the batch timer
      if (!batch_timer_active_) {
        RAY_LOG(DEBUG) << "Batch timer expires after " << max_batch_delay_ms_.count()
                       << " ms";
        batch_timer_active_ = true;
        batch_timer_.expires_after(max_batch_delay_ms_);
        // Use weak_ptr to avoid use-after-free when the reactor is destroyed.
        auto weak_self = std::weak_ptr<RaySyncerBidiReactor>(self_ref_);
        batch_timer_.async_wait([weak_self, this](const boost::system::error_code &ec) {
          auto self = weak_self.lock();
          if (!self) {
            return;
          }
          batch_timer_active_ = false;
          if (!ec && !*IsDisconnected()) {
            StartSend();
          } else if (ec != boost::asio::error::operation_aborted) {
            RAY_LOG(ERROR) << "Batch timer error: " << ec.message();
          }
        });
      }
    }
    return true;
  }

  virtual ~RaySyncerBidiReactorBase() {
    if (batch_timer_active_) {
      batch_timer_.cancel();
    }
  }

  void SetOutboundLog(const OutboundLog *log, size_t start_cursor) override {
    outbound_log_ = log;
    log_cursor_ = start_cursor;
  }

  void KickSend() override {
    if (*IsDisconnected()) {
      return;
    }
    StartSend();
  }

  void StartPull() {
    receiving_wire_ = std::make_shared<WireT>();
    RAY_LOG(DEBUG) << "Start reading: " << NodeID::FromBinary(GetRemoteNodeID());
    StartRead(receiving_wire_.get());
  }

 protected:
  /// The io context
  instrumented_io_context &io_context_;

 private:
  /// Handle the updates sent from the remote node.
  ///
  /// \param message_batch The message batch received.
  void ReceiveUpdate(const RaySyncMessageBatch &batch) {
    RAY_CHECK(batch.messages_size() > 0);

    RAY_LOG(DEBUG) << "Receive message batch with messages_size="
                   << batch.messages_size();

    for (const auto &message : batch.messages()) {
      auto &node_versions = GetNodeComponentVersions(message.node_id());
      RAY_LOG(DEBUG) << "Receive update: "
                     << " message_type=" << message.message_type()
                     << ", message_version=" << message.version()
                     << ", local_message_version="
                     << node_versions[message.message_type()];
      if (node_versions[message.message_type()] < message.version()) {
        node_versions[message.message_type()] = message.version();
        message_processor_(std::make_shared<const RaySyncMessage>(message));
      } else {
        RAY_LOG_EVERY_MS(WARNING, 1000)
            << "Drop message received from " << NodeID::FromBinary(message.node_id())
            << " because the message version " << message.version()
            << " is older than the local version "
            << node_versions[message.message_type()]
            << ". Message type: " << message.message_type();
      }
    }
  }

  void SendNext() {
    sending_ = false;
    // Release the completed write's buffer instead of retaining it until the
    // next send (a retained giant batch per connection is a memory bomb).
    sending_wire_.reset();
    StartSend();
  }

  bool LogHasPending() const {
    return outbound_log_ != nullptr && log_cursor_ < outbound_log_->entries.size();
  }

  void StartSend() {
    if (sending_ || (sending_buffer_.empty() && !LogHasPending())) {
      return;
    }

    RAY_LOG(DEBUG) << "Start sending to " << NodeID::FromBinary(GetRemoteNodeID())
                   << ", pending messages: " << sending_buffer_.size();

    // Bench-only bounded-batch experiment knob (fix-B candidate): cap messages per
    // write so a huge coalesced buffer is drained in slices instead of one giant
    // uninterruptible write. SIZE_MAX (default) == upstream behavior.
    static const size_t kMaxPerWrite = []() -> size_t {
      const char *v = std::getenv("SYNCER_MAX_PER_WRITE");
      return (v != nullptr && atoll(v) > 0) ? static_cast<size_t>(atoll(v))
                                            : std::numeric_limits<size_t>::max();
    }();
    const size_t take = std::min(kMaxPerWrite, sending_buffer_.size());

    static const size_t kMaxWriteBytes = []() -> size_t {
      const char *v = std::getenv("SYNCER_MAX_WRITE_BYTES");
      return (v != nullptr && atoll(v) > 0) ? static_cast<size_t>(atoll(v))
                                            : std::numeric_limits<size_t>::max();
    }();

    auto wire = std::make_shared<WireT>();
    if constexpr (kRawWire) {
      // Serialize-once fan-out: assemble the batch as a sequence of the messages'
      // cached frames (refcounted slices; no per-destination copy or serialization).
      std::vector<grpc::Slice> slices;
      slices.reserve(take);
      size_t bytes = 0;
      // Catch-up buffer first (initial-view push / per-message path).
      size_t taken = 0;
      for (auto it = sending_buffer_.begin();
           it != sending_buffer_.end() && taken < take && bytes < kMaxWriteBytes;
           ++taken) {
        const auto &entry = it->second;
        if (entry->frame.size() > 0) {
          slices.push_back(entry->frame);
        } else {
          slices.push_back(MakeSyncMessageFrame(*entry->msg));
        }
        bytes += slices.back().size();
        sending_buffer_.erase(it++);
      }
      // Then the shared outbound log: cursor-ranged; superseded/origin frames are
      // skipped, and adjacent surviving frames (arena-packed) coalesce into a
      // single zero-copy slice spanning the run.
      if (outbound_log_ != nullptr) {
        const auto &entries = outbound_log_->entries;
        size_t run_block = 0, run_start = 0, run_end = 0;
        bool in_run = false;
        auto flush_run = [&]() {
          if (in_run) {
            slices.push_back(MakeBlockSlice(
                outbound_log_->blocks[run_block], run_start, run_end - run_start));
            bytes += run_end - run_start;
            in_run = false;
          }
        };
        while (log_cursor_ < entries.size() && bytes < kMaxWriteBytes) {
          const auto &e = entries[log_cursor_];
          if (e.superseded || e.msg->node_id() == GetRemoteNodeID()) {
            flush_run();
            log_cursor_++;
            continue;
          }
          if (in_run && e.block == run_block && e.offset == run_end) {
            run_end += e.length;
          } else {
            flush_run();
            in_run = true;
            run_block = e.block;
            run_start = e.offset;
            run_end = e.offset + e.length;
          }
          log_cursor_++;
        }
        flush_run();
      }
      if (slices.empty()) {
        return;  // Everything in range was skipped; nothing to write.
      }
      *wire = grpc::ByteBuffer(slices.data(), slices.size());
    } else {
      size_t taken = 0;
      for (auto it = sending_buffer_.begin();
           it != sending_buffer_.end() && taken < take;
           ++taken) {
        *wire->add_messages() = *it->second->msg;
        sending_buffer_.erase(it++);
      }
    }

    Send(std::move(wire));
    sending_ = true;
  }

  /// Sending a message to the remote node
  ///
  /// \param wire The wire message (typed batch or raw byte buffer) to be sent
  void Send(std::shared_ptr<WireT> wire) {
    sending_wire_ = std::move(wire);
    RAY_LOG(DEBUG) << "[BidiReactor] Sending message batch to "
                   << NodeID::FromBinary(GetRemoteNodeID());
    StartWrite(sending_wire_.get());
  }

  // Please refer to grpc callback api for the following four methods:
  //     https://github.com/grpc/proposal/blob/master/L67-cpp-callback-api.md
  using T::StartRead;
  using T::StartWrite;

  void OnWriteDone(bool ok) override {
    io_context_.dispatch(
        [this, disconnected = IsDisconnected(), ok]() {
          if (*disconnected) {
            return;
          }
          if (ok) {
            SendNext();
          } else {
            RAY_LOG_EVERY_MS(INFO, 1000) << "Failed to send a message to node: "
                                         << NodeID::FromBinary(GetRemoteNodeID());
            Disconnect();
          }
        },
        "");
  }

  void OnReadDone(bool ok) override {
    io_context_.dispatch(
        [this, ok, msg_batch = std::move(receiving_wire_)]() mutable {
          // NOTE: According to the grpc callback streaming api best practices 3.)
          // https://grpc.io/docs/languages/cpp/best_practices/#callback-streaming-api
          // The client must read all incoming data i.e. until OnReadDone(ok = false)
          // happens for OnDone to be called. Hence even if disconnected_ is true, we
          // still need to allow OnReadDone to repeatedly execute until StartReadData has
          // consumed all the data for OnDone to be called.
          if (!ok) {
            RAY_LOG_EVERY_MS(INFO, 1000) << "Failed to read a message from node: "
                                         << NodeID::FromBinary(GetRemoteNodeID());
            Disconnect();
            return;
          }

          // Successful rpc completion callback.
          if (on_rpc_completion_) {
            on_rpc_completion_(NodeID::FromBinary(remote_node_id_));
          }
          if constexpr (kRawWire) {
            RaySyncMessageBatch batch;
            grpc::ProtoBufferReader reader(msg_batch.get());
            RAY_CHECK(batch.ParseFromZeroCopyStream(&reader))
                << "Failed to parse RaySyncMessageBatch from raw wire.";
            ReceiveUpdate(batch);
          } else {
            ReceiveUpdate(*msg_batch);
          }
          StartPull();
        },
        "");
  }

  /// grpc requests for sending and receiving
  std::shared_ptr<WireT> sending_wire_;
  std::shared_ptr<WireT> receiving_wire_;

  // For testing
  FRIEND_TEST(RaySyncerTest, RaySyncerBidiReactorBase);
  FRIEND_TEST(RaySyncerTest, RaySyncerBidiReactorBaseBatchSizeTriggerSend);
  FRIEND_TEST(RaySyncerTest, RaySyncerBidiReactorBaseBatchTimeoutTriggerSend);

  friend struct SyncerServerTest;

  std::array<int64_t, kComponentArraySize> &GetNodeComponentVersions(
      const std::string &node_id) {
    auto iter = node_versions_.find(node_id);
    if (iter == node_versions_.end()) {
      iter = node_versions_.emplace(node_id, std::array<int64_t, kComponentArraySize>())
                 .first;
      iter->second.fill(-1);
    }
    return iter->second;
  }

  /// Handler of a message update.
  const std::function<void(std::shared_ptr<const RaySyncMessage>)> message_processor_;

 private:
  /// Buffering all the updates. Sending will be done in an async way.
  absl::flat_hash_map<std::pair<std::string, MessageType>, CachedSyncMessagePtr>
      sending_buffer_;

  /// Keep track of the versions of components in the remote node.
  /// This field will be updated when messages are received or sent.
  /// We'll filter the received or sent messages when the message is stale.
  absl::flat_hash_map<std::string, std::array<int64_t, kComponentArraySize>>
      node_versions_;

  bool sending_ = false;

  /// Shared outbound frame log (raw-wire fan-out) and this connection's cursor.
  const OutboundLog *outbound_log_ = nullptr;
  size_t log_cursor_ = 0;

  /// Batch configuration
  const size_t max_batch_size_;
  const std::chrono::milliseconds max_batch_delay_ms_;

  /// Batch timer for delayed sending
  boost::asio::steady_timer batch_timer_;
  bool batch_timer_active_ = false;
};

}  // namespace ray::syncer
