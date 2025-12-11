"""
Test that runtime_env_agent port is correctly discovered and passed.

Test cases:
1. ray start --head (auto port discovery)
2. ray start --head with fixed runtime-env-agent-port
3. ray.init() local cluster (auto port discovery)
(we don't have ray.init() with fixed _runtime_env_agent_port)
"""

import subprocess
import sys
import time
import urllib.error
import urllib.request

import ray
from ray._common.network_utils import find_free_port


def cleanup_ray():
    subprocess.run(["ray", "stop", "--force"], capture_output=True)
    time.sleep(1)


def verify_agent_port(expected_port=None):
    nodes = ray.nodes()
    if not nodes:
        print("  FAILED: No nodes found")
        return False

    agent_port = nodes[0].get("RuntimeEnvAgentPort")
    print(f"  Runtime Env Agent Port: {agent_port}")

    if not agent_port or agent_port <= 0:
        print(f"  FAILED: Invalid agent port: {agent_port}")
        return False

    if expected_port is not None and agent_port != expected_port:
        print(f"  FAILED: Expected port {expected_port}, got {agent_port}")
        return False

    print("  PASSED: Agent port is valid!")

    # Try to connect to the agent
    node_ip = nodes[0]["NodeManagerAddress"]

    try:
        url = f"http://{node_ip}:{agent_port}/"
        print(f"  Trying to connect to agent at {url}")
        req = urllib.request.Request(url, method="GET")
        urllib.request.urlopen(req, timeout=5)
        print("  PASSED: Agent responded!")
    except urllib.error.HTTPError as e:
        # 404 or other HTTP error is fine - it means we connected
        print(f"  PASSED: Agent responded with HTTP {e.code} (connection works)")
    except urllib.error.URLError as e:
        print(f"  FAILED: Cannot connect to agent: {e}")
        return False

    return True


def test_ray_start_auto_port():
    """Test 1: ray start --head with auto port discovery."""
    print("\n" + "=" * 60)
    print("Test 1: ray start --head (auto port discovery)")
    print("=" * 60)

    cleanup_ray()

    print("\n[Starting Ray head node...]")
    proc = subprocess.Popen(
        ["ray", "start", "--head", "--port=0"],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    stdout, _ = proc.communicate(timeout=60)
    print(stdout.decode())

    try:
        import ray

        ray.init()

        result = verify_agent_port()

        ray.shutdown()
        return result

    finally:
        cleanup_ray()


def test_ray_start_fixed_port():
    """Test 2: ray start --head with fixed runtime-env-agent-port."""
    print("\n" + "=" * 60)
    print("Test 2: ray start --head with fixed runtime-env-agent-port")
    print("=" * 60)

    cleanup_ray()

    fixed_port = find_free_port()
    print(f"\n[Starting Ray head node with --runtime-env-agent-port={fixed_port}...]")
    proc = subprocess.Popen(
        [
            "ray",
            "start",
            "--head",
            "--port=0",
            f"--runtime-env-agent-port={fixed_port}",
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    stdout, _ = proc.communicate(timeout=60)
    print(stdout.decode())

    try:
        import ray

        ray.init()

        result = verify_agent_port(expected_port=fixed_port)

        ray.shutdown()
        return result

    finally:
        cleanup_ray()


def test_ray_init_local():
    """Test 3: ray.init() starts local cluster with auto port discovery."""
    print("\n" + "=" * 60)
    print("Test 3: ray.init() local cluster (auto port discovery)")
    print("=" * 60)

    cleanup_ray()

    try:
        import ray

        print("\n[Starting Ray via ray.init()...]")
        ray.init()

        result = verify_agent_port()

        ray.shutdown()
        return result

    finally:
        cleanup_ray()


def main():
    print("=" * 60)
    print("Runtime Env Agent Port Discovery Tests")
    print("=" * 60)

    tests = [
        ("ray start --head (auto port)", test_ray_start_auto_port),
        ("ray start --head (fixed port)", test_ray_start_fixed_port),
        ("ray.init() local (auto port)", test_ray_init_local),
    ]

    results = []
    for name, test_func in tests:
        try:
            success = test_func()
            results.append((name, success))
        except Exception as e:
            print(f"\n  ERROR: {e}")
            results.append((name, False))

    # Summary
    print("\n" + "=" * 60)
    print("TEST SUMMARY")
    print("=" * 60)

    all_passed = True
    for name, success in results:
        status = "✓ PASSED" if success else "✗ FAILED"
        print(f"  {status}: {name}")
        if not success:
            all_passed = False

    print("=" * 60)
    if all_passed:
        print("ALL TESTS PASSED!")
    else:
        print("SOME TESTS FAILED!")
    print("=" * 60)

    return all_passed


if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)
