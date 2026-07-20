"""Diag v2 — CLOSE the CPU budget: is the ~28 s clean floor GCS-thread-bound or
downstream(worker-provisioning)-bound?

Method: snapshot the GCS "Main service" event-loop stats AND the head raylet's
NodeManager event-loop stats at t0 (just before firing) and t1 (at completion),
then DELTA them. The delta is the work done DURING the burst only (lifetime
cumulative dumps are diluted by the minutes-long scale-up and cannot answer this).

From the delta we compute, for the burst window:
  - thread-blocking CPU on each loop (sum of *.HandleRequestImpl + *.OnReplyReceived
    + named callbacks; EXCLUDES bare grpc_client/grpc_server entries which are
    async RPC-latency spans, not thread time)  -> utilization % = blockingCPU / wall
  - event_loop_lag_probe (direct saturation signal)
  - RequestWorkerLease.OnReplyReceived count -> lease-GRANT rate (downstream throughput)
  - PushTask presence/cost (the per-actor task-spec copy + push)
If GCS blocking-CPU ~= wall -> GCS-bound. If ~46% and lag low -> downstream-bound.
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

ray.init(address="auto")
LOG = "/tmp/ray/session_latest/logs"
GCS = f"{LOG}/gcs_server.out"
RAYLET = f"{LOG}/raylet.out"

# matches: "<name> - <count> total (...), Execution time: mean = X ms, total = Y ms,
#           Queueing time: mean = Q ms, max = M ms, ..."
STAT = re.compile(
    r'([\w:.]+) - (\d+) total[^,]*, Execution time: mean = ([\d.-]+)ms, '
    r'total = ([\d.-]+)ms, Queueing time: mean = ([\d.-]+)ms, max = ([\d.-]+)ms')


def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True).stdout


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))]


def n_alive():
    return sum(1 for n in ray.nodes() if n["Alive"])


def is_blocking(name):
    """Thread-blocking on the loop? handlers + reply callbacks + named callbacks.
    Exclude bare grpc_client.X / grpc_server.X (those are async RPC-latency spans)."""
    if name.endswith(".HandleRequestImpl") or name.endswith(".OnReplyReceived"):
        return True
    if ".grpc_client." in name or ".grpc_server." in name:
        return False  # bare RPC span
    return True  # named callback (RayletLoadPulled, GcsResourceManager::Update, Put, ...)


def snapshot(logfile, marker):
    """Last log message containing `marker`; parse its stat lines -> {name: (cnt, exec_total, q_mean, q_max)}."""
    # pull the whole (json) line containing the marker, last occurrence
    line = sh(f"grep -a '{marker}' {logfile} 2>/dev/null | tail -1")
    if not line.strip():
        return {}
    body = line.replace('\\n', '\n').replace('\\t', '\t')
    # restrict to the section starting at the marker (so GCS 'Main service' block only)
    idx = body.find(marker)
    if idx >= 0:
        body = body[idx:]
    out = {}
    for m in STAT.finditer(body):
        name, cnt, _emean, etot, qmean, qmax = m.groups()
        out[name] = (int(cnt), float(etot), float(qmean), float(qmax))
    return out


def delta_report(tag, s0, s1, wall):
    print(f"\n=== {tag}: burst-window DELTA over {wall:.1f}s wall ===", flush=True)
    if not s0 or not s1:
        print(f"  MISSING snapshot (s0={len(s0)} s1={len(s1)})", flush=True)
        return
    rows = []
    block_cpu = 0.0
    for name, (c1, e1, qm1, qx1) in s1.items():
        c0, e0, _q0, _x0 = s0.get(name, (0, 0.0, 0.0, 0.0))
        dc, de = c1 - c0, e1 - e0
        if dc <= 0 and de <= 0.5:
            continue
        blk = is_blocking(name)
        if blk:
            block_cpu += de
        rows.append((de, dc, name, blk, qm1, qx1))
    rows.sort(reverse=True)
    print(f"  >>> thread-BLOCKING CPU during burst = {block_cpu/1000:.2f}s  "
          f"=> utilization = {100*block_cpu/1000/wall:.0f}% of {wall:.1f}s wall", flush=True)
    print(f"  {'dExec(s)':>9} {'dCount':>8} {'qMean(ms)':>9} {'qMax(ms)':>9}  blk name", flush=True)
    for de, dc, name, blk, qm, qx in rows[:22]:
        print(f"  {de/1000:9.2f} {dc:8d} {qm:9.1f} {qx:9.1f}  {'B' if blk else '.'} {name}", flush=True)


# ---------- warm up ----------
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
print(f"=== DIAG2: alive={n_alive()} cluster_CPU={cap} -> burst {n} actors @ {ACTOR_CPU}cpu | "
      f"report_ms={os.environ.get('RAY_raylet_report_resources_period_milliseconds','default')} "
      f"refresh_ms={os.environ.get('RAY_ray_syncer_message_refresh_interval_ms','default')} "
      f"evt_print_ms={os.environ.get('RAY_event_stats_print_interval_ms','default')} ===", flush=True)


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


# ---------- t0 snapshot (pre-fire) ----------
gcs0 = snapshot(GCS, "Main service Event stats:")
ray0 = snapshot(RAYLET, "NodeManagerService.grpc_server.RequestWorkerLease")
w0 = time.time()
print(f"[t0 snapshot] gcs_handlers={len(gcs0)} raylet_handlers={len(ray0)}", flush=True)

t0 = time.time()
actors = [A.remote() for _ in range(n)]
refs = [a.ping.remote() for a in actors]
fire = time.time() - t0
print(f"[fired] {n} actors+pings in {fire:.1f}s", flush=True)

MS = [0.5, 0.9, 0.99, 1.0]
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

# ---------- t1 snapshot (completion) ----------
w1 = time.time()
gcs1 = snapshot(GCS, "Main service Event stats:")
ray1 = snapshot(RAYLET, "NodeManagerService.grpc_server.RequestWorkerLease")
wall = w1 - w0
print(f"[DONE] all {n} ready in {final:.2f}s (fire={fire:.1f}s)" if not pending
      else f"[DNF] {done}/{n} at {MAX_RUN_S}s", flush=True)
print("[MILESTONES] " + " | ".join(f"{m*100:g}%={hit[m]:.2f}s" for m in MS if m in hit), flush=True)

# ---------- per-actor worker startup ----------
vals = ray.get(refs[:SAMPLE])
startup = [v[0] - v[1] for v in vals if v[0] and v[1]]
if startup:
    print(f"[WORKER STARTUP] proc-age@__init__ p20={pct(startup,.2):.2f} p50={pct(startup,.5):.2f} "
          f"p90={pct(startup,.9):.2f} p99={pct(startup,.99):.2f} max={max(startup):.2f}s", flush=True)

# ---------- GCS created-rate ----------
acl = sh(f"grep -a 'Actor created successfully' {GCS} 2>/dev/null").splitlines()
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
    print(f"[GCS created-rate] n={len(secs)} span={rel[-1]:.1f}s avg={len(secs)/max(rel[-1],0.1):.0f}/s | per-2s:", flush=True)
    print("  " + " ".join(f"{b}s:{bk[b]}" for b in sorted(bk)), flush=True)

# ---------- THE DELTAS (the answer) ----------
delta_report("GCS Main service", gcs0, gcs1, wall)
delta_report("HEAD raylet NodeManager", ray0, ray1, wall)

print("=== DIAG2 DONE ===", flush=True)
ray.shutdown()
