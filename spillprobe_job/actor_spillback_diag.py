"""Diagnostic — decompose the clean-floor completion (where do the ~29 s go?).

Runs the CLEAN config (both timers 600 s → 0 reject), then decomposes:
  - per-actor worker startup cost: time from the worker PROCESS start (psutil
    create_time) to running the actor's __init__  ≈ fork + import ray + setup;
  - the completion ramp + milestones;
  - the HEAD raylet.out / gcs_server.out (driver runs on head → read directly):
    is the head the lease/spill funnel?
  - a fan-out SAMPLE of worker-node core-worker logs (num_cpus=0 node-affinity
    tasks, so they run even though all CPUs are held by actors).
"""
import collections
import glob
import os
import re
import subprocess
import time

import ray
from ray.util.scheduling_strategies import NodeAffinitySchedulingStrategy

ACTOR_CPU = float(os.environ.get("ACTOR_CPU", "1"))
EXPECT_NODES = int(os.environ.get("EXPECT_NODES", "0"))
WARM_TIMEOUT_S = float(os.environ.get("WARM_TIMEOUT_S", "1200"))
POLL_S = float(os.environ.get("POLL_S", "1.0"))
MAX_RUN_S = float(os.environ.get("MAX_RUN_S", "900"))
SAMPLE = int(os.environ.get("SAMPLE_ACTORS", "4000"))
LOG_NODES = int(os.environ.get("LOG_NODES", "12"))

ray.init(address="auto")
LOG = "/tmp/ray/session_latest/logs"


def n_alive():
    return sum(1 for n in ray.nodes() if n["Alive"])


def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True).stdout


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))]


t = time.time()
while time.time() - t < WARM_TIMEOUT_S:
    a = n_alive()
    print(f"[warm] alive_nodes={a} (want {EXPECT_NODES}+1)", flush=True)
    if EXPECT_NODES and a >= EXPECT_NODES + 1:
        break
    time.sleep(10)
time.sleep(20)

cap = ray.cluster_resources().get("CPU", 0.0)
n = int(cap / ACTOR_CPU)
print(f"=== DIAG: alive={n_alive()} cluster_CPU={cap} -> burst {n} actors @ {ACTOR_CPU}cpu | "
      f"report_ms={os.environ.get('RAY_raylet_report_resources_period_milliseconds','default(100)')} "
      f"refresh_ms={os.environ.get('RAY_ray_syncer_message_refresh_interval_ms','default(3000)')} ===",
      flush=True)


@ray.remote(num_cpus=ACTOR_CPU)
class A:
    def __init__(self):
        self.t_init = time.time()
        try:
            import psutil
            self.t_proc = psutil.Process().create_time()
        except Exception:
            self.t_proc = None

    def ping(self):
        return (self.t_init, self.t_proc)


t0 = time.time()
actors = [A.remote() for _ in range(n)]
refs = [a.ping.remote() for a in actors]
fire = time.time() - t0
print(f"[fired] {n} actors+pings in {fire:.1f}s", flush=True)

MS = [0.5, 0.9, 0.99, 0.999, 1.0]
hit = {}
pending = list(refs)
done = 0
end = t0 + MAX_RUN_S
while pending and time.time() < end:
    r, pending = ray.wait(pending, num_returns=len(pending), timeout=POLL_S, fetch_local=False)
    done += len(r)
    now = time.time() - t0
    for m in MS:
        if m not in hit and done >= m * n:
            hit[m] = now
            print(f"[milestone] {m*100:g}% at t+{now:.2f}s", flush=True)
final = time.time() - t0
print(f"[DONE] all {n} ready in {final:.2f}s (fire={fire:.1f}s)" if not pending
      else f"[DNF] {done}/{n} at {MAX_RUN_S}s", flush=True)
print("[MILESTONES] " + " | ".join(f"{m*100:g}%={hit[m]:.2f}s" for m in MS if m in hit), flush=True)

# --- per-actor decomposition ---
vals = ray.get(refs[:SAMPLE])
inits = [v[0] - t0 for v in vals if v[0]]
startup = [v[0] - v[1] for v in vals if v[0] and v[1]]
print(f"[DECOMP] sample={len(vals)} actors", flush=True)
if inits:
    print(f"  reached __init__ (t_init - t0): p50={pct(inits,.5):.1f} p90={pct(inits,.9):.1f} "
          f"p99={pct(inits,.99):.1f} max={max(inits):.1f}s", flush=True)
