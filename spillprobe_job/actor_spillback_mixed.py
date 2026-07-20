"""MIXED task+actor SPILLPROBE — the FIX3-vs-L1.5 differentiator.

L1.5 (RAY_SPILLBACK_FIX2) releases the optimistic spill decrement by diffing the
spill target's AGGREGATE resource report (release == "available dropped by the lease's
size"). That diff is only clean for a pure-actor burst. Mix in normal tasks that
acquire+release CPU on the same nodes and the aggregate availability oscillates for
reasons unrelated to the spilled lease -> L1.5 mis-attributes the wiggle -> releases the
decrement early -> the stale-syncer clobber re-exposes the node -> spillback storm
returns. FIX3 releases ONLY on the real grant/reject signal, so it is immune.

So: this entrypoint runs the SAME burst as actor_spillback_2k.py, but with a continuous
background of short CPU tasks churning on the cluster during placement. Compare
SPILL_FIX2=1 (storm should return) vs SPILL_FIX3=1 (storm stays dead).

Env (adds to the 2k set): CHURN_CPU_FRAC=0.30 (fraction of cluster CPU kept busy with
churn tasks) CHURN_SLEEP_S=0.3 (per-task hold) ACTOR_CPU_FRAC=0.65 (actors target this
fraction of cap, leaving headroom for churn so reports actually oscillate).
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
DUMP_S = float(os.environ.get("DUMP_S", "12"))
MAX_RUN_S = float(os.environ.get("MAX_RUN_S", "420"))
CHURN_CPU_FRAC = float(os.environ.get("CHURN_CPU_FRAC", "0.30"))
CHURN_SLEEP_S = float(os.environ.get("CHURN_SLEEP_S", "0.3"))
ACTOR_CPU_FRAC = float(os.environ.get("ACTOR_CPU_FRAC", "0.65"))

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
n_actors = int(cap * ACTOR_CPU_FRAC / ACTOR_CPU)  # leave headroom for churn
n_churn = max(1, int(cap * CHURN_CPU_FRAC))       # tasks kept in-flight (1 CPU each)
print(f"=== MIXED: alive={n_alive()} cluster_CPU={cap} -> burst {n_actors} actors "
      f"@ {ACTOR_CPU}cpu ({ACTOR_CPU_FRAC:.0%} cap) + {n_churn} churn tasks "
      f"({CHURN_CPU_FRAC:.0%} cap, {CHURN_SLEEP_S}s hold) ===", flush=True)
print(f"=== FIX2(L1.5)={os.environ.get('RAY_SPILLBACK_FIX2','0')} "
      f"FIX3={os.environ.get('RAY_SPILLBACK_FIX3','0')} "
      f"report_ms={os.environ.get('RAY_raylet_report_resources_period_milliseconds','default(100)')} "
      f"refresh_ms={os.environ.get('RAY_ray_syncer_message_refresh_interval_ms','default(3000)')} ===",
      flush=True)


@ray.remote(num_cpus=ACTOR_CPU)
class A:
    def ping(self):
        return 1


@ray.remote(num_cpus=1)
def churn(hold):
    time.sleep(hold)
    return 1


# --- churn is managed from the MAIN thread (ray.wait is NOT thread-safe; a background
#     thread calling it segfaults the driver). Keep ~n_churn short tasks in-flight,
#     topped up every tick, so node availability oscillates during placement. ---
churn_inflight = [churn.remote(CHURN_SLEEP_S) for _ in range(n_churn)]
churn_done_total = [0]


def tick_churn():
    global churn_inflight
    done, churn_inflight = ray.wait(churn_inflight, num_returns=len(churn_inflight),
                                    timeout=0)
    churn_done_total[0] += len(done)
    churn_inflight += [churn.remote(CHURN_SLEEP_S) for _ in range(len(done))]


time.sleep(2)  # let churn ramp so reports are already oscillating when the burst lands

# --- fire the burst NON-BLOCKING ---
t0 = time.time()
actors = [A.remote() for _ in range(n_actors)]
refs = [a.ping.remote() for a in actors]
print(f"[fired] {n_actors} actors + pings in {time.time()-t0:.1f}s (churn running); "
      f"streaming every {DUMP_S}s ===", flush=True)

actor_re = re.compile(r"for actor (\w+)")
end = time.time() + MAX_RUN_S
loop = 0
while time.time() < end:
    loop += 1
    tick_churn()  # top up churn from the main thread
    ready = len(ray.wait(refs, num_returns=len(refs), timeout=0)[0])
    pick = grep("SPILLPROBE PICK", "raylet.out*")
    dec = grep("SPILLPROBE DEC", "raylet.out*")
    wa = grep("SPILLPROBE UPDATE-WIPE", "raylet.out*")
    wb = grep("SPILLPROBE RESET-WIPE", "raylet.out*")
    rej = grep("as the resources are not enough", "gcs_server.out*")
    resched = collections.Counter(
        m.group(1) for l in rej for m in [actor_re.search(l)] if m)
    mx = resched.most_common(1)[0][1] if resched else 0
    f3rec = grep("SPILLFIX3 recorded=", "raylet.out*")
    f3rel = grep("SPILLFIX3 release recv=", "raylet.out*")
    f3gcs = grep("SPILLFIX3 gcs ", "gcs_server.out*")
    print(f"\n===== [dump {loop} t+{time.time()-t0:.0f}s] ready={ready}/{n_actors} | "
          f"churn_done={churn_done_total[0]} PICK={len(pick)} DEC={len(dec)} "
          f"UPDATE-WIPE={len(wa)} RESET-WIPE={len(wb)} reject={len(rej)} "
          f"distinct_rejected={len(resched)} max_reschedule_1actor={mx} =====", flush=True)
    for l in (f3rec[-1:] + f3rel[-1:] + f3gcs[-2:]):
        print("  FIX3 " + l.split("SPILLFIX3", 1)[-1].strip()[:160], flush=True)
    for l in rej[-3:]:
        print("  REJ  ..." + l.strip()[-150:], flush=True)
    if ready >= n_actors and n_actors > 0:
        print(f"[done] all {n_actors} ready in {time.time()-t0:.1f}s "
              f"(churn_done={churn_done_total[0]})", flush=True)
        break
    # sleep DUMP_S but keep churning every ~0.5s so node availability oscillates
    t_dump = time.time()
    while time.time() - t_dump < DUMP_S:
        tick_churn()
        time.sleep(0.5)

print("[exit] mixed window ended", flush=True)
ray.shutdown()
