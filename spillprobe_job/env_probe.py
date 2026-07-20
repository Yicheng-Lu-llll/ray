"""Diagnostic: is RAY_SPILLBACK_FIX3 visible to gcs_server (head) AND raylet (workers)?

The FIX3 release relay lives in gcs_server; the decrement lives in the raylet. If the
env var reaches one process but not the other, decrements leak. Reads /proc/<pid>/environ
for the local gcs_server + raylet, and runs one remote task to read a worker raylet's.
"""
import glob
import os
import subprocess

import ray

VAR = os.environ.get("PROBE_VAR", "RAY_SPILLBACK_FIX3")


def env_of(procname):
    """Return whether VAR is in the environ of the first process matching procname."""
    try:
        pids = subprocess.run(["pgrep", "-f", procname], capture_output=True,
                              text=True).stdout.split()
        for pid in pids:
            try:
                with open(f"/proc/{pid}/environ", "rb") as f:
                    environ = f.read().decode("utf-8", "replace")
                kv = dict(e.split("=", 1) for e in environ.split("\0")
                          if "=" in e)
                if procname in " ".join(open(f"/proc/{pid}/cmdline", "rb")
                                        .read().decode("utf-8", "replace").split("\0")):
                    return pid, (VAR in kv), kv.get(VAR, "<unset>")
            except Exception as e:
                continue
    except Exception as e:
        return None, None, str(e)
    return None, None, "<no proc>"


ray.init(address="auto")
print(f"=== ENV PROBE for {VAR} ===", flush=True)
print(f"driver os.environ[{VAR}] = {os.environ.get(VAR, '<unset>')}", flush=True)
for p in ("gcs_server", "raylet"):
    pid, present, val = env_of(p)
    print(f"  HEAD {p}: pid={pid} {VAR}_present={present} val={val}", flush=True)


@ray.remote(num_cpus=1)
def worker_env():
    pids = subprocess.run(["pgrep", "-f", "raylet"], capture_output=True,
                          text=True).stdout.split()
    out = {}
    for pid in pids:
        try:
            with open(f"/proc/{pid}/environ", "rb") as f:
                environ = f.read().decode("utf-8", "replace")
            kv = dict(e.split("=", 1) for e in environ.split("\0") if "=" in e)
            out[pid] = kv.get(VAR, "<unset>")
        except Exception:
            pass
    return socket_host(), out


import socket
def socket_host():
    return socket.gethostname()


host, wenv = ray.get(worker_env.remote())
print(f"  WORKER raylet @ {host}: {VAR} = {wenv}", flush=True)
ray.shutdown()
