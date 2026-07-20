"""Local repro of the smoke hang: PG-per-rack + wildcard actors exactly filling bundles.
Toggle the three fix flags via _system_config; report how many actors become ready."""
import os
import sys
import time

import ray
from ray.cluster_utils import Cluster
from ray.util.placement_group import placement_group
from ray.util.scheduling_strategies import PlacementGroupSchedulingStrategy

RACK = "ray.io/gpu-domain"
NODES_PER_RACK = 4
NUM_RACKS = 2
CPU_PER_NODE = 2
ACTORS_PER_NODE = 4  # 4 x 0.5 CPU = 2 CPU, exact fit

cfg = {}
for flag in sys.argv[1:]:
    cfg[flag] = True
print(f"flags on: {list(cfg) or 'NONE (baseline)'}", flush=True)

cluster = Cluster()
head = cluster.add_node(num_cpus=0, _system_config=cfg)
ray.init(address=cluster.address)
for r in range(NUM_RACKS):
    for _ in range(NODES_PER_RACK):
        cluster.add_node(num_cpus=CPU_PER_NODE, resources={}, labels={RACK: f"rack-{r}"})
cluster.wait_for_nodes()
time.sleep(2)

pgs = []
for r in range(NUM_RACKS):
    pg = placement_group(bundles=[{"CPU": CPU_PER_NODE}] * NODES_PER_RACK,
                         strategy="STRICT_SPREAD")
    pgs.append(pg)
ray.get([pg.ready() for pg in pgs])
print("PGs ready", flush=True)


@ray.remote(num_cpus=CPU_PER_NODE / ACTORS_PER_NODE)
class A:
    def ping(self):
        return "pong"


actors = []
for pg in pgs:
    for _ in range(NODES_PER_RACK * ACTORS_PER_NODE):
        actors.append(A.options(
            scheduling_strategy=PlacementGroupSchedulingStrategy(placement_group=pg)).remote())
n = len(actors)
print(f"submitted {n} wildcard actors", flush=True)

pings = [a.ping.remote() for a in actors]
ready = 0
deadline = time.time() + 60
pending = list(pings)
while pending and time.time() < deadline:
    done, pending = ray.wait(pending, num_returns=len(pending), timeout=3)
    ready += len(done)
print(f"RESULT ready={ready}/{n} stuck={len(pending)} elapsed_ok={not pending}", flush=True)
ray.shutdown()
cluster.shutdown()
sys.exit(0 if ready == n else 1)
