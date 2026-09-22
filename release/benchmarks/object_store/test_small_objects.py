"""release/benchmarks/object_store/test_small_objects.py plus a latency-decomposition probe.

Same benchmark loop as the original; each ray.wait / ray.get / .remote() call in the
many_to_one loop is timed with perf_counter and the totals go into TEST_OUTPUT_JSON as
results.latency_probe. Everything not inside those three calls (including ObjectRef
destructors, which run when `ready` / `ready_ref` are rebound) is `other`.
"""
import json
import os
import time
from time import perf_counter

import numpy as np

import ray


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

    t_wait = t_get = t_submit = 0.0
    iters = 0
    num_messages = 0
    start_time = time.time()
    loop_t0 = perf_counter()
    while time.time() - start_time < 60:
        t0 = perf_counter()
        ready, not_ready = ray.wait(not_ready, num_returns=10)
        t1 = perf_counter()
        t_wait += t1 - t0
        for ready_ref in ready:
            g0 = perf_counter()
            _, actor_idx = ray.get(ready_ref)
            g1 = perf_counter()
            not_ready.append(actors[actor_idx].send.remote(0, actor_idx))
            g2 = perf_counter()
            t_get += g1 - g0
            t_submit += g2 - g1
        num_messages += 10
        iters += 1
    total = perf_counter() - loop_t0
    us = lambda sec: round(sec / max(num_messages, 1) * 1e6, 1)
    probe.update({
        "loop_total_s": round(total, 3), "iterations": iters, "objects": num_messages,
        "wait_s": round(t_wait, 3), "get_s": round(t_get, 3), "submit_s": round(t_submit, 3),
        "other_s": round(total - t_wait - t_get - t_submit, 3),
        "per_object_us": {"wait": us(t_wait), "get": us(t_get), "submit": us(t_submit),
                          "other": us(total - t_wait - t_get - t_submit),
                          "cycle": us(total)},
        "inflight": 64,
        "round_trip_estimate_ms": round(64 * total / max(num_messages, 1) * 1e3, 2),
    })
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
print("LATENCY_PROBE " + json.dumps(probe))
one_to_many_throughput = test_small_objects_one_to_many()
print(f"Number of messages per second one_to_many: {one_to_many_throughput}")


if "TEST_OUTPUT_JSON" in os.environ:
    with open(os.environ["TEST_OUTPUT_JSON"], "w") as out_file:
        results = {
            "num_messages_many_to_one": many_to_one_throughput,
            "num_messages_one_to_many": one_to_many_throughput,
            "latency_probe": probe,
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
