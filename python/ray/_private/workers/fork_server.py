"""Fork server: fork pre-warmed Python workers instead of exec-ing fresh ones.

Started by the raylet (behind ``raylet_fork_worker_from_template``). This
process imports ``ray`` once and then serves fork requests over a unix
socket: each request carries the exact ``default_worker.py`` argv (plus the
environment delta) that the raylet would otherwise ``exec``. The forked child
applies the delta and runs the same worker entrypoint, skipping the Python
interpreter startup and ``import ray`` (~1.4s per worker on small nodes, and
its CPU serialization across simultaneous worker starts).

Fork safety: this process must stay single-threaded and unconnected. It only
imports ``ray`` (verified to create no threads and open no connections) and
uses blocking socket IO on the main thread. Do not add background services
here.

Protocol (newline-delimited JSON over a single raylet connection):
  request:  {"argv": [...], "env": {...}, "setpgid": bool}
  response: {"pid": <int>} | {"error": "<msg>"}
Child exits are reaped here (the raylet detects worker death through its own
connection to the worker, as with exec-ed workers).
"""

import argparse
import json
import os
import runpy
import signal
import socket
import sys

import ray  # noqa: F401  (the template warmth: pre-import ray)


def _reap_children(signum, frame):
    while True:
        try:
            pid, _ = os.waitpid(-1, os.WNOHANG)
        except ChildProcessError:
            return
        if pid == 0:
            return


def _spawn(request, inherited_sockets):
    argv = request["argv"]
    env = request.get("env", {})
    pid = os.fork()
    if pid != 0:
        return pid
    # ---- child: becomes a regular ray worker ----
    code = 1
    try:
        for s in inherited_sockets:
            try:
                s.close()
            except OSError:
                pass
        signal.signal(signal.SIGCHLD, signal.SIG_DFL)
        if request.get("setpgid", False):
            os.setpgid(0, 0)
        for key, value in env.items():
            os.environ[key] = value
        # RayConfig snapshots RAY_* env vars when ray is imported (i.e. in the
        # template, before this worker's env existed). Re-scan the env so
        # per-worker values (RAY_JOB_ID etc.) are picked up, exactly as an
        # exec-ed worker would see them at import time.
        from ray._raylet import Config

        Config.initialize("")
        import random

        random.seed()
        # argv is the full exec command: [python, .../default_worker.py, flags...].
        # Run the same entrypoint in-process; ray is already imported (COW).
        sys.argv = argv[1:]
        runpy.run_path(sys.argv[0], run_name="__main__")
        code = 0
    except SystemExit as e:
        # The worker's normal exit path (main_loop calls sys.exit). Mirror the
        # interpreter: propagate the code silently.
        if e.code is None:
            code = 0
        elif isinstance(e.code, int):
            code = e.code
        else:
            print(e.code, file=sys.stderr)
    except BaseException:
        import traceback

        traceback.print_exc()
    finally:
        os._exit(code)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--socket-path", required=True)
    args = parser.parse_args()

    signal.signal(signal.SIGCHLD, _reap_children)
    try:
        os.unlink(args.socket_path)
    except FileNotFoundError:
        pass
    server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    server.bind(args.socket_path)
    os.chmod(args.socket_path, 0o600)
    server.listen(1)

    while True:
        conn, _ = server.accept()
        buf = b""
        with conn:
            while True:
                try:
                    chunk = conn.recv(65536)
                except OSError:
                    break
                if not chunk:
                    break
                buf += chunk
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    if not line.strip():
                        continue
                    try:
                        request = json.loads(line)
                        response = {"pid": _spawn(request, [server, conn])}
                    except Exception as e:  # noqa: BLE001
                        response = {"error": repr(e)}
                    try:
                        conn.sendall((json.dumps(response) + "\n").encode())
                    except OSError:
                        break
        # The raylet reconnects after transient errors; keep accepting.


if __name__ == "__main__":
    main()
