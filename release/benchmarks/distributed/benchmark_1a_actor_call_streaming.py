"""
Benchmark 1A: Actor Call -- Streaming

Usage:
  python benchmark_1a_actor_call_streaming.py --num-actors 100 --call-duration 1.0
"""

import argparse
import json
import time

import numpy as np

import ray

# Ray Data's DEFAULT_TARGET_MAX_BLOCK_SIZE.
BLOCK_SIZE_BYTES = 128 * 1024 * 1024

# Ray Data's default: max_concurrency(1) * DEFAULT_ACTOR_MAX_TASKS_IN_FLIGHT_TO_MAX_CONCURRENCY_FACTOR(2).
MAX_INFLIGHT_PER_ACTOR = 2


@ray.remote(num_cpus=1)
class InferenceActor:
    def __init__(self):
        self._block = np.zeros(BLOCK_SIZE_BYTES, dtype=np.uint8)

    def ready(self):
        return True

    def process(self, duration_s):
        time.sleep(duration_s)
        yield self._block


def run_benchmark(
    num_actors,
    call_duration,
    measure_s=60.0,
):
    theoretical_max = num_actors / call_duration

    warmup_s = max(30.0, call_duration * 3)

    actors = [
        InferenceActor.options(scheduling_strategy="SPREAD").remote()
        for _ in range(num_actors)
    ]
    ray.get([a.ready.remote() for a in actors])

    num_nodes = sum(1 for n in ray.nodes() if n["Alive"])
    print(f"Cluster: {num_nodes} nodes, ~{num_actors / num_nodes:.1f} actors/node")

    task_to_actor = {}

    def dispatch(actor):
        gen = actor.process.options(num_returns="streaming").remote(call_duration)
        task_to_actor[gen] = actor

    def scheduling_loop(duration_s):
        completed = 0
        loop_durations = []
        t_start = time.monotonic()
        next_report = t_start + 5.0

        while True:
            t_loop = time.monotonic()
            if t_loop - t_start >= duration_s:
                break

            waitables = list(task_to_actor)
            if not waitables:
                time.sleep(0.01)
                continue

            ready, _ = ray.wait(
                waitables, num_returns=len(waitables), timeout=0.1, fetch_local=False
            )

            for gen in ready:
                while True:
                    try:
                        ref = gen._next_sync(timeout_s=0)
                    except StopIteration:
                        # Task complete. Refill this actor's pipeline.
                        actor = task_to_actor.pop(gen)
                        completed += 1
                        dispatch(actor)
                        break
                    if ref.is_nil():
                        # No more ready output right now.
                        break
                    # Block ref yielded; release to free Object Store memory.
                    del ref

            loop_durations.append(time.monotonic() - t_loop)

            now = time.monotonic()
            if now >= next_report:
                dt = now - t_start
                print(f"  [{dt:.0f}s] {completed} calls, {completed / dt:.1f}/s")
                next_report = now + 5.0

        elapsed = time.monotonic() - t_start
        return completed, elapsed, loop_durations

    for actor in actors:
        for _ in range(MAX_INFLIGHT_PER_ACTOR):
            dispatch(actor)

    print(f"Warmup ({warmup_s:.0f}s)...")
    scheduling_loop(warmup_s)

    print(f"Measuring ({measure_s:.0f}s)...")
    measure_completed, measure_duration, loop_durations = scheduling_loop(measure_s)
    throughput = measure_completed / measure_duration
    efficiency = throughput / theoretical_max * 100

    loop_durations.sort()
    n = len(loop_durations)
    loop_p50 = loop_durations[n // 2] * 1000 if n else 0
    loop_p99 = loop_durations[min(int(n * 0.99), n - 1)] * 1000 if n else 0

    result = {
        "num_actors": num_actors,
        "num_nodes": num_nodes,
        "call_duration_s": call_duration,
        "completed_calls": measure_completed,
        "measure_duration_s": round(measure_duration, 2),
        "throughput_calls_per_sec": round(throughput, 2),
        "theoretical_max_calls_per_sec": round(theoretical_max, 2),
        "efficiency_pct": round(efficiency, 2),  # This is bascially the overhead!
        "loop_time_p50_ms": round(loop_p50, 2),
        "loop_time_p99_ms": round(loop_p99, 2),
    }
    print(f"\n{json.dumps(result, indent=2)}")
    return result


def main():
    p = argparse.ArgumentParser(description="Benchmark 1A: Actor Call -- Streaming")
    p.add_argument("--num-actors", type=int, required=True)
    p.add_argument("--call-duration", type=float, required=True)
    p.add_argument("--measure-seconds", type=float, default=60)
    args = p.parse_args()

    ray.init(address="auto")
    run_benchmark(
        args.num_actors,
        args.call_duration,
        args.measure_seconds,
    )


if __name__ == "__main__":
    main()
