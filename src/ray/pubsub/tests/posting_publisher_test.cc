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

#include "ray/pubsub/posting_publisher.h"

#include <memory>
#include <string>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "mock/ray/pubsub/publisher.h"
#include "ray/asio/instrumented_io_context.h"

namespace ray {
namespace pubsub {

namespace {

rpc::PubMessage MakeMessage() {
  rpc::PubMessage msg;
  msg.set_channel_type(rpc::ChannelType::WORKER_OBJECT_LOCATIONS_CHANNEL);
  msg.set_key_id("key");
  msg.mutable_worker_object_locations_message();
  return msg;
}

}  // namespace

// Publishing from the io_context's own thread must reach the inner publisher
// inline, before the current handler yields. If it were post()ed instead, the
// message would sit on asio's thread-private queue until the handler finishes,
// behind anything other threads post in the meantime; a stale snapshot published
// on subscriber registration could then be delivered after (and overwrite) a
// newer location update.
TEST(PostingPublisherTest, PublishesInlineOnTheIoContextThread) {
  instrumented_io_context io_service(/*enable_lag_probe=*/false,
                                     /*running_on_single_thread=*/true);
  auto inner = std::make_shared<MockPublisher>();
  PostingPublisher posting(inner, io_service);

  int published = 0;
  int failures_published = 0;
  EXPECT_CALL(*inner, Publish).WillRepeatedly([&](rpc::PubMessage) { published++; });
  EXPECT_CALL(*inner, PublishFailure)
      .WillRepeatedly(
          [&](rpc::ChannelType, const std::string &) { failures_published++; });

  bool publish_ran_inline = false;
  bool failure_ran_inline = false;
  io_service.post(
      [&]() {
        posting.Publish(MakeMessage());
        publish_ran_inline = published == 1;
        posting.PublishFailure(rpc::ChannelType::WORKER_OBJECT_LOCATIONS_CHANNEL, "key");
        failure_ran_inline = failures_published == 1;
      },
      "test");
  io_service.run();

  EXPECT_TRUE(publish_ran_inline);
  EXPECT_TRUE(failure_ran_inline);
}

TEST(PostingPublisherTest, DefersPublishFromOtherThreads) {
  instrumented_io_context io_service(/*enable_lag_probe=*/false,
                                     /*running_on_single_thread=*/true);
  auto inner = std::make_shared<MockPublisher>();
  PostingPublisher posting(inner, io_service);

  int published = 0;
  EXPECT_CALL(*inner, Publish).WillRepeatedly([&](rpc::PubMessage) { published++; });

  // This thread is not running the io_context, so the publish must be deferred
  // until the io_context drains.
  posting.Publish(MakeMessage());
  EXPECT_EQ(published, 0);

  io_service.run();
  EXPECT_EQ(published, 1);
}

}  // namespace pubsub
}  // namespace ray
