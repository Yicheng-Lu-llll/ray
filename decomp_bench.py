"""Local GCS register/create decomposition benchmark.

Creates N trivial PG actors (matching the release benchmark's actor shape). Only a few
fit in the bundle and actually start workers; the rest stay PENDING but STILL go through
RegisterActor + CreateActor on the GCS main thread -- which is exactly the hot path we
are decomposing. The gcs_server DECOMPDBG log lines carry the per-sub-step averages.
"""
import os
import time

import ray
from ray.util.placement_group import placement_group
from ray.util.scheduling_strategies import PlacementGroupSchedulingStrategy

N = int(os.environ.get("N", "4000"))
WAIT = int(os.environ.get("WAIT", "60"))

ray.init(address="auto")

# Small bundle: only a handful of num_cpus=1 actors get a worker; the remaining
# N-few stay PENDING (register+create already processed by GCS). Keeps worker count
# and memory tiny while still driving N register/create through the GCS main thread.
pg = placement_group([{"CPU": 4}])
ray.get(pg.ready())


@ray.remote(num_cpus=1)
class A:
    def ping(self):
        return "pong"


t0 = time.time()
actors = [
    A.options(
        scheduling_strategy=PlacementGroupSchedulingStrategy(placement_group=pg)
    ).remote()
    for _ in range(N)
]
print(f"submitted {N} actor handles in {time.time()-t0:.2f}s; draining for {WAIT}s")
time.sleep(WAIT)
print("done waiting; check gcs_server.out for DECOMPDBG lines")
