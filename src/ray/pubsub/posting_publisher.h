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

#include <memory>
#include <string>
#include <utility>

#include "absl/container/flat_hash_map.h"
#include "absl/strings/str_cat.h"
#include "ray/asio/instrumented_io_context.h"
#include "ray/pubsub/publisher_interface.h"

namespace ray {
namespace pubsub {

/// PublisherInterface decorator: posts Publish and PublishFailure to the given
/// io_context so callers don't take the publisher's mutex on their own thread.
///
/// Calls made from the io_context's own thread run inline instead (dispatch
/// semantics). This is a correctness requirement, not an optimization: messages
/// on these channels carry absolute state and subscribers keep the last one they
/// receive, so a message must be sequenced at the time its content is built.
/// Posting from within a handler running on this io_context would put the message
/// on asio's thread-private queue, which is appended to the shared queue only when
/// the current handler finishes. Any message posted by another thread in the
/// meantime would then be delivered first, letting a stale snapshot (e.g. the one
/// published on subscriber registration) overwrite a newer location update.
class PostingPublisher : public PublisherInterface {
 public:
  PostingPublisher(std::shared_ptr<PublisherInterface> inner,
                   instrumented_io_context &io_service)
      : inner_(std::move(inner)), io_service_(io_service) {}

  void Publish(rpc::PubMessage pub_message) override {
    if (io_service_.get_executor().running_in_this_thread()) {
      // A caller on this thread may hold ReferenceCounter::mutex_ (e.g.
      // PublishObjectLocationSnapshot), so this inline path takes the inner
      // Publisher's mutex under it: the lock order is ReferenceCounter -> Publisher.
      // Keep Publisher a leaf lock (it must never call back into ReferenceCounter) or
      // this becomes a deadlock.
      inner_->Publish(std::move(pub_message));
      return;
    }
    const auto channel_type = pub_message.channel_type();
    io_service_.post(
        [inner = inner_, pub_message = std::move(pub_message)]() mutable {
          inner->Publish(std::move(pub_message));
        },
        PublishEventName(channel_type));
  }

  void PublishFailure(const rpc::ChannelType channel_type,
                      const std::string &key_id) override {
    if (io_service_.get_executor().running_in_this_thread()) {
      inner_->PublishFailure(channel_type, key_id);
      return;
    }
    io_service_.post(
        [inner = inner_, channel_type, key_id]() {
          inner->PublishFailure(channel_type, key_id);
        },
        PublishFailureEventName(channel_type));
  }

  void ConnectToSubscriber(
      const rpc::PubsubLongPollingRequest &request,
      std::string *publisher_id,
      google::protobuf::RepeatedPtrField<rpc::PubMessage> *pub_messages,
      rpc::SendReplyCallback send_reply_callback) override {
    inner_->ConnectToSubscriber(
        request, publisher_id, pub_messages, std::move(send_reply_callback));
  }

  StatusSet<StatusT::InvalidArgument> RegisterSubscription(
      const rpc::ChannelType channel_type,
      const UniqueID &subscriber_id,
      const std::optional<std::string> &key_id) override {
    return inner_->RegisterSubscription(channel_type, subscriber_id, key_id);
  }

  void UnregisterSubscription(const rpc::ChannelType channel_type,
                              const UniqueID &subscriber_id,
                              const std::optional<std::string> &key_id) override {
    inner_->UnregisterSubscription(channel_type, subscriber_id, key_id);
  }

  void UnregisterSubscriber(const UniqueID &subscriber_id) override {
    inner_->UnregisterSubscriber(subscriber_id);
  }

  std::string DebugString() const override { return inner_->DebugString(); }

 private:
  static absl::flat_hash_map<int, std::string> BuildEventNames(absl::string_view prefix) {
    absl::flat_hash_map<int, std::string> names;
    for (int i = rpc::ChannelType_MIN; i <= rpc::ChannelType_MAX; ++i) {
      if (rpc::ChannelType_IsValid(i)) {
        names[i] =
            absl::StrCat(prefix, rpc::ChannelType_Name(static_cast<rpc::ChannelType>(i)));
      }
    }
    return names;
  }

  static const std::string &PublishEventName(rpc::ChannelType channel_type) {
    static const auto &names = *new auto(BuildEventNames("Publisher.Publish."));
    return names.at(static_cast<int>(channel_type));
  }

  static const std::string &PublishFailureEventName(rpc::ChannelType channel_type) {
    static const auto &names = *new auto(BuildEventNames("Publisher.PublishFailure."));
    return names.at(static_cast<int>(channel_type));
  }

  std::shared_ptr<PublisherInterface> inner_;
  instrumented_io_context &io_service_;
};

}  // namespace pubsub
}  // namespace ray
