"""E2E regression tests for GCS double-counting a placement group's wildcard
resource when a commit is applied to a resource view that already reflects it.

GcsPlacementGroupScheduler::CommitBundleResources computes a wildcard resource's
new capacity as "the bundles in this batch" + "what the resource view says the
node already has" (gcs_placement_group_scheduler.cc). That read-modify-write is
only correct while the view has not yet seen the bundles being committed. Two
things put the view ahead of it:

  - the raylet applies the commit and broadcasts its new resource view over
    ray-syncer ~immediately, while GCS handles the commit reply on its main
    io_context, and
  - after a GCS restart, GcsPlacementGroupManager::Initialize replays the commit
    for every pg left in PREPARED, against a view that was just reseeded from
    the raylets. The raylet's CommitBundle is a silent no-op for a bundle that is
    already COMMITTED (placement_group_resource_manager.cc), so nothing is
    broadcast afterwards that could walk a wrong value back.

Both tests force that ordering with a GCS-side delay on the commit reply
(RAY_testing_asio_delay_us on NodeManagerService.grpc_client.CommitBundleResources
delays OnReplyReceived only -- the raylet still commits and broadcasts at once).
"""

import sys
import time

import pytest

import ray
from ray._common.test_utils import wait_for_condition
from ray.util import placement_group_table

# How long GCS sits on the commit reply. Must be long enough to (a) kill and
# restart GCS inside the window and (b) let the raylet's ray-syncer reconnect
# (2s backoff, then a full push of its cluster view) land first.
COMMIT_REPLY_DELAY_S = 20

_DELAY_US = COMMIT_REPLY_DELAY_S * 1_000_000

SYSTEM_CONFIG = {
    # conftest.get_default_fixure_system_config(), which the fixture param replaces.
    "object_timeout_milliseconds": 200,
    "health_check_initial_delay_ms": 0,
    "health_check_failure_threshold": 10,
    "object_store_full_delay_ms": 100,
    "local_gc_min_interval_s": 1,
    "testing_asio_delay_us": (
        f"NodeManagerService.grpc_client.CommitBundleResources={_DELAY_US}:{_DELAY_US}"
    ),
    # ClusterResourceManager's periodic ResetRemoteNodeView pastes the last raylet
    # snapshot back over any node whose view was modified locally, which papers
    # over the double count a few seconds after it lands. Push it out of the way
    # so these tests assert on the write itself and not on how fast we can read it.
    "ray_syncer_message_refresh_interval_ms": 600_000,
}


def _assert_stays(get_value, expected, seconds=2.0):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        assert get_value() == expected
        time.sleep(0.1)


def test_wildcard_capacity_not_doubled_when_view_already_has_the_bundle(
    ray_start_cluster,
):
    cluster = ray_start_cluster
    cluster.add_node(num_cpus=0, _system_config=SYSTEM_CONFIG)
    ray.init(address=cluster.address)
    cluster.add_node(num_cpus=1)
    cluster.wait_for_nodes()

    pg = ray.util.placement_group([{"CPU": 1}])
    wildcard = f"CPU_group_{pg.id.hex()}"

    # The raylet commits and broadcasts long before GCS gets to the commit reply,
    # so the view already carries this bundle when CommitBundleResources runs.
    wait_for_condition(lambda: wildcard in ray.cluster_resources(), timeout=30)
    assert ray.cluster_resources()[wildcard] == 1.0
    assert placement_group_table(pg)["state"] == "PREPARED"

    # pg.ready() is resolved from the CREATED table write, which happens in the
    # same callback as CommitBundleResources and strictly after it.
    ray.get(pg.ready(), timeout=COMMIT_REPLY_DELAY_S + 60)

    _assert_stays(lambda: ray.cluster_resources()[wildcard], 1.0)
    _assert_stays(lambda: ray.available_resources()[wildcard], 1.0)


@pytest.mark.parametrize(
    "ray_start_cluster_head_with_external_redis",
    [{"num_cpus": 0, "_system_config": SYSTEM_CONFIG}],
    indirect=True,
)
def test_wildcard_capacity_not_doubled_by_commit_replay_after_gcs_restart(
    ray_start_cluster_head_with_external_redis,
):
    cluster = ray_start_cluster_head_with_external_redis
    cluster.add_node(num_cpus=1)
    cluster.wait_for_nodes()

    pg = ray.util.placement_group([{"CPU": 1}], name="wildcard_pg")
    wildcard = f"CPU_group_{pg.id.hex()}"

    # GCS has persisted PREPARED and sent the commit; the raylet has committed and
    # broadcast. GCS has not processed the commit reply, so the wildcard capacity
    # in its view comes purely from the raylet.
    wait_for_condition(
        lambda: placement_group_table(pg)["state"] == "PREPARED", timeout=30
    )
    wait_for_condition(lambda: wildcard in ray.cluster_resources(), timeout=30)
    assert ray.cluster_resources()[wildcard] == 1.0

    # Restart GCS inside that window. Initialize() replays the commit for the
    # PREPARED pg; the raylet's CommitBundle returns early and broadcasts nothing.
    cluster.head_node.kill_gcs_server()
    cluster.head_node.start_gcs_server()

    # The fresh GCS learns the wildcard from the raylet's syncer reconnect push
    # before it processes the replayed commit reply. Asserting this is what keeps
    # the test from passing vacuously: without it the replayed commit would be
    # adding to an empty view and would come out right for the wrong reason.
    wait_for_condition(lambda: wildcard in ray.cluster_resources(), timeout=30)
    assert ray.cluster_resources()[wildcard] == 1.0
    assert placement_group_table(pg)["state"] == "PREPARED"

    wait_for_condition(
        lambda: placement_group_table(pg)["state"] == "CREATED",
        timeout=COMMIT_REPLY_DELAY_S + 60,
        retry_interval_ms=500,
    )

    _assert_stays(lambda: ray.cluster_resources()[wildcard], 1.0)
    _assert_stays(lambda: ray.available_resources()[wildcard], 1.0)


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
