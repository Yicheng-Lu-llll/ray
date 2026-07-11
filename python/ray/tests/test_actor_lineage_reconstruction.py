import gc
import json
import os
import signal
import sys

import pytest

import ray
from ray._common.test_utils import wait_for_condition
from ray._private.test_utils import (
    RPC_FAILURE_MAP,
    RPC_FAILURE_TYPES,
)
from ray.core.generated import common_pb2, gcs_pb2


@pytest.mark.parametrize("deterministic_failure", RPC_FAILURE_TYPES)
@pytest.mark.parametrize("actor_ref_deleted_push_enabled", [False, True])
def test_actor_reconstruction_triggered_by_lineage_reconstruction(
    monkeypatch,
    ray_start_cluster,
    deterministic_failure,
    actor_ref_deleted_push_enabled,
):
    # Test the sequence of events:
    # actor goes out of scope and killed
    # -> lineage reconstruction triggered by object lost
    # -> actor is restarted
    # -> actor goes out of scope again after lineage reconstruction is done
    # -> actor is permanently dead when there is no reference.
    # This test also injects network failure to make sure relevant rpcs are retried.
    # When actor_ref_deleted_push_enabled is set, the final REF_DELETED transition
    # is reported by the owner via ReportActorRefDeleted instead of the GCS's
    # per-actor WaitForActorRefDeleted long-poll on the owner.
    failure = RPC_FAILURE_MAP[deterministic_failure].copy()
    failure["num_failures"] = 1
    monkeypatch.setenv(
        "RAY_testing_rpc_failure",
        json.dumps(
            {
                "ray::rpc::ActorInfoGcsService.grpc_client.RestartActorForLineageReconstruction": failure,
                "ray::rpc::ActorInfoGcsService.grpc_client.ReportActorOutOfScope": failure,
                "ray::rpc::ActorInfoGcsService.grpc_client.ReportActorRefDeleted": failure,
            }
        ),
    )
    cluster = ray_start_cluster
    cluster.add_node(
        resources={"head": 1},
        _system_config={
            "actor_ref_deleted_push_enabled": actor_ref_deleted_push_enabled
        },
    )
    ray.init(address=cluster.address)
    worker1 = cluster.add_node(resources={"worker": 1})

    @ray.remote(
        num_cpus=1, resources={"worker": 1}, max_restarts=-1, max_task_retries=-1
    )
    class Actor:
        def ping(self):
            return [1] * 1024 * 1024

        def pid(self):
            return os.getpid()

    actor = Actor.remote()
    actor_id = actor._actor_id

    obj1 = actor.ping.remote()
    os.kill(ray.get(actor.pid.remote()), signal.SIGKILL)

    # obj2 should be ready after actor is restarted
    obj2 = actor.ping.remote()

    # Make the actor out of scope
    actor = None

    def verify1():
        gc.collect()
        actor_info = ray._private.state.state.get_actor_info(actor_id)
        assert actor_info is not None
        actor_info = gcs_pb2.ActorTableData.FromString(actor_info)
        assert actor_info.state == gcs_pb2.ActorTableData.ActorState.DEAD
        assert (
            actor_info.death_cause.actor_died_error_context.reason
            == common_pb2.ActorDiedErrorContext.Reason.OUT_OF_SCOPE
        )
        assert actor_info.num_restarts_due_to_lineage_reconstruction == 0
        return True

    wait_for_condition(lambda: verify1())

    # objs will be lost and recovered
    # during the process, actor will be reconstructured
    # and dead again after lineage reconstruction finishes
    cluster.remove_node(worker1)
    cluster.add_node(resources={"worker": 1})

    assert ray.get(obj1) == [1] * 1024 * 1024
    assert ray.get(obj2) == [1] * 1024 * 1024

    def verify2():
        actor_info = ray._private.state.state.get_actor_info(actor_id)
        assert actor_info is not None
        actor_info = gcs_pb2.ActorTableData.FromString(actor_info)
        assert actor_info.state == gcs_pb2.ActorTableData.ActorState.DEAD
        assert (
            actor_info.death_cause.actor_died_error_context.reason
            == common_pb2.ActorDiedErrorContext.Reason.OUT_OF_SCOPE
        )
        # 1 restart recovers two objects
        assert actor_info.num_restarts_due_to_lineage_reconstruction == 1
        return True

    wait_for_condition(lambda: verify2())

    # actor can be permanently dead since no lineage reconstruction will happen
    del obj1
    del obj2

    def verify3():
        actor_info = ray._private.state.state.get_actor_info(actor_id)
        assert actor_info is not None
        actor_info = gcs_pb2.ActorTableData.FromString(actor_info)
        assert actor_info.state == gcs_pb2.ActorTableData.ActorState.DEAD
        assert (
            actor_info.death_cause.actor_died_error_context.reason
            == common_pb2.ActorDiedErrorContext.Reason.REF_DELETED
        )
        assert actor_info.num_restarts_due_to_lineage_reconstruction == 1
        return True

    wait_for_condition(lambda: verify3())


