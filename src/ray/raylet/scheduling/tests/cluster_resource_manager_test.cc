// Copyright 2021 The Ray Authors.
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

#include "ray/raylet/scheduling/cluster_resource_manager.h"

#include <memory>
#include <random>
#include <vector>

#include "gtest/gtest.h"
#include "ray/asio/periodical_runner.h"

namespace ray {

NodeResources CreateNodeResources(double available_cpu,
                                  double total_cpu,
                                  double available_custom_resource = 0,
                                  double total_custom_resource = 0,
                                  bool object_pulls_queued = false) {
  NodeResources resources;
  resources.available.Set(ResourceID::CPU(), available_cpu);
  resources.total.Set(ResourceID::CPU(), total_cpu);
  resources.available.Set(scheduling::ResourceID("CUSTOM"), available_custom_resource);
  resources.total.Set(scheduling::ResourceID("CUSTOM"), total_custom_resource);
  resources.object_pulls_queued = object_pulls_queued;
  return resources;
}

struct ClusterResourceManagerTest : public ::testing::Test {
  void SetUp() {
    ::testing::Test::SetUp();
    static instrumented_io_context io_context;
    manager =
        std::make_unique<ClusterResourceManager>(PeriodicalRunner::Create(io_context));
    manager->AddOrUpdateNode(node0,
                             CreateNodeResources(/*available_cpu*/ 1, /*total_cpu*/ 1));
    manager->AddOrUpdateNode(node1,
                             CreateNodeResources(/*available_cpu*/ 0,
                                                 /*total_cpu*/ 0,
                                                 /*available_custom*/ 1,
                                                 /*total_custom*/ 1));
    manager->AddOrUpdateNode(node2,
                             CreateNodeResources(/*available_cpu*/ 1,
                                                 /*total_cpu*/ 1,
                                                 /*available_custom*/ 1,
                                                 /*total_custom*/ 1,
                                                 /*object_pulls_queued*/ true));
  }
  scheduling::NodeID node0 = scheduling::NodeID(0);
  scheduling::NodeID node1 = scheduling::NodeID(1);
  scheduling::NodeID node2 = scheduling::NodeID(2);
  scheduling::NodeID node3 = scheduling::NodeID(3);
  std::unique_ptr<ClusterResourceManager> manager;
};

TEST_F(ClusterResourceManagerTest, UpdateNode) {
  // Prepare a sync message with updated totals/available, labels and flags.
  syncer::ResourceViewSyncMessage payload;
  payload.mutable_resources_total()->insert({"CPU", 10.0});
  payload.mutable_resources_available()->insert({"CPU", 5.0});
  payload.mutable_labels()->insert({"zone", "us-east-1a"});
  payload.set_object_pulls_queued(true);
  payload.set_idle_duration_ms(42);
  payload.set_is_draining(true);
  payload.set_draining_deadline_timestamp_ms(123456);

  // Update existing node and validate the local view reflects the payload.
  ASSERT_TRUE(manager->UpdateNode(node0, payload));

  const auto &node_resources = manager->GetNodeResources(node0);
  ASSERT_EQ(node_resources.total.Get(scheduling::ResourceID("CPU")), 10);
  ASSERT_EQ(node_resources.available.Get(scheduling::ResourceID("CPU")), 5);
  ASSERT_EQ(node_resources.labels.at("zone"), "us-east-1a");
  ASSERT_TRUE(node_resources.object_pulls_queued);
  ASSERT_EQ(node_resources.idle_resource_duration_ms, 42);
  ASSERT_TRUE(node_resources.is_draining);
  ASSERT_EQ(node_resources.draining_deadline_timestamp_ms, 123456);
  ASSERT_TRUE(node_resources.last_resource_update_time.has_value());
}

TEST_F(ClusterResourceManagerTest, CustomResourceNodeIndex) {
  static instrumented_io_context io_context;
  auto index_manager = std::make_unique<ClusterResourceManager>(
      PeriodicalRunner::Create(io_context),
      /*maintain_custom_resource_node_index=*/true);
  scheduling::NodeID node(NodeID::FromRandom().Binary());

  auto make_msg = [](const absl::flat_hash_map<std::string, double> &resources) {
    syncer::ResourceViewSyncMessage msg;
    for (const auto &[name, value] : resources) {
      (*msg.mutable_resources_total())[name] = value;
      (*msg.mutable_resources_available())[name] = value;
    }
    return msg;
  };
  auto candidates_for = [](ClusterResourceManager &m, const std::string &name) {
    return m.GetCandidateNodesForRequest(
        ResourceMapToResourceRequest({{name, 1.0}}, false));
  };

  // A custom resource in a node's totals lands in the index.
  index_manager->AddOrUpdateNode(node, make_msg({{"CPU", 4.0}, {"accel", 1.0}}));
  auto candidates = candidates_for(*index_manager, "accel");
  ASSERT_TRUE(candidates != nullptr);
  ASSERT_EQ(*candidates, std::vector<scheduling::NodeID>{node});
  // Predefined names never restrict the domain.
  ASSERT_EQ(candidates_for(*index_manager, "CPU"), nullptr);
  // A custom name on no node gives an empty (infeasible-everywhere) domain.
  ASSERT_TRUE(candidates_for(*index_manager, "missing")->empty());

  // Removing the resource from the node's totals removes the entry.
  index_manager->AddOrUpdateNode(node, make_msg({{"CPU", 4.0}}));
  ASSERT_TRUE(candidates_for(*index_manager, "accel")->empty());

  // Node removal clears its entries.
  index_manager->AddOrUpdateNode(node, make_msg({{"CPU", 4.0}, {"accel", 1.0}}));
  index_manager->RemoveNode(node);
  ASSERT_TRUE(candidates_for(*index_manager, "accel")->empty());

  // Without the flag (the GCS), queries never restrict the domain: nullptr,
  // not an empty set, or every custom-resource request would be misjudged
  // infeasible against the unmaintained index.
  manager->AddOrUpdateNode(node, make_msg({{"accel", 1.0}}));
  ASSERT_EQ(candidates_for(*manager, "accel"), nullptr);
}

TEST_F(ClusterResourceManagerTest, CustomResourceNodeIndexInvariant) {
  // The index may never diverge from the view: after every step of a random
  // update sequence, its answer for every name must equal what a fresh scan
  // of the totals would answer, in both directions (no stale members, no
  // missing members).
  static instrumented_io_context io_context;
  ClusterResourceManager index_manager(PeriodicalRunner::Create(io_context),
                                       /*maintain_custom_resource_node_index=*/true);
  std::mt19937 rng(20260829);
  std::vector<scheduling::NodeID> node_pool;
  for (int i = 0; i < 8; i++) {
    node_pool.emplace_back(NodeID::FromRandom().Binary());
  }
  const std::vector<std::string> names = {"a", "b", "c", "d", "never_added"};

  auto make_msg = [](const absl::flat_hash_map<std::string, double> &resources) {
    syncer::ResourceViewSyncMessage msg;
    for (const auto &[name, value] : resources) {
      (*msg.mutable_resources_total())[name] = value;
      (*msg.mutable_resources_available())[name] = value;
    }
    return msg;
  };

  for (int step = 0; step < 300; ++step) {
    const auto &node = node_pool[rng() % node_pool.size()];
    if (rng() % 5 == 0) {
      index_manager.RemoveNode(node);
    } else {
      absl::flat_hash_map<std::string, double> resources{{"CPU", 4.0}};
      for (size_t k = 0; k + 1 < names.size(); ++k) {
        if (rng() % 2 == 0) {
          resources[names[k]] = 1.0;
        }
      }
      index_manager.AddOrUpdateNode(node, make_msg(resources));
    }
    for (const auto &name : names) {
      std::vector<scheduling::NodeID> expected;
      for (const auto &[node_id, node_ref] : index_manager.GetResourceView()) {
        if (node_ref.GetLocalView().total.Get(scheduling::ResourceID(name)) > 0) {
          expected.push_back(node_id);
        }
      }
      std::sort(expected.begin(), expected.end());
      auto got = index_manager.GetCandidateNodesForRequest(
          ResourceMapToResourceRequest({{name, 1.0}}, false));
      ASSERT_TRUE(got != nullptr);
      ASSERT_EQ(*got, expected) << "step " << step << " name " << name;
    }
  }
}

TEST_F(ClusterResourceManagerTest, DebugStringTest) {
  // Test max_num_nodes_to_include parameter is working.
  ASSERT_EQ(std::vector<std::string>(absl::StrSplit(manager->DebugString(), "node id:"))
                    .size() -
                1,
            3);
  ASSERT_EQ(std::vector<std::string>(
                absl::StrSplit(manager->DebugString(/*max_num_nodes_to_include=*/5),
                               "node id:"))
                    .size() -
                1,
            3);
  ASSERT_EQ(std::vector<std::string>(
                absl::StrSplit(manager->DebugString(/*max_num_nodes_to_include=*/2),
                               "node id:"))
                    .size() -
                1,
            2);
}

TEST_F(ClusterResourceManagerTest, HasFeasibleResourcesTest) {
  ASSERT_FALSE(manager->HasFeasibleResources(node3, {}));
  ASSERT_FALSE(manager->HasFeasibleResources(
      node0,
      ResourceMapToResourceRequest({{"GPU", 1}},
                                   /*requires_object_store_memory=*/false)));
  ASSERT_TRUE(manager->HasFeasibleResources(
      node0,
      ResourceMapToResourceRequest({{"CPU", 1}},
                                   /*requires_object_store_memory=*/false)));
  manager->SubtractNodeAvailableResources(
      node0,
      ResourceMapToResourceRequest({{"CPU", 1}},
                                   /*requires_object_store_memory=*/false));
  // node0 has no available CPU resource but it's still feasible.
  ASSERT_TRUE(manager->HasFeasibleResources(
      node0,
      ResourceMapToResourceRequest({{"CPU", 1}},
                                   /*requires_object_store_memory=*/false)));
}

TEST_F(ClusterResourceManagerTest, HasAvailableResourcesTest) {
  ASSERT_FALSE(manager->HasAvailableResources(
      node3, {}, /*ignore_object_store_memory_requirement*/ false));
  ASSERT_TRUE(manager->HasAvailableResources(
      node0,
      ResourceMapToResourceRequest({{"CPU", 1}},
                                   /*requires_object_store_memory=*/true),
      /*ignore_object_store_memory_requirement*/ false));
  ASSERT_FALSE(manager->HasAvailableResources(
      node0,
      ResourceMapToResourceRequest({{"CUSTOM", 1}},
                                   /*requires_object_store_memory=*/true),
      /*ignore_object_store_memory_requirement*/ false));
  ASSERT_TRUE(manager->HasAvailableResources(
      node1,
      ResourceMapToResourceRequest({{"CUSTOM", 1}},
                                   /*requires_object_store_memory=*/true),
      /*ignore_object_store_memory_requirement*/ false));
  ASSERT_TRUE(manager->HasAvailableResources(
      node2,
      ResourceMapToResourceRequest({{"CPU", 1}},
                                   /*requires_object_store_memory=*/false),
      /*ignore_object_store_memory_requirement*/ false));
  ASSERT_FALSE(manager->HasAvailableResources(
      node2,
      ResourceMapToResourceRequest({{"CPU", 1}},
                                   /*requires_object_store_memory=*/true),
      /*ignore_object_store_memory_requirement*/ false));
  ASSERT_TRUE(manager->HasAvailableResources(
      node2,
      ResourceMapToResourceRequest({{"CPU", 1}},
                                   /*requires_object_store_memory=*/true),
      /*ignore_object_store_memory_requirement*/ true));
}

TEST_F(ClusterResourceManagerTest, SubtractAndAddNodeAvailableResources) {
  const auto &node_resources = manager->GetNodeResources(node0);
  ASSERT_EQ(node_resources.available.Get(ResourceID::CPU()), 1);

  manager->SubtractNodeAvailableResources(
      node0,
      ResourceMapToResourceRequest({{"CPU", 1}},
                                   /*requires_object_store_memory=*/false));
  ASSERT_EQ(node_resources.available.Get(ResourceID::CPU()), 0);
  // Subtract again and make sure the available == 0.
  manager->SubtractNodeAvailableResources(
      node0,
      ResourceMapToResourceRequest({{"CPU", 1}},
                                   /*requires_object_store_memory=*/false));
  ASSERT_EQ(node_resources.available.Get(ResourceID::CPU()), 0);

  // Add resources back.
  manager->AddNodeAvailableResources(node0, ResourceSet({{"CPU", FixedPoint(1)}}));
  ASSERT_EQ(node_resources.available.Get(ResourceID::CPU()), 1);
  // Add again and make sure the available == 1 (<= total).
  manager->AddNodeAvailableResources(node0, ResourceSet({{"CPU", FixedPoint(1)}}));
  ASSERT_EQ(node_resources.available.Get(ResourceID::CPU()), 1);
}

}  // namespace ray
