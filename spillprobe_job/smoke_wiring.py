"""Smoke wiring-check on the release image, BEFORE the 2000-node A/B.

Verifies (experiment-discipline §4 — wiring on a small cluster):
  (1) cluster comes up on the release image + version/optimized.
  (2) the RAY_ timer env actually reaches the raylet process (/proc/PID/environ)
      — burned before: RAY_<config> may not reach a raylet.
  (3) the reject log string is compiled into this release's GCS binary
      — the metric grep ("as the resources are not enough") may differ across versions.
  (4) a small burst runs + the driver can read gcs_server.out.
"""
import glob
import os
import subprocess
import time

import ray


def sh(c):
    return subprocess.run(c, shell=True, capture_output=True, text=True).stdout.strip()


ray.init(address="auto")
LOG = "/tmp/ray/session_latest/logs"
EXPECT = int(os.environ.get("EXPECT_NODES", "3"))

print("=== (1) CLUSTER ===", flush=True)
for _ in range(60):
    if sum(1 for n in ray.nodes() if n["Alive"]) >= EXPECT + 1:
        break
    time.sleep(5)
print("  ray.__version__ =", ray.__version__, "| commit =", getattr(ray, "__commit__", "?"))
print("  ALIVE =", sum(1 for n in ray.nodes() if n["Alive"]), "(want", EXPECT + 1, ")",
      "| cluster CPU =", ray.cluster_resources().get("CPU"))
print("  ANYSCALE_DISABLE_OPTIMIZED_RAY =",
      os.environ.get("ANYSCALE_DISABLE_OPTIMIZED_RAY", "(unset -> optimized Ray)"))

print("=== (2) TIMER RAY_ ENV REACHED RAYLET PROCESS ===", flush=True)
found = sh("for p in $(pgrep -f raylet 2>/dev/null); do "
           "tr '\\0' '\\n' < /proc/$p/environ 2>/dev/null "
           "| grep -iE 'syncer_message_refresh|report_resources_period'; done | sort -u")
print("  timer env in raylet process(es):", found or "(NOT FOUND - treatment would be a no-op!)")

print("=== (3) REJECT LOG STRING IN GCS BINARY ===", flush=True)
core = sh("python -c 'import ray,os;print(os.path.dirname(ray.__file__))'")
hits = sh(f"grep -arl 'as the resources are not enough' {core}/ 2>/dev/null | head -5")
print("  binaries containing the reject string:")
print("   ", (hits.replace(chr(10), chr(10) + "    ") if hits else
              "(NOT FOUND - reject metric string may differ in this release!)"))

print("=== (4) WORKLOAD + LOG READ ===", flush=True)
cap = int(ray.cluster_resources().get("CPU", 0))


@ray.remote(num_cpus=1)
class A:
    def p(self):
        return 1


acts = [A.remote() for _ in range(cap)]
refs = [a.p.remote() for a in acts]
time.sleep(40)
ready = len(ray.wait(refs, num_returns=len(refs), timeout=0)[0])
files = glob.glob(f"{LOG}/gcs_server.out*")
rej = sh(f"grep -ah 'as the resources are not enough' {LOG}/gcs_server.out* 2>/dev/null | wc -l")
print(f"  bursted {cap} actors -> ready={ready}/{cap}")
print(f"  gcs_server.out present: {bool(files)} {files}")
print(f"  reject lines in gcs_server.out so far: {rej}")
print("=== SMOKE DONE ===", flush=True)
