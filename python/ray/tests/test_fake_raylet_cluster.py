import sys
import time

import pytest

import ray
from ray._private.fake_raylet_cluster import FakeRayletCluster


def test_fake_raylets_register_and_survive_health_checks(ray_start_regular):
    n = 20
    cluster = FakeRayletCluster(
        num_nodes=n,
        cpus_per_node=4,
        labels_fn=lambda i: {"ray.io/gpu-domain": f"rack-{i // 5}"},
    ).start()
    try:
        # Wait past the health-check initial delay plus a few periods; the fake
        # nodes must stay ALIVE because the shared server answers health checks.
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            alive = [x for x in ray.nodes() if x["Alive"]]
            fake = [x for x in alive if x["NodeName"].startswith("fake-raylet-")]
            if len(fake) == n and time.monotonic() > deadline - 15:
                break
            time.sleep(1)
        alive = [x for x in ray.nodes() if x["Alive"]]
        fake = [x for x in alive if x["NodeName"].startswith("fake-raylet-")]
        assert len(fake) == n, f"expected {n} fake nodes alive, got {len(fake)}"
        # Labels made it into the node table.
        labeled = [x for x in fake if x.get("Labels", {}).get("ray.io/gpu-domain")]
        assert len(labeled) == n
        # Resources made it into the view.
        assert all(x["Resources"].get("CPU") == 4 for x in fake)
    finally:
        cluster.stop()


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
