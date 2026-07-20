"""Submit the SPILLPROBE actor-scheduling Job (smoke | baseline | treatment).

Usage (ray_dev python):
  python submit_spillprobe.py smoke       # 3 workers on-demand, wiring check (~$0.5)
  python submit_spillprobe.py baseline    # 2000 workers spot, default syncer (catch the chain)
  python submit_spillprobe.py treatment   # 2000 workers spot, syncer->60s (does spillback ->0)

Single variable: baseline vs treatment differ ONLY by the two RAY_ syncer env vars.
"""
import os
import sys
import time

import anyscale.job
from anyscale.job.models import JobConfig

IMAGE = os.environ.get(
    "IMG",
    "029272617770.dkr.ecr.us-west-2.amazonaws.com/anyscale/ray:"
    "yicheng_spillfix_9c0c7809a8")  # has RAY_SPILLBACK_FIX (off unless env set)

mode = sys.argv[1] if len(sys.argv) > 1 else "smoke"
probe = sys.argv[2] if len(sys.argv) > 2 else "probe"  # probe | noprobe (noprobe = faithful timing)
if mode == "smoke":
    workers, market, head, warm = 3, "ON_DEMAND", "m5.2xlarge", "300"
elif mode == "mid":
    # mid-scale A/B (cheap): enough nodes for the syncer-clobber storm to manifest,
    # small enough to run FIX2-vs-FIX3 cheaply. ON_DEMAND (§1).
    workers, market, head, warm = (
        int(os.environ.get("MID_NODES", "128")), "ON_DEMAND", "m5.2xlarge", "600")
elif mode in ("baseline", "treatment", "bonly", "aonly"):
    # ON_DEMAND, NOT spot (§1): spot reclaims inject spillback/reschedule noise.
    workers, market, head, warm = 2000, "ON_DEMAND", "m5.8xlarge", "1200"
else:
    raise SystemExit(f"unknown mode {mode!r}")

env = {
    "EXPECT_NODES": str(workers),
    "ACTOR_CPU": "1",
    "WARM_TIMEOUT_S": warm,
    # Run-to-completion: entrypoint stops when all actors are ready (ready>=N);
    # MAX_RUN_S is only a backstop for storming arms that never converge.
    "MAX_RUN_S": os.environ.get("MAX_RUN_S", "420"),
    # Silence GCS ray_event_recorder "Dropping events" spam: at 2000 nodes + fast
    # report it floods gcs_server.out + crowds driver dumps out of stored logs.
    # Pure observability -> does NOT affect scheduling behavior being measured.
    "RAY_enable_ray_event": "0",
}
if probe == "probe":  # heavy SPILLPROBE (mechanism); OFF = faithful scheduling time
    env["RAY_SPILLBACK_PROBE"] = "1"
if os.environ.get("SPILL_FIX"):  # L1: save decrement + reapply after reset; clear on report
    env["RAY_SPILLBACK_FIX"] = "1"
if os.environ.get("SPILL_FIX2"):  # L1.5: absorption release (release only as report shows consumed)
    env["RAY_SPILLBACK_FIX2"] = "1"
if os.environ.get("SPILL_FIX3"):  # Fix-1: per-lease decrement, release on real grant/reject (task-proof)
    env["RAY_SPILLBACK_FIX3"] = "1"
# forward churn-test knobs (actor_spillback_churn.py reads these from the job env)
for _k in ("CHURN_FRAC", "CHURN_PERIOD_S", "CHURN_S", "FILL_TIMEOUT_S", "DUMP_S",
           "ACTOR_CPU_FRAC", "CHURN_CPU_FRAC", "CHURN_SLEEP_S"):
    if os.environ.get(_k):
        env[_k] = os.environ[_k]
# syncer config is startup-read -> separate cluster per arm (§1):
syncer_ms = os.environ.get("SYNCER_MS", "60000")  # both-knob value: 60000=60s (default), 600000=600s
if mode == "treatment":  # both knobs -> SYNCER_MS
    env["RAY_raylet_report_resources_period_milliseconds"] = syncer_ms
    env["RAY_ray_syncer_message_refresh_interval_ms"] = syncer_ms
elif mode == "bonly":  # ONLY reset timer -> SYNCER_MS; report period left default (100ms)
    env["RAY_ray_syncer_message_refresh_interval_ms"] = syncer_ms
elif mode == "aonly":  # ONLY report period -> SYNCER_MS; reset timer left default (3s)
    env["RAY_raylet_report_resources_period_milliseconds"] = syncer_ms

d = {
    "name": f"spillprobe-{mode}-{syncer_ms}ms-{probe}-{int(time.time())}",
    "image_uri": IMAGE,
    "working_dir": "./spillprobe_job",
    "entrypoint": f"python {os.environ.get('ENTRYPOINT_FILE', 'actor_spillback_2k.py')}",
    "env_vars": env,
    "max_retries": 0,
    "compute_config": {
        "cloud": "anyscale_v2_default_cloud",
        "head_node": {"instance_type": head},
        "worker_nodes": [{
            "instance_type": "m5.2xlarge",
            "min_nodes": workers,
            "max_nodes": workers,
            "market_type": market,
        }],
    },
}
jid = anyscale.job.submit(JobConfig.from_dict(d))
print(f"MODE={mode} WORKERS={workers} MARKET={market} JOB_ID={jid}")
