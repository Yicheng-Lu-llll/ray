import sys
import time

import pytest

import ray
from ray._private.test_utils import wait_for_condition


def _gcs_knows(actor):
    return actor._actor_id.hex() in ray._private.state.actors()


@ray.remote(num_cpus=0)
class Counter:
    def __init__(self, x=1):
        self.x = x

    def val(self):
        return self.x


@ray.remote
def slow_value(delay):
    time.sleep(delay)
    return 42


def test_lazy_fleet_basic(ray_start_regular):
    # Anonymous actors work end to end under lazy registration (the create
    # request carries the registration).
    actors = [Counter.remote() for _ in range(20)]
    assert ray.get([a.val.remote() for a in actors]) == [1] * 20


def test_gcs_unaware_while_resolving(ray_start_regular):
    # The lazy core: an anonymous actor whose handle never escapes the owner
    # sends nothing to the GCS while its dependencies resolve. Eager
    # registration would show it as DEPENDENCIES_UNREADY here.
    a = Counter.remote(slow_value.remote(2))
    assert not _gcs_knows(a)
    # Once the dependency resolves, the single create message registers and
    # creates the actor.
    assert ray.get(a.val.remote()) == 42
    assert _gcs_knows(a)


def test_escape_via_task_argument_registers(ray_start_regular):
    # Passing the handle out of the owner while the actor is still resolving
    # its dependencies must register it first: if the owner died now, the
    # borrower's only death notice can come from the GCS.
    @ray.remote
    def borrower(handle):
        return True

    a = Counter.remote(slow_value.remote(30))
    assert not _gcs_knows(a)
    assert ray.get(borrower.remote(a))
    assert _gcs_knows(a)
    # Kill in the still-unresolved window: the GCS knows the actor, so the
    # regular destroy path applies and callers fail fast instead of hanging.
    ray.kill(a)

    def dead():
        try:
            ray.get(a.val.remote(), timeout=2)
            return False
        except ray.exceptions.RayActorError:
            return True

    wait_for_condition(dead, timeout=30)


def test_escape_via_put_registers(ray_start_regular):
    # ray.put of (an object containing) the handle is an escape: the bytes can
    # outlive the owner, so the GCS must know the actor first.
    a = Counter.remote(slow_value.remote(30))
    assert not _gcs_knows(a)
    ray.put([a])
    assert _gcs_knows(a)
    ray.kill(a)


def test_escape_via_task_return_registers(ray_start_regular):
    # A worker task that creates an actor and returns the handle escapes it:
    # the caller becomes a borrower, so the returned handle must be usable and
    # the actor must be registered.
    @ray.remote
    def make_actor():
        return Counter.remote()

    a = ray.get(make_actor.remote())
    assert ray.get(a.val.remote()) == 1
    assert _gcs_knows(a)


def test_kill_immediately_after_create(ray_start_regular):
    # Kill racing the create RPC: the client keeps its registering bookkeeping
    # alive until the create reply, so the kill waits for the GCS to know the
    # actor and reliably terminates it (no lost-kill window).
    for _ in range(3):
        a = Counter.remote()
        ray.kill(a)

        def dead():
            try:
                ray.get(a.val.remote(), timeout=2)
                return False
            except ray.exceptions.RayActorError:
                return True

        wait_for_condition(dead, timeout=30)


def test_named_actor_still_eager(ray_start_regular):
    # Named actors keep the synchronous eager registration: they are visible
    # to the GCS right after .remote() even while their dependencies resolve.
    named = Counter.options(name="lazy_named").remote(slow_value.remote(5))
    assert _gcs_knows(named)
    assert ray.get(ray.get_actor("lazy_named").val.remote()) == 42


def test_kill_unschedulable_actor_returns_promptly(ray_start_regular):
    # Adversarial-review regression (deadlock): the create RPC of an
    # unschedulable actor never replies until the actor is destroyed, and the
    # destroy can only come from this kill. The kill must therefore wait only
    # for the escape-triggered standalone registration (milliseconds), never
    # for the create reply.
    a = Counter.options(num_gpus=1).remote()  # cluster has no GPU
    # The create RPC has reached the GCS once its inline registration lands.
    wait_for_condition(lambda: _gcs_knows(a))
    start = time.time()
    ray.kill(a)
    assert time.time() - start < 10

    def dead():
        try:
            ray.get(a.val.remote(), timeout=2)
            return False
        except ray.exceptions.RayActorError:
            return True

    wait_for_condition(dead, timeout=30)


def test_escape_during_create_flight_is_fast(ray_start_regular):
    # An escape while the create RPC is in flight resolves via a standalone
    # (idempotent) registration in one round trip; it must not wait out the
    # actor creation (unbounded here: the actor is unschedulable).
    a = Counter.options(num_gpus=1).remote()
    # The create RPC has reached the GCS once its inline registration lands.
    wait_for_condition(lambda: _gcs_knows(a))
    start = time.time()
    ray.put([a])
    assert time.time() - start < 10
    assert _gcs_knows(a)
    ray.kill(a)


@pytest.mark.parametrize(
    "ray_start_regular",
    # Dead-actor cache of one: another death evicts the killed actor's entry,
    # so a create that escaped the client-side tombstone would really
    # resurrect it -- this makes the tombstone the load-bearing defense
    # instead of a redundant fast path in front of the cache. (A cache of
    # zero would be simpler but crashes the GCS on the first destruction,
    # independent of this change.)
    [{"_system_config": {"maximum_gcs_destroyed_actor_cached_count": 1}}],
    indirect=True,
)
def test_kill_before_create_send_no_zombie(ray_start_regular):
    # Kill while the dependencies are still resolving: the kill registers and
    # destroys the actor on the GCS, and the later dependency-resolved create
    # is suppressed client-side (tombstone), so the actor can never be
    # resurrected -- not even after the GCS dead-actor cache evicts it.
    dep = slow_value.remote(4)
    a = Counter.remote(dep)
    ray.kill(a)

    # Push the killed actor's entry out of the size-one dead-actor cache
    # before the dependencies resolve.
    dummy = Counter.remote()
    ray.get(dummy.val.remote())
    ray.kill(dummy)
    wait_for_condition(lambda: not _gcs_knows(a))

    ray.get(dep)  # dependencies resolve; the suppressed create must not run
    # A create that slipped through would re-register the actor -- nothing on
    # the GCS is left to reject it -- and resurrect it as a fresh ALIVE actor,
    # which is immediately visible as a new GCS record. Give a would-be create
    # ample time to land, then require silence (a negative needs a window).
    time.sleep(3)
    assert not _gcs_knows(a)
    # NOTE: no assertion on calls to the killed handle here: once the dead
    # record is evicted, a late subscriber gets no death notice and the call
    # hangs -- a pre-existing corner shared with eager registration.
    # test_kill_immediately_after_create covers the error surfacing while the
    # record exists.


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