if startup:
    print(f"  WORKER STARTUP (proc-age at __init__ ≈ fork+import ray): p20={pct(startup,.2):.2f} "
          f"p50={pct(startup,.5):.2f} p90={pct(startup,.9):.2f} p99={pct(startup,.99):.2f} "
          f"max={max(startup):.2f}s  (caveat: includes any idle-in-pool time)", flush=True)
else:
    print("  WORKER STARTUP: psutil unavailable", flush=True)

# --- head logs (driver is on the head node) ---
print("[HEAD raylet.out] " + sh(f"wc -l {LOG}/raylet.out 2>/dev/null").strip(), flush=True)
print(sh(f"tail -10 {LOG}/raylet.out 2>/dev/null"), flush=True)
print("[HEAD gcs_server.out] " + sh(f"wc -l {LOG}/gcs_server.out 2>/dev/null").strip(), flush=True)
print(sh(f"tail -4 {LOG}/gcs_server.out 2>/dev/null"), flush=True)
# GCS-side actor-creation window (each 'Actor created successfully' line is timestamped)
acl = [l for l in sh(f"grep -a 'Actor created successfully' {LOG}/gcs_server.out 2>/dev/null").splitlines() if l.strip()]
print(f"[GCS Actor-created] count={len(acl)}", flush=True)
if acl:
    print("  first: " + acl[0][:115], flush=True)
    print("  last:  " + acl[-1][:115], flush=True)

# --- (NEW) GCS actor-created RATE over time (per 2s bucket) -> where it paces/saturates ---
secs = []
for l in acl:
    m = re.search(r'(\d\d):(\d\d):(\d\d),(\d+)', l)
    if m:
        h, mm, s, ms = (int(x) for x in m.groups())
        secs.append(h * 3600 + mm * 60 + s + ms / 1000.0)
if secs:
    secs.sort()
    rel = [x - secs[0] for x in secs]
    bk = collections.Counter(int(x // 2) * 2 for x in rel)
    print(f"[GCS created-rate] span={rel[-1]:.1f}s n={len(secs)} avg={len(secs)/max(rel[-1],0.1):.0f}/s | per-2s (created):", flush=True)
    print("  " + " ".join(f"{b}s:{bk[b]}" for b in sorted(bk)), flush=True)

# --- (NEW) GCS event-loop stats: is the GCS the saturated funnel? (compare to head's 14 s) ---
print("[GCS event-loop stats]", flush=True)
print(sh("grep -aE 'Global stats: |grpc_server.RegisterActor |grpc_server.CreateActor |"
         "grpc_client.RequestWorkerLease |PushTask |GcsSubscriberPoll ' "
         f"{LOG}/gcs_server.out 2>/dev/null | tail -22"), flush=True)

# --- (NEW) HEAD RequestWorkerLease across ALL state-dumps (cumulative count + queueing growth) ---
print("[HEAD RequestWorkerLease across state-dumps]", flush=True)
print(sh(f"grep -aoE 'RequestWorkerLease[A-Za-z.]* - [0-9]+ total[^Q]*Queueing time: mean = [0-9.]+ms, max = [0-9.-]+ms' {LOG}/raylet.out 2>/dev/null"), flush=True)

# --- worker-node log fan-out (sample) ---
head = ray.get_runtime_context().get_node_id()
nodes = [nn["NodeID"] for nn in ray.nodes() if nn["Alive"] and nn["NodeID"] != head][:LOG_NODES]


@ray.remote(num_cpus=0)
def read_logs():
    import glob as g
    L = "/tmp/ray/session_latest/logs"
    cw = sorted(g.glob(f"{L}/python-core-worker-*.log"))
    s = {"node": ray.get_runtime_context().get_node_id()[:12], "n_cw": len(cw)}
    if cw:
        ls = open(cw[0], errors="replace").read().splitlines()
        s["first"] = ls[:7]
        s["last"] = ls[-3:]
    return s


print(f"[WORKER FANOUT — {len(nodes)} sampled nodes]", flush=True)
try:
    res = ray.get([read_logs.options(
        scheduling_strategy=NodeAffinitySchedulingStrategy(nid, soft=False)).remote()
        for nid in nodes], timeout=120)
    for s in res[:6]:
        print(f"  node {s['node']} core_worker_logs={s['n_cw']}", flush=True)
        for l in s.get("first", []):
            print("    CW| " + l[:170], flush=True)
except Exception as e:
    print(f"  fanout error: {e!r}", flush=True)
print("=== DIAG DONE ===", flush=True)
ray.shutdown()