@pytest.mark.parametrize("actor_ref_deleted_push_enabled", [False, True])
def test_del_actor_destroys_actor_and_releases_name(
    ray_start_cluster, actor_ref_deleted_push_enabled
):
    # Deleting all handles destroys the actor permanently (REF_DELETED) and
    # releases its name, under both the long-poll and the owner-push protocols.
    # max_restarts=-1 keeps the OUT_OF_SCOPE-dead actor restartable, so reaching
    # the permanent REF_DELETED state (and releasing the name) strictly requires
    # the ref-deleted signal to be delivered — which is exactly the protocol
    # under test. (With max_restarts=0 the actor is erased at OUT_OF_SCOPE and
    # the death cause stays OUT_OF_SCOPE.)
    cluster = ray_start_cluster
    cluster.add_node(
        num_cpus=2,
        _system_config={
            "actor_ref_deleted_push_enabled": actor_ref_deleted_push_enabled
        },
    )
    ray.init(address=cluster.address)

    @ray.remote(num_cpus=0, max_restarts=-1)
    class Actor:
        def ping(self):
            return 1

    def assert_dead_with_ref_deleted(actor_id):
        gc.collect()
        actor_info = ray._private.state.state.get_actor_info(actor_id)
        assert actor_info is not None
        actor_info = gcs_pb2.ActorTableData.FromString(actor_info)
        assert actor_info.state == gcs_pb2.ActorTableData.ActorState.DEAD
        assert (
            actor_info.death_cause.actor_died_error_context.reason
            == common_pb2.ActorDiedErrorContext.Reason.REF_DELETED
        )
        return True

    # Anonymous actor: del -> permanently destroyed.
    actor = Actor.remote()
    actor_id = actor._actor_id
    ray.get(actor.ping.remote())
    del actor
    wait_for_condition(lambda: assert_dead_with_ref_deleted(actor_id))

    # Immediate del right after creation (before the actor is ever used): the
    # ref-deleted event may fire before/while the owner installs the callback;
    # the actor must still be destroyed eventually.
    early = Actor.remote()
    early_id = early._actor_id
    del early
    wait_for_condition(lambda: assert_dead_with_ref_deleted(early_id))

    # Named actor: del -> destroyed and the name becomes reusable.
    named = Actor.options(name="ref_deleted_push_named").remote()
    named_id = named._actor_id
    ray.get(named.ping.remote())
    del named
    wait_for_condition(lambda: assert_dead_with_ref_deleted(named_id))

    def name_reusable():
        try:
            reborn = Actor.options(name="ref_deleted_push_named").remote()
        except ValueError:
            return False
        ray.get(reborn.ping.remote())
        return True

    wait_for_condition(name_reusable)


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
