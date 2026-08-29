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

#include "absl/random/mock_distributions.h"
#include "absl/random/mocking_bit_gen.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "ray/raylet/scheduling/policy/composite_scheduling_policy.h"

namespace ray {

namespace raylet_scheduling_policy {

using namespace ::testing;
using namespace ray::raylet;

NodeResources CreateNodeResources(double available_cpu,
                                  double total_cpu,
                                  double available_memory,
                                  double total_memory,
                                  double available_gpu,
                                  double total_gpu) {
  NodeResources resources;
  resources.available.Set(ResourceID::CPU(), available_cpu)
      .Set(ResourceID::Memory(), available_memory)
      .Set(ResourceID::GPU(), available_gpu);
  resources.total.Set(ResourceID::CPU(), total_cpu)
      .Set(ResourceID::Memory(), total_memory)
      .Set(ResourceID::GPU(), total_gpu);
  return resources;
}

class HybridSchedulingPolicyTest : public ::testing::Test {
 public:
  scheduling::NodeID local_node = scheduling::NodeID(0);
  scheduling::NodeID n1 = scheduling::NodeID(1);
  scheduling::NodeID n2 = scheduling::NodeID(2);
  scheduling::NodeID n3 = scheduling::NodeID(3);
  scheduling::NodeID n4 = scheduling::NodeID(4);
  absl::flat_hash_map<scheduling::NodeID, Node> nodes;

  SchedulingOptions HybridOptions(
      float spread,
      bool avoid_local_node,
      bool require_node_available,
      bool avoid_gpu_nodes = RayConfig::instance().scheduler_avoid_gpu_nodes(),
      int schedule_top_k_absolute = 1,
      float scheduler_top_k_fraction = 0.1) {
    return SchedulingOptions(SchedulingType::HYBRID,
                             RayConfig::instance().scheduler_spread_threshold(),
                             avoid_local_node,
                             require_node_available,
                             avoid_gpu_nodes,
                             /*target_topology_assignment*/ std::nullopt,
                             /*scheduling_context*/ nullptr,
                             /*preferred_node*/ "",
                             schedule_top_k_absolute,
                             scheduler_top_k_fraction);
  }
};

TEST_F(HybridSchedulingPolicyTest, GetBestNode) {
  std::vector<std::pair<scheduling::NodeID, float>> node_scores{
      {n3, 0.6},
      {n4, 0.7},
      {n1, 0},
      {n2, 0},
  };

  // Test return 1 node always return the first node.
  {
    HybridSchedulingPolicy policy{local_node, {}, [](auto) { return true; }};
    EXPECT_EQ(n1,
              policy.GetBestNode(node_scores,
                                 /*num_candidate_nodes*/ 1,
                                 /*preferred_node*/ {},
                                 /*preferred_node_score*/ 1));
  }

  // Test return 3 node calls to the random generator.
  {
    absl::MockingBitGen mock;
    EXPECT_CALL(absl::MockUniform<size_t>(), Call(mock, 0u, 3u))
        .WillOnce(Return(1))
        .WillOnce(Return(2))
        .WillOnce(Return(0));
    HybridSchedulingPolicy policy{local_node, {}, [](auto) { return true; }};
    policy.bitgenref_ = absl::BitGenRef{mock};
    EXPECT_EQ(n2,
              policy.GetBestNode(node_scores,
                                 /*num_candidate_nodes*/ 3,
                                 /*preferred_node_id*/ {},
                                 /*preferred_node_score*/ 1));
    EXPECT_EQ(n3,
              policy.GetBestNode(node_scores,
                                 /*num_candidate_nodes*/ 3,
                                 /*preferred_node_id*/ {},
                                 /*preferred_node_score*/ 1));
    EXPECT_EQ(n1,
              policy.GetBestNode(node_scores,
                                 /*num_candidate_nodes*/ 3,
                                 /*preferred_node_id*/ {},
                                 /*preferred_node_score*/ 1));
  }
}

TEST_F(HybridSchedulingPolicyTest, GetBestNodePrioritizePreferredNode) {
  {
    std::vector<std::pair<scheduling::NodeID, float>> node_scores{
        {n3, 0.6},
        {n4, 0.7},
        {n1, 0},
        {n2, 0},
    };

    HybridSchedulingPolicy policy{local_node, {}, [](auto) { return true; }};
    // local node score is greater than the smallest one
    EXPECT_EQ(n1,
              policy.GetBestNode(node_scores,
                                 /*num_candidate_nodes*/ 1,
                                 /*preferred_node_id*/ {local_node},
                                 /*preferred_node_score*/ 0.5));

    // local node score is equal to the smallest one.
    EXPECT_EQ(local_node,
              policy.GetBestNode(node_scores,
                                 /*num_candidate_nodes*/ 1,
                                 /*preferred_node_id*/ {local_node},
                                 /*preferred_node_score*/ 0));
    // preferred node score is equal to the smallest one.
    EXPECT_EQ(n2,
              policy.GetBestNode(node_scores,
                                 /*num_candidate_nodes*/ 1,
                                 /*preferred_node_id*/ {n2},
                                 /*preferred_node_score*/ 0));
  }
}

TEST_F(HybridSchedulingPolicyTest, CandidateNodesRestrictTheScan) {
  // With candidate_nodes_ set, the policy considers only those nodes; the
  // selection semantics within the set (available bucket first, then
  // feasible-but-unavailable) are unchanged.
  nodes.emplace(local_node, CreateNodeResources(8, 8, 0, 0, 0, 0));
  nodes.emplace(n1, CreateNodeResources(8, 8, 0, 0, 0, 0));
  nodes.emplace(n2, CreateNodeResources(8, 8, 0, 0, 0, 0));
  nodes.emplace(n3, CreateNodeResources(0, 8, 0, 0, 0, 0));
  HybridSchedulingPolicy policy{local_node, nodes, [](auto) { return true; }};

  ResourceRequest request =
      ResourceMapToResourceRequest({{"CPU", 1}}, /*requires_object_store_memory=*/false);

  // Unrestricted, an idle node wins (the local node has the minimal score and
  // is prioritized).
  auto unrestricted = HybridOptions(0.5, false, false);
  EXPECT_EQ(policy.Schedule(request, unrestricted), local_node);

  // Restricted to {n2}, only n2 may be returned even though other nodes are
  // available.
  auto restricted = HybridOptions(0.5, false, false);
  restricted.candidate_nodes_ =
      std::make_shared<const std::vector<scheduling::NodeID>>(
          std::vector<scheduling::NodeID>{n2});
  EXPECT_EQ(policy.Schedule(request, restricted), n2);

  // Restricted to a busy node, the feasible-but-unavailable fallback still
  // applies within the set.
  auto restricted_busy = HybridOptions(0.5, false, false);
  restricted_busy.candidate_nodes_ =
      std::make_shared<const std::vector<scheduling::NodeID>>(
          std::vector<scheduling::NodeID>{n3});
  EXPECT_EQ(policy.Schedule(request, restricted_busy), n3);

  // Restricted to a busy node with require_node_available, nothing matches.
  auto restricted_strict = HybridOptions(0.5, false, true);
  restricted_strict.candidate_nodes_ =
      std::make_shared<const std::vector<scheduling::NodeID>>(
          std::vector<scheduling::NodeID>{n3});
  EXPECT_TRUE(policy.Schedule(request, restricted_strict).IsNil());
}

}  // namespace raylet_scheduling_policy

}  // namespace ray
