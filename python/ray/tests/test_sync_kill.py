import sys

import pytest

import ray
from ray._private.test_utils import wait_for_condition


@ray.remote(num_cpus=0)
class Named:
    def ping(self):
        return 1

    def die(self):
        ray.actor.exit_actor()


def test_kill_returns_ref_and_frees_name(ray_start_regular):
    # ray.kill returns an ObjectRef; after ray.get it, recreating a same-named
    # actor must not hit AlreadyExists.
    for i in range(20):
        a = Named.options(name="w", namespace="ns").remote()
        assert ray.get(a.ping.remote()) == 1
        ref = ray.kill(a)
        assert isinstance(ref, ray.ObjectRef)
        assert ray.get(ref, timeout=30) is None  # resolves to None, does not raise
        # Immediate recreate under the same name — the whole point.
        b = Named.options(name="w", namespace="ns").remote()
        assert ray.get(b.ping.remote()) == 1
        ray.get(ray.kill(b), timeout=30)


def test_kill_no_restart_false_restarts_and_keeps_name(ray_start_regular):
    # no_restart=False: the ref resolves once the GCS has processed the kill,
    # but the actor restarts and its name is NOT released.
    a = Named.options(name="r", namespace="ns", max_restarts=1).remote()
    assert ray.get(a.ping.remote()) == 1
    ref = ray.kill(a, no_restart=False)
    assert ray.get(ref, timeout=30) is None
    # The actor comes back under the same handle.
    wait_for_condition(lambda: ray.get(a.ping.remote(), timeout=5) == 1, timeout=30)
    # The name was never released: a same-named create must fail.
    with pytest.raises(ray.exceptions.ActorAlreadyExistsError, match="already taken"):
        Named.options(name="r", namespace="ns").remote()


def test_kill_detached(ray_start_regular):
    # Replace flow on a detached named actor (the common service-actor case).
    a = Named.options(name="d", namespace="ns", lifetime="detached").remote()
    assert ray.get(a.ping.remote()) == 1
    ray.get(ray.kill(a), timeout=30)
    b = Named.options(name="d", namespace="ns", lifetime="detached").remote()
    assert ray.get(b.ping.remote()) == 1
    ray.get(ray.kill(b), timeout=30)


def test_kill_already_dead_succeeds(ray_start_regular):
    # Killing an already-dead actor succeeds: the GCS reports the actor is
    # already gone (name already free), so the ref resolves to None.
    a = Named.options(name="t").remote()
    assert ray.get(a.ping.remote()) == 1
    ray.get(ray.kill(a), timeout=30)
    ref2 = ray.kill(a)
    assert ray.get(ref2, timeout=30) is None
    # Name is reusable.
    b = Named.options(name="t").remote()
    assert ray.get(b.ping.remote()) == 1


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
