"""release/benchmarks/object_store/test_small_objects.py plus a per-thread CPU probe.

Drop-in replacement for the release test script (experiment builds only). The benchmark
loop is byte-identical to the original; around the many_to_one window it samples
utime+stime of every thread of the driver, of the head raylet, and of each worker
raylet (one num_cpus=0 task pinned per node), and writes them into the same
TEST_OUTPUT_JSON the release harness already uploads as result.json. No new log
channel is needed: the numbers ride the artifact we already collect.
"""
import json
import os
import subprocess
import time

import numpy as np

import ray
from ray.util.scheduling_strategies import NodeAffinitySchedulingStrategy

JIFFIES = os.sysconf("SC_CLK_TCK")


def _raylet_pid():
    out = subprocess.run(["pgrep", "-f", "raylet/raylet"], capture_output=True, text=True).stdout
    pids = [int(x) for x in out.split() if x.strip()]
    return pids[0] if pids else None


def _snap(pid):
    """{tid: (thread_name, cpu_jiffies)} for one process."""
    res = {}
    if pid is None:
        return res
    base = f"/proc/{pid}/task"
    try:
        tids = os.listdir(base)
    except OSError:
        return res
    for tid in tids:
        try:
            with open(f"{base}/{tid}/stat") as f:
                stat = f.read()
            name = stat[stat.index("(") + 1 : stat.rindex(")")]
            fields = stat[stat.rindex(")") + 2 :].split()
            res[tid] = (name, int(fields[11]) + int(fields[12]))  # utime + stime
        except (OSError, ValueError):
            continue
    return res


def _delta(a, b, seconds):
    """Per-thread % of one core between two snapshots, sorted, top 12."""
    rows = []
    for tid, (name, t1) in b.items():
        if tid in a:
            rows.append((name, round((t1 - a[tid][1]) / JIFFIES / seconds * 100, 1)))
    rows.sort(key=lambda r: -r[1])
    return rows[:12]


@ray.remote(num_cpus=0)
def sample_raylet_threads(seconds):
    import socket

    pid = _raylet_pid()
    a = _snap(pid)
    time.sleep(seconds)
    b = _snap(pid)
    return {"ip": socket.gethostbyname(socket.gethostname()), "raylet_pid": pid,
            "threads": _delta(a, b, seconds)}


def test_small_objects_many_to_one(probe):
    @ray.remote(num_cpus=1)
    class Actor:
        def send(self, _, actor_idx):
            # this size is chosen because it's >100kb so big enough to be stored in plasma
            numpy_arr = np.ones((20, 1024))
            return (numpy_arr, actor_idx)

    actors = [Actor.remote() for _ in range(64)]
    not_ready = []
    for index, actor in enumerate(actors):
        not_ready.append(actor.send.remote(0, index))

    # --- probe: start sampling once actors are up, over the middle 40 s of the 60 s window
    head_ip = ray.util.get_node_ip_address()
    workers = [n for n in ray.nodes() if n["Alive"] and n["NodeManagerAddress"] != head_ip]
    worker_refs = [
        sample_raylet_threads.options(
            scheduling_strategy=NodeAffinitySchedulingStrategy(node_id=n["NodeID"], soft=False)
        ).remote(40)
        for n in workers
    ]
    drv_pid, ray_pid = os.getpid(), _raylet_pid()
    drv_a = ray_a = None
    probe_t0 = None

    num_messages = 0
    start_time = time.time()
    while time.time() - start_time < 60:
        ready, not_ready = ray.wait(not_ready, num_returns=10)
        for ready_ref in ready:
            _, actor_idx = ray.get(ready_ref)
            not_ready.append(actors[actor_idx].send.remote(0, actor_idx))
        num_messages += 10
        now = time.time()
        if drv_a is None and now - start_time >= 10:
            drv_a, ray_a, probe_t0 = _snap(drv_pid), _snap(ray_pid), now
        elif probe_t0 is not None and "head" not in probe and now - probe_t0 >= 40:
            probe["head"] = {"driver": _delta(drv_a, _snap(drv_pid), now - probe_t0),
                             "raylet": _delta(ray_a, _snap(ray_pid), now - probe_t0)}
    if "head" not in probe and drv_a is not None:
        now = time.time()
        probe["head"] = {"driver": _delta(drv_a, _snap(drv_pid), now - probe_t0),
                         "raylet": _delta(ray_a, _snap(ray_pid), now - probe_t0)}
    probe["workers"] = ray.get(worker_refs)
    return num_messages / 60


def test_small_objects_one_to_many():
    @ray.remote(num_cpus=1)
    class Actor:
        def receive(self, numpy_arr, actor_idx):
            return actor_idx

    actors = [Actor.remote() for _ in range(64)]
    numpy_arr_ref = ray.put(np.ones((20, 1024)))
    not_ready = []

    num_messages = 0
    start_time = time.time()
    for idx, actor in enumerate(actors):
        not_ready.append(actor.receive.remote(numpy_arr_ref, idx))
    while time.time() - start_time < 60:
        ready, not_ready = ray.wait(not_ready, num_returns=10)
        actor_idxs = ray.get(ready)
        for actor_idx in actor_idxs:
            not_ready.append(actors[actor_idx].receive.remote(numpy_arr_ref, actor_idx))
        num_messages += 10
    return num_messages / 60


ray.init(address="auto")
probe = {}
many_to_one_throughput = test_small_objects_many_to_one(probe)
print(f"Number of messages per second many_to_one: {many_to_one_throughput}")
print("THREAD_CPU_PROBE " + json.dumps(probe))
one_to_many_throughput = test_small_objects_one_to_many()
print(f"Number of messages per second one_to_many: {one_to_many_throughput}")


if "TEST_OUTPUT_JSON" in os.environ:
    with open(os.environ["TEST_OUTPUT_JSON"], "w") as out_file:
        results = {
            "num_messages_many_to_one": many_to_one_throughput,
            "num_messages_one_to_many": one_to_many_throughput,
            "thread_cpu_probe": probe,
        }
        results["perf_metrics"] = [
            {
                "perf_metric_name": "num_small_objects_many_to_one",
                "perf_metric_value": many_to_one_throughput,
                "perf_metric_type": "THROUGHPUT",
            },
            {
                "perf_metric_name": "num_small_objects_one_to_many_per_second",
                "perf_metric_value": one_to_many_throughput,
                "perf_metric_type": "THROUGHPUT",
            },
        ]
        json.dump(results, out_file)
