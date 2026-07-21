import sys
import time

import pytest

import ray
from ray._private.test_utils import wait_for_condition

# Big enough to matter, small enough to stay inlined in the task spec (the
# default inline limit is 100KB): the registration request drops it, the
# create request carries it.
BIG_ARG = b"x" * 50_000


def _state(actor):
    info = ray._private.state.actors().get(actor._actor_id.hex())
    return None if info is None else info["State"]


@ray.remote(num_cpus=0)
class Holder:
    def __init__(self, payload=b""):
        self.payload = payload

    def size(self):
        return len(self.payload)

    def crash(self):
        import os

        os._exit(1)


@ray.remote
def slow_value(delay):
    time.sleep(delay)
    return b"y" * 1000


def test_slim_big_inline_arg_survives(ray_start_regular):
    # The registration request drops the (inlined) constructor arguments; the
    # create request still carries them, so __init__ must see the full value.
    holders = [Holder.remote(BIG_ARG) for _ in range(10)]
    assert ray.get([h.size.remote() for h in holders]) == [len(BIG_ARG)] * 10


def test_slim_visibility_unchanged(ray_start_regular):
    # Unlike lazy registration, slim registration keeps the eager protocol:
    # the actor is registered (and visible) while its dependencies resolve.
    a = Holder.remote(slow_value.remote(3))
    wait_for_condition(lambda: _state(a) == "DEPENDENCIES_UNREADY", timeout=10)
    assert ray.get(a.size.remote()) == 1000
    assert _state(a) == "ALIVE"


def test_slim_named_actor_immediately_visible(ray_start_regular):
    named = Holder.options(name="slim_named").remote(slow_value.remote(3))
    assert ray.get_actor("slim_named") is not None
    assert ray.get(named.size.remote()) == 1000


def test_slim_detached_actor(ray_start_regular):
    # Named and detached actors are excluded from slimming entirely (they are
    # queryable by strangers); this is a full-registration sanity check.
    d = Holder.options(name="slim_detached", lifetime="detached").remote(BIG_ARG)
    assert ray.get(d.size.remote()) == len(BIG_ARG)
    assert ray.get(ray.get_actor("slim_detached").size.remote()) == len(BIG_ARG)
    ray.kill(d)


def test_slim_kill_while_resolving(ray_start_regular):
    a = Holder.remote(slow_value.remote(30))
    ray.kill(a)

    def dead():
        try:
            ray.get(a.size.remote(), timeout=2)
            return False
        except ray.exceptions.RayActorError:
            return True

    wait_for_condition(dead, timeout=30)


def test_slim_restart_reruns_init_with_args(ray_start_regular):
    # Restart re-runs the creation task from the spec the GCS holds; under
    # slim registration that spec is the creation-time (resolved) one, so the
    # restarted __init__ must still receive the full argument.
    h = Holder.options(max_restarts=1).remote(BIG_ARG)
    assert ray.get(h.size.remote()) == len(BIG_ARG)
    with pytest.raises(ray.exceptions.RayActorError):
        ray.get(h.crash.remote())

    def restarted():
        try:
            return ray.get(h.size.remote(), timeout=2) == len(BIG_ARG)
        except ray.exceptions.RayActorError:
            return False

    wait_for_condition(restarted, timeout=30)


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
