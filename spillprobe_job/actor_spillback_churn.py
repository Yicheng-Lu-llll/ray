"""CHURN test for the L1.5 spillback fix (RAY_SPILLBACK_FIX2=1).

Why: L1.5's release is "absorption" — it releases a saved optimistic decrement only
for the resources a node REPORTS as consumed (diff of consecutive snapshots). Under
CHURN (actors dying/finishing → availability RISES, new actors placed → availability
DROPS) that attribution could mis-release (over-release → over-pick storm) or over-hold
(nodes look full forever → can't refill → leak). This test stresses exactly that.

Design: fill the cluster (16000 actors), then for CHURN_S keep killing+replacing a
fraction each period (frees + re-placements on a near-full cluster). The signal to watch:
- cumulative `reject` should stay ~flat / bounded (re-placements succeed first-try) — a
  climbing reject = absorption mis-releasing under churn.
- `alive` should stay ~= n_actors (cluster refills) — a falling alive = decrements leaking
  (nodes wrongly look full, replacements can't place).

Env: EXPECT_NODES ACTOR_CPU=1 WARM_TIMEOUT_S=1200 FILL_TIMEOUT_S=180 DUMP_S=10
     CHURN_FRAC=0.05 CHURN_PERIOD_S=10 CHURN_S=300
Run WITH RAY_SPILLBACK_FIX2=1 (fix on) and also WITHOUT (control) to compare.
"""
import collections
import glob
import os
import random
import re
import subprocess
import time

import ray

ACTOR_CPU = float(os.environ.get("ACTOR_CPU", "1"))
EXPECT_NODES = int(os.environ.get("EXPECT_NODES", "0"))
WARM_TIMEOUT_S = float(os.environ.get("WARM_TIMEOUT_S", "1200"))
FILL_TIMEOUT_S = float(os.environ.get("FILL_TIMEOUT_S", "180"))
DUMP_S = float(os.environ.get("DUMP_S", "10"))
CHURN_FRAC = float(os.environ.get("CHURN_FRAC", "0.05"))
CHURN_PERIOD_S = float(os.environ.get("CHURN_PERIOD_S", "10"))
CHURN_S = float(os.environ.get("CHURN_S", "300"))

ray.init(address="auto")
LOGDIR = "/tmp/ray/session_latest/logs"


def n_alive():
    return sum(1 for n in ray.nodes() if n["Alive"])


def grep(pat, glob_pat):
    out = []
    for fp in glob.glob(os.path.join(LOGDIR, glob_pat)):
        r = subprocess.run(["grep", "-ah", pat, fp], capture_output=True, text=True)
        out += [l for l in r.stdout.splitlines() if l.strip()]
    return out


actor_re = re.compile(r"for actor (\w+)")


def reject_stats():
    rej = grep("as the resources are not enough", "gcs_server.out*")
    resched = collections.Counter(m.group(1) for l in rej for m in [actor_re.search(l)] if m)
    mx = resched.most_common(1)[0][1] if resched else 0
    return len(rej), len(resched), mx


# --- warm ---
t = time.time()
while time.time() - t < WARM_TIMEOUT_S:
    a = n_alive()
    print(f"[warm] alive_nodes={a} (want {EXPECT_NODES}+1 head)", flush=True)
    if EXPECT_NODES and a >= EXPECT_NODES + 1:
        break
    time.sleep(10)
time.sleep(20)

cap = ray.cluster_resources().get("CPU", 0.0)
n_actors = int(cap / ACTOR_CPU)
fix = os.environ.get("RAY_SPILLBACK_FIX2", "") or os.environ.get("RAY_SPILLBACK_FIX", "")
print(f"=== CHURN test: cluster_CPU={cap} n_actors={n_actors} FIX={'on' if fix else 'OFF'} "
      f"churn={CHURN_FRAC} every {CHURN_PERIOD_S}s for {CHURN_S}s ===", flush=True)


@ray.remote(num_cpus=ACTOR_CPU)
class A:
    def ping(self):
        return 1


# --- phase 1: fill ---
t0 = time.time()
actors = [A.remote() for _ in range(n_actors)]
refs = [a.ping.remote() for a in actors]
print(f"[fired] {n_actors} actors in {time.time()-t0:.1f}s; waiting to fill", flush=True)
fill_end = time.time() + FILL_TIMEOUT_S
while time.time() < fill_end:
    ready = len(ray.wait(refs, num_returns=len(refs), timeout=0)[0])
    rj, dist, mx = reject_stats()
    print(f"[fill t+{time.time()-t0:.0f}s] ready={ready}/{n_actors} reject={rj} "
          f"distinct={dist} max_resched={mx}", flush=True)
    if ready >= n_actors:
        print(f"[filled] all {n_actors} in {time.time()-t0:.1f}s", flush=True)
        break
    time.sleep(DUMP_S)

# --- phase 2: churn ---
print(f"=== CHURN PHASE begins (watch reject stays flat + alive stays ~{n_actors}) ===", flush=True)
rej0, _, _ = reject_stats()
tc = time.time()
ci = 0
K = max(1, int(CHURN_FRAC * n_actors))
while time.time() - tc < CHURN_S:
    ci += 1
    victims = random.sample(range(len(actors)), K)
    for idx in victims:
        ray.kill(actors[idx])
    newa = [A.remote() for _ in range(K)]
    newr = [a.ping.remote() for a in newa]
    for j, idx in enumerate(victims):
        actors[idx] = newa[j]
        refs[idx] = newr[j]
    time.sleep(CHURN_PERIOD_S)
    ready = len(ray.wait(refs, num_returns=len(refs), timeout=0)[0])
    rj, dist, mx = reject_stats()
    print(f"[churn {ci} t+{time.time()-tc:.0f}s] killed+made {K} | ready={ready}/{n_actors} "
          f"reject={rj} (+{rj-rej0} since churn start) distinct={dist} max_resched={mx} "
          f"alive_nodes={n_alive()}", flush=True)
print(f"[done] churn ended. reject during churn = {rj-rej0} over {CHURN_S}s "
      f"({K} kills/replaces every {CHURN_PERIOD_S}s).", flush=True)
ray.shutdown()
