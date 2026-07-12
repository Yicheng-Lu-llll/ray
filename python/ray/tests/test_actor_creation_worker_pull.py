import json
import sys

import pytest

import ray
from ray._common.test_utils import wait_for_condition
from ray._private.test_utils import (
    RPC_FAILURE_MAP,
    RPC_FAILURE_TYPES,
)

# The three RPCs of the worker-pull actor creation protocol
# (gcs_actor_creation_worker_pull_enabled):
#   raylet -> worker signal, worker -> GCS spec fetch, worker -> GCS outcome report.
PULL_PROTOCOL_RPCS = [
    "ray::rpc::CoreWorkerService.grpc_client.PullActorCreationTask",
    "ray::rpc::ActorInfoGcsService.grpc_client.GetActorCreationTaskSpec",
    "ray::rpc::ActorInfoGcsService.grpc_client.ReportActorCreationDone",
]


@pytest.mark.parametrize("deterministic_failure", RPC_FAILURE_TYPES)
def test_worker_pull_creation_with_rpc_failures(
    monkeypatch, ray_start_cluster, deterministic_failure
):
    # Every RPC of the pull protocol fails once (request or response side) and
    # must recover through its retry path:
    #   - the raylet's signal is retried (retryable client); a response-side
    #     failure additionally delivers a duplicate signal, which the worker's
    #     dedup guard must swallow without double-reporting;
    #   - the spec fetch and the outcome report go through the GCS client's
    #     standard retry; a retried report hits the GCS's idempotent ack.
    failure = RPC_FAILURE_MAP[deterministic_failure].copy()
    failure["num_failures"] = 1
    monkeypatch.setenv(
        "RAY_testing_rpc_failure",
        json.dumps({rpc: failure for rpc in PULL_PROTOCOL_RPCS}),
    )
    cluster = ray_start_cluster
    cluster.add_node(
        num_cpus=2,
        _system_config={"gcs_actor_creation_worker_pull_enabled": True},
    )
    ray.init(address=cluster.address)

    @ray.remote(num_cpus=1, max_restarts=1)
    class Actor:
        def ping(self):
            import os

            return os.getpid()

    actors = [Actor.remote() for _ in range(2)]
    pids = ray.get([a.ping.remote() for a in actors], timeout=120)
    assert len(set(pids)) == 2

    # The restart path re-runs the whole pull protocol (new lease -> new signal).
    # ray.kill is async: an immediate ping can still be served by the dying
    # process, so wait until the answering pid changes.
    ray.kill(actors[0], no_restart=False)

    def restarted():
        try:
            return ray.get(actors[0].ping.remote(), timeout=10) != pids[0]
        except ray.exceptions.RayActorError:
            return False

    wait_for_condition(restarted, timeout=120)


def test_worker_pull_creation_restart_and_del(ray_start_cluster):
    # Flag-on lifecycle without injected failures: burst create, restart, and
    # out-of-scope destroy while other creations are in flight.
    cluster = ray_start_cluster
    cluster.add_node(
        num_cpus=4,
        _system_config={"gcs_actor_creation_worker_pull_enabled": True},
    )
    ray.init(address=cluster.address)

    @ray.remote(num_cpus=0, max_restarts=1)
    class Actor:
        def ping(self):
            return 1

    actors = [Actor.remote() for _ in range(20)]
    assert ray.get([a.ping.remote() for a in actors], timeout=120) == [1] * 20

    # Drop a handle: the GCS destroys the actor through the same paths as the
    # push protocol (creation was already reported done), and the survivors are
    # unaffected.
    dead = actors.pop(0)
    dead_id = dead._actor_id
    del dead

    from ray.core.generated import gcs_pb2

    def actor_dead():
        info = ray._private.state.actors(actor_id=dead_id.hex())
        return info["State"] == gcs_pb2.ActorTableData.ActorState.Name(
            gcs_pb2.ActorTableData.DEAD
        )

    wait_for_condition(actor_dead, timeout=60)
    assert ray.get([a.ping.remote() for a in actors], timeout=60) == [1] * 19


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
