import os
import sys

import pytest

import ray


@ray.remote(num_cpus=0, max_restarts=1)
class Proc:
    def info(self):
        return (os.getpid(), os.getppid())

    def ping(self):
        return 1


def _raylet_pid():
    return int(os.popen("pgrep -x raylet | head -1").read().strip())


@pytest.mark.skipif(sys.platform != "linux", reason="fork server is Linux-only")
def test_workers_forked_from_template(monkeypatch, ray_start_cluster):
    monkeypatch.setenv("RAY_raylet_fork_worker_from_template", "1")
    cluster = ray_start_cluster
    cluster.add_node(num_cpus=4)
    ray.init(address=cluster.address)
    raylet = _raylet_pid()

    from ray._common.test_utils import wait_for_condition

    # While the template is still importing ray, spawns fall back to exec by
    # design (the connect attempt is non-blocking). Wait until a probe worker
    # is fork-born before asserting on a full batch.
    def fork_server_serving():
        a = Proc.remote()
        try:
            _, ppid = ray.get(a.info.remote(), timeout=60)
        finally:
            ray.kill(a)
        return ppid != raylet

    wait_for_condition(fork_server_serving, timeout=60, retry_interval_ms=500)

    actors = [Proc.remote() for _ in range(8)]
    infos = ray.get([a.info.remote() for a in actors], timeout=120)
    # All plain Python workers come from the fork server, so their parent is
    # the fork-server process, not the raylet.
    assert all(ppid != raylet for _, ppid in infos), infos
    # They share one parent (the fork server).
    assert len({ppid for _, ppid in infos}) == 1

    # Tasks and objects work on forked workers.
    @ray.remote
    def f(x):
        return x * 2

    assert ray.get(f.remote(21)) == 42
    obj = ray.put("data")
    assert ray.get(obj) == "data"

    # The restart path re-runs the fork spawn.
    old_pid = infos[0][0]
    ray.kill(actors[0], no_restart=False)

    from ray._common.test_utils import wait_for_condition

    def restarted():
        try:
            pid, ppid = ray.get(actors[0].info.remote(), timeout=10)
            return pid != old_pid and ppid != raylet
        except ray.exceptions.RayActorError:
            return False

    wait_for_condition(restarted, timeout=120)


@pytest.mark.skipif(sys.platform != "linux", reason="fork server is Linux-only")
def test_runtime_env_workers_fall_back_to_exec(monkeypatch, ray_start_cluster):
    # Workers with a runtime env context are not fork-eligible: they must go
    # through the setup_worker/exec path and still work.
    monkeypatch.setenv("RAY_raylet_fork_worker_from_template", "1")
    cluster = ray_start_cluster
    cluster.add_node(num_cpus=4)
    ray.init(address=cluster.address)

    a = Proc.options(runtime_env={"env_vars": {"MY_TEST_VAR": "42"}}).remote()
    assert ray.get(a.ping.remote(), timeout=120) == 1

    @ray.remote(runtime_env={"env_vars": {"MY_TEST_VAR": "43"}})
    def get_var():
        return os.environ.get("MY_TEST_VAR")

    assert ray.get(get_var.remote(), timeout=120) == "43"


def test_flag_off_workers_are_raylet_children(ray_start_cluster):
    cluster = ray_start_cluster
    cluster.add_node(num_cpus=4)
    ray.init(address=cluster.address)

    infos = ray.get([Proc.remote().info.remote() for _ in range(4)], timeout=120)
    raylet = _raylet_pid()
    assert all(ppid == raylet for _, ppid in infos), infos


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
