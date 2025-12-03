"""
Test that ray_client_server works correctly with dynamic runtime env agent port.
This verifies the end-to-end flow where:
1. Ray starts with agent on dynamic port
2. ray_client_server receives agent port via pipe
3. Client connects with runtime_env
4. ProxyManager._create_runtime_env successfully communicates with agent
"""

import ray
import subprocess
import time
import sys


def test_ray_client_with_runtime_env():
    print("=" * 60)
    print("Testing Ray Client with Runtime Env (Dynamic Port Fetching)")
    print("=" * 60)

    # Start Ray with ray client server
    print("\n[Step 1] Starting Ray with ray-client-server...")
    proc = subprocess.Popen(
        ["ray", "start", "--head", "--ray-client-server-port=25100", "--port=0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    stdout, _ = proc.communicate(timeout=60)
    print(stdout.decode())

    try:
        print("\n[Step 2] Connecting via ray.client with runtime_env...")
        # Connect with a simple runtime_env (env_vars don't require pip install)
        with ray.client("localhost:25100").env(
            {"env_vars": {"TEST_VAR": "hello"}}
        ).connect():
            print("[Step 2] Connected successfully!")

            # Verify the env var is set
            @ray.remote
            def check_env():
                import os

                return os.environ.get("TEST_VAR", "NOT_SET")

            result = ray.get(check_env.remote())
            print(f"[Step 3] Remote task env var TEST_VAR = {result}")

            if result == "hello":
                print("[Step 3] PASSED: Runtime env was applied correctly!")
            else:
                print(f"[Step 3] FAILED: Expected 'hello', got '{result}'")
                return False

        print("\n" + "=" * 60)
        print("ALL TESTS PASSED!")
        print("=" * 60)
        return True

    finally:
        print("\n[Cleanup] Stopping Ray...")
        subprocess.run(["ray", "stop", "--force"], capture_output=True)


if __name__ == "__main__":
    success = test_ray_client_with_runtime_env()
    sys.exit(0 if success else 1)
