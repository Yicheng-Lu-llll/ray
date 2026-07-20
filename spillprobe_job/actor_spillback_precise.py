"""Precise-completion variant — resolves B vs C (the 12 s dump cadence was too coarse).

Driver-side ONLY (no scheduler perturbation): a tight `ray.wait` poll (POLL_S=0.5 s)
gives the exact completion time + tail milestones (50/90/99/99.9/100%). reject is
grepped coarsely (~5 s, cumulative) for the mechanism curve, off the timing path.
"""
import collections
import glob
import os
import re
import subprocess
import time

import ray

ACTOR_CPU = float(os.environ.get("ACTOR_CPU", "1"))
EXPECT_NODES = int(os.environ.get("EXPECT_NODES", "0"))
WARM_TIMEOUT_S = float(os.environ.get("WARM_TIMEOUT_S", "1200"))
POLL_S = float(os.environ.get("POLL_S", "1.0"))
GREP_EVERY_S = float(os.environ.get("GREP_EVERY_S", "5"))
MAX_RUN_S = float(os.environ.get("MAX_RUN_S", "900"))

ray.init(address="auto")
LOG = "/tmp/ray/session_latest/logs"
actor_re = re.compile(r"for actor (\w+)")


def n_alive():
    return sum(1 for n in ray.nodes() if n["Alive"])


def grep(pat, gp):
    out = []
    for fp in glob.glob(os.path.join(LOG, gp)):
        r = subprocess.run(["grep", "-ah", pat, fp], capture_output=True, text=True)
        out += [l for l in r.stdout.splitlines() if l.strip()]
    return out


def reject_stats():
    rej = grep("as the resources are not enough", "gcs_server.out*")
    resched = collections.Counter(m.group(1) for l in rej for m in [actor_re.search(l)] if m)
    mx = resched.most_common(1)[0][1] if resched else 0
    return len(rej), len(resched), mx


# --- warm (skip autoscaler ramp) ---
t = time.time()
while time.time() - t < WARM_TIMEOUT_S:
    a = n_alive()
    print(f"[warm] alive_nodes={a} (want {EXPECT_NODES}+1 head)", flush=True)
    if EXPECT_NODES and a >= EXPECT_NODES + 1:
        break
    time.sleep(10)
time.sleep(20)

cap = ray.cluster_resources().get("CPU", 0.0)
n = int(cap / ACTOR_CPU)
print(f"=== PRECISE: alive={n_alive()} cluster_CPU={cap} -> burst {n} actors @ {ACTOR_CPU}cpu "
      f"(default hybrid, no affinity/PG) ===", flush=True)
print(f"=== syncer report_ms="
      f"{os.environ.get('RAY_raylet_report_resources_period_milliseconds','default(100)')} "
      f"refresh_ms={os.environ.get('RAY_ray_syncer_message_refresh_interval_ms','default(3000)')} "
      f"| POLL_S={POLL_S} ===", flush=True)


@ray.remote(num_cpus=ACTOR_CPU)
class A:
    def ping(self):
        return 1


t0 = time.time()
actors = [A.remote() for _ in range(n)]
refs = [a.ping.remote() for a in actors]
fire = time.time() - t0
print(f"[fired] {n} actors + pings in {fire:.1f}s; polling every {POLL_S}s ===", flush=True)

MS = [0.5, 0.9, 0.99, 0.999, 1.0]
hit = {}
pending = list(refs)
done = 0
last_grep = -GREP_EVERY_S
end = t0 + MAX_RUN_S
while pending and time.time() < end:
    r, pending = ray.wait(pending, num_returns=len(pending), timeout=POLL_S, fetch_local=False)
    done += len(r)
    now = time.time() - t0
    for m in MS:
        if m not in hit and done >= m * n:
            hit[m] = now
            print(f"[milestone] {m*100:g}% ({done}/{n}) at t+{now:.2f}s", flush=True)
    if now - last_grep >= GREP_EVERY_S:
        rj, dist, mx = reject_stats()
        print(f"[t+{now:.1f}s] ready={done}/{n} reject={rj} distinct={dist} max_resched={mx}", flush=True)
        last_grep = now

final = time.time() - t0
rj, dist, mx = reject_stats()
if not pending:
    print(f"[DONE] all {n} ready in {final:.2f}s (fire={fire:.1f}s) | "
          f"reject={rj} distinct={dist} max_resched={mx}", flush=True)
else:
    print(f"[DNF] {done}/{n} ({100*done/max(n,1):.1f}%) at MAX_RUN_S={MAX_RUN_S}s | "
          f"reject={rj} distinct={dist} max_resched={mx}", flush=True)
print("[MILESTONES] " + " | ".join(f"{m*100:g}%={hit[m]:.2f}s" for m in MS if m in hit), flush=True)
ray.shutdown()
