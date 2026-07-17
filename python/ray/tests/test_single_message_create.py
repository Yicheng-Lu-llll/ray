import sys

import pytest

import ray
from ray._private.test_utils import wait_for_condition

SINGLE_MESSAGE_CFG = {
    "_system_config": {"actor_single_message_create_enabled": True}
}


@pytest.mark.parametrize("ray_start_regular", [SINGLE_MESSAGE_CFG], indirect=True)
def test_single_message_create_basic(ray_start_regular):
    # Dependency-free anonymous actors work end to end with the flag on.
    @ray.remote(num_cpus=0)
    class A:
        def ping(self):
            return 1

    actors = [A.remote() for _ in range(20)]
    assert ray.get([a.ping.remote() for a in actors]) == [1] * 20


@pytest.mark.parametrize("ray_start_regular", [SINGLE_MESSAGE_CFG], indirect=True)
def test_kill_immediately_after_create(ray_start_regular):
    # Regression: the client keeps its registering bookkeeping alive over the
    # create RPC, so a kill issued right after .remote() waits until the GCS
    # knows the actor and reliably terminates it (no lost-kill window).
    @ray.remote(num_cpus=0)
    class A:
        def ping(self):
            return 1

    for _ in range(5):
        a = A.remote()
        ray.kill(a)

        def dead():
            try:
                ray.get(a.ping.remote(), timeout=2)
                return False
            except ray.exceptions.RayActorError:
                return True

        wait_for_condition(dead, timeout=30)


@pytest.mark.parametrize("ray_start_regular", [SINGLE_MESSAGE_CFG], indirect=True)
def test_named_and_dependency_actors_use_old_path(ray_start_regular):
    # Named actors and actors with ObjectRef args keep the two-message path and
    # still work with the flag on.
    @ray.remote(num_cpus=0)
    class A:
        def __init__(self, x=1):
            self.x = x

        def val(self):
            return self.x

    named = A.options(name="smc_named").remote()
    assert ray.get(named.val.remote()) == 1
    dep = A.remote(ray.put(42))
    assert ray.get(dep.val.remote()) == 42


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
