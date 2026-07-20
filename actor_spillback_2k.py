"""2K SPILLPROBE catch — stream the chain MID-RUN, kill once nailed (no completion needed).

Heavy probe logging is fine. Fire the burst NON-BLOCKING so the scheduling storm
starts, then loop-dump the head raylet.out SPILLPROBE chain + gcs_server.out rejects
to stdout every DUMP_S, so `anyscale job logs` shows the DEC->UPDATE-WIPE->re-PICK
->REJECT chain WHILE it runs. Grab it, then terminate the job (on-demand = costly).

Env: EXPECT_NODES  ACTOR_CPU=1  WARM_TIMEOUT_S=1200  DUMP_S=12  MAX_RUN_S=420
Syncer A/B via job RAY_* env (baseline=defaults; treatment=report/refresh 60000).
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
MAX_RUN_S = float(os.environ.get("MAX_RUN_S", "420"))  # cost backstop (on-demand ~$13/min)

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


# --- warm (don't read autoscaler ramp; wait for nodes to register) ---
t = time.time()
while time.time() - t < WARM_TIMEOUT_S:
    a = n_alive()
    print(f"[warm] alive_nodes={a} (want {EXPECT_NODES}+1 head)", flush=True)
    if EXPECT_NODES and a >= EXPECT_NODES + 1:
        break
    time.sleep(10)
time.sleep(20)  # let last joiners report once

cap = ray.cluster_resources().get("CPU", 0.0)
n_actors = int(cap / ACTOR_CPU)  # demand == capacity
print(f"=== 2K CATCH: alive={n_alive()} cluster_CPU={cap} -> burst {n_actors} actors "
      f"@ {ACTOR_CPU}cpu (default hybrid, no affinity/PG) ===", flush=True)
print(f"=== syncer report_ms="
      f"{os.environ.get('RAY_raylet_report_resources_period_milliseconds','default(100)')} "
      f"refresh_ms={os.environ.get('RAY_ray_syncer_message_refresh_interval_ms','default(3000)')} ===",
      flush=True)


@ray.remote(num_cpus=ACTOR_CPU)
class A:
    def ping(self):
        return 1


# --- fire the burst NON-BLOCKING (starts the storm; driver does not wait) ---
t0 = time.time()
actors = [A.remote() for _ in range(n_actors)]
refs = [a.ping.remote() for a in actors]
print(f"[fired] {n_actors} actors + pings in {time.time()-t0:.1f}s; streaming chain every "
      f"{DUMP_S}s (kill me once the chain is nailed) ===", flush=True)

actor_re = re.compile(r"for actor (\w+)")
end = time.time() + MAX_RUN_S
loop = 0
while time.time() < end:
    loop += 1
    ready = len(ray.wait(refs, num_returns=len(refs), timeout=0)[0])
    pick = grep("SPILLPROBE PICK", "raylet.out*")
    dec = grep("SPILLPROBE DEC", "raylet.out*")
    wa = grep("SPILLPROBE UPDATE-WIPE", "raylet.out*")
    wb = grep("SPILLPROBE RESET-WIPE", "raylet.out*")
    rej = grep("as the resources are not enough", "gcs_server.out*")
    resched = collections.Counter(
        m.group(1) for l in rej for m in [actor_re.search(l)] if m)
    mx = resched.most_common(1)[0][1] if resched else 0
    print(f"\n===== [dump {loop} t+{time.time()-t0:.0f}s] ready={ready}/{n_actors} | "
          f"PICK={len(pick)} DEC={len(dec)} UPDATE-WIPE={len(wa)} RESET-WIPE={len(wb)} "
          f"reject={len(rej)} distinct_rejected={len(resched)} max_reschedule_1actor={mx} =====",
          flush=True)
    for l in wa[-6:]:
        print("  WIPE " + l.split("SPILLPROBE", 1)[-1].strip()[:200], flush=True)
    for l in rej[-4:]:
        print("  REJ  ..." + l.strip()[-150:], flush=True)
    # PER-NODE smoking-gun chain at scale: the hottest-rejected node's full raylet
    # timeline (PICK/DEC/UPDATE-WIPE/RESET-WIPE) in time order -> see DEC -> WIPE(raised)
    # -> re-PICK on ONE real node. Streamed to stdout (no need to keep the cluster).
    node_re = re.compile(r"from node (\w+)")
    nc = collections.Counter(m.group(1) for l in rej for m in [node_re.search(l)] if m)
    if nc:
        hot, hotn = nc.most_common(1)[0]
        tl = grep("node=" + hot, "raylet.out*")  # file order == time order
        print(f"  >>> CHAIN @ hot node {hot[:14]}.. ({hotn} rejects here) — last "
              f"{min(22, len(tl))} of {len(tl)} scheduling events on it:", flush=True)
        for l in tl[-22:]:
            s = l.split("SPILLPROBE", 1)[-1].strip() if "SPILLPROBE" in l else l.strip()
            tag = ("UPD-WIPE" if "UPDATE-WIPE" in s else "RST-WIPE" if "RESET-WIPE" in s
                   else "DEC" if s.startswith("DEC") else "PICK" if "PICK" in s else "?")
            print(f"      [{tag}] {s[:150]}", flush=True)
    if ready >= n_actors and n_actors > 0:
        print(f"[done] all {n_actors} ready in {time.time()-t0:.1f}s", flush=True)
        break
    time.sleep(DUMP_S)
print("[exit] streaming window ended", flush=True)
ray.shutdown()
