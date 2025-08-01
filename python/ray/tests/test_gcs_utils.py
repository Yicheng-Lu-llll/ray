import asyncio
import contextlib
import os
import signal
import sys
import time

import pytest
from ray._common.test_utils import async_wait_for_condition
import redis

import ray
from ray._raylet import GcsClient, NodeID
import ray._private.gcs_utils as gcs_utils
from ray._private.test_utils import (
    external_redis_test_enabled,
    find_free_port,
    generate_system_config_map,
)
import ray._private.ray_constants as ray_constants

# Import asyncio timeout depends on python version
if sys.version_info >= (3, 11):
    from asyncio import timeout as asyncio_timeout
else:
    from async_timeout import timeout as asyncio_timeout


@contextlib.contextmanager
def stop_gcs_server():
    process = ray._private.worker._global_node.all_processes[
        ray._private.ray_constants.PROCESS_TYPE_GCS_SERVER
    ][0].process
    pid = process.pid
    os.kill(pid, signal.SIGSTOP)
    try:
        yield
    finally:
        os.kill(pid, signal.SIGCONT)


def test_kv_basic(ray_start_regular, monkeypatch):
    monkeypatch.setenv("TEST_RAY_COLLECT_KV_FREQUENCY", "1")
    gcs_address = ray._private.worker.global_worker.gcs_client.address
    gcs_client = ray._raylet.GcsClient(address=gcs_address)
    # Wait until all other calls finished
    time.sleep(2)
    # reset the counter
    ray._private.utils._CALLED_FREQ.clear()
    assert gcs_client.internal_kv_get(b"A", b"NS") is None
    assert gcs_client.internal_kv_put(b"A", b"B", False, b"NS") == 1
    assert gcs_client.internal_kv_get(b"A", b"NS") == b"B"
    assert gcs_client.internal_kv_put(b"A", b"C", False, b"NS") == 0
    assert gcs_client.internal_kv_get(b"A", b"NS") == b"B"
    assert gcs_client.internal_kv_put(b"A", b"C", True, b"NS") == 0
    assert gcs_client.internal_kv_get(b"A", b"NS") == b"C"
    assert gcs_client.internal_kv_put(b"AA", b"B", False, b"NS") == 1
    assert gcs_client.internal_kv_put(b"AB", b"B", False, b"NS") == 1
    assert set(gcs_client.internal_kv_keys(b"A", b"NS")) == {b"A", b"AA", b"AB"}
    assert gcs_client.internal_kv_del(b"A", False, b"NS") == 1
    assert set(gcs_client.internal_kv_keys(b"A", b"NS")) == {b"AA", b"AB"}
    assert gcs_client.internal_kv_keys(b"A", b"NSS") == []
    assert gcs_client.internal_kv_del(b"A", True, b"NS") == 2
    assert gcs_client.internal_kv_keys(b"A", b"NS") == []
    assert gcs_client.internal_kv_del(b"A", False, b"NSS") == 0
    assert ray._private.utils._CALLED_FREQ["internal_kv_get"] == 4
    assert ray._private.utils._CALLED_FREQ["internal_kv_put"] == 5

    # Test internal_kv_multi_get
    assert gcs_client.internal_kv_multi_get([b"A", b"B"], b"NS") == {}
    assert gcs_client.internal_kv_put(b"A", b"B", False, b"NS") == 1
    assert gcs_client.internal_kv_put(b"B", b"C", False, b"NS") == 1
    assert gcs_client.internal_kv_multi_get([b"A", b"B"], b"NS") == {
        b"A": b"B",
        b"B": b"C",
    }
    assert gcs_client.internal_kv_multi_get([b"A", b"B"], b"NSS") == {}

    # Test internal_kv_multi_get where some keys don't exist
    assert gcs_client.internal_kv_multi_get([b"A", b"B", b"C"], b"NS") == {
        b"A": b"B",
        b"B": b"C",
    }

    assert ray._private.utils._CALLED_FREQ["internal_kv_multi_get"] == 4


@pytest.mark.skipif(sys.platform == "win32", reason="Windows doesn't have signals.")
def test_kv_timeout(ray_start_regular):
    gcs_address = ray._private.worker.global_worker.gcs_client.address
    gcs_client = ray._raylet.GcsClient(address=gcs_address)

    assert gcs_client.internal_kv_put(b"A", b"", False, b"") == 1

    with stop_gcs_server():
        with pytest.raises(ray.exceptions.RpcError, match="Deadline Exceeded"):
            gcs_client.internal_kv_put(b"A", b"B", False, b"NS", timeout=2)

        with pytest.raises(ray.exceptions.RpcError, match="Deadline Exceeded"):
            gcs_client.internal_kv_get(b"A", b"NS", timeout=2)

        with pytest.raises(ray.exceptions.RpcError, match="Deadline Exceeded"):
            gcs_client.internal_kv_keys(b"A", b"NS", timeout=2)

        with pytest.raises(ray.exceptions.RpcError, match="Deadline Exceeded"):
            gcs_client.internal_kv_del(b"A", True, b"NS", timeout=2)


def test_kv_transient_network_error(shutdown_only, monkeypatch):
    monkeypatch.setenv(
        "RAY_testing_rpc_failure",
        "ray::rpc::InternalKVGcsService.grpc_client.InternalKVGet=5:25:25,"
        "ray::rpc::InternalKVGcsService.grpc_client.InternalKVPut=5:25:25",
    )
    ray.init()
    gcs_address = ray._private.worker.global_worker.gcs_client.address
    gcs_client = ray._raylet.GcsClient(address=gcs_address)

    gcs_client.internal_kv_put(b"A", b"Hello", True, b"")
    assert gcs_client.internal_kv_get(b"A", b"") == b"Hello"


@pytest.mark.asyncio
async def test_kv_basic_aio(ray_start_regular):
    gcs_client = ray._private.worker.global_worker.gcs_client

    assert await gcs_client.async_internal_kv_get(b"A", b"NS") is None
    assert await gcs_client.async_internal_kv_put(b"A", b"B", False, b"NS") == 1
    assert await gcs_client.async_internal_kv_get(b"A", b"NS") == b"B"
    assert await gcs_client.async_internal_kv_put(b"A", b"C", False, b"NS") == 0
    assert await gcs_client.async_internal_kv_get(b"A", b"NS") == b"B"
    assert await gcs_client.async_internal_kv_put(b"A", b"C", True, b"NS") == 0
    assert await gcs_client.async_internal_kv_get(b"A", b"NS") == b"C"
    assert await gcs_client.async_internal_kv_put(b"AA", b"B", False, b"NS") == 1
    assert await gcs_client.async_internal_kv_put(b"AB", b"B", False, b"NS") == 1
    keys = await gcs_client.async_internal_kv_keys(b"A", b"NS")
    assert set(keys) == {b"A", b"AA", b"AB"}
    assert await gcs_client.async_internal_kv_del(b"A", False, b"NS") == 1
    keys = await gcs_client.async_internal_kv_keys(b"A", b"NS")
    assert set(keys) == {b"AA", b"AB"}
    assert await gcs_client.async_internal_kv_keys(b"A", b"NSS") == []
    assert await gcs_client.async_internal_kv_del(b"A", True, b"NS") == 2
    assert await gcs_client.async_internal_kv_keys(b"A", b"NS") == []
    assert await gcs_client.async_internal_kv_del(b"A", False, b"NSS") == 0

    # Test internal_kv_multi_get
    assert await gcs_client.async_internal_kv_multi_get([b"A", b"B"], b"NS") == {}
    assert await gcs_client.async_internal_kv_put(b"A", b"B", False, b"NS") == 1
    assert await gcs_client.async_internal_kv_put(b"B", b"C", False, b"NS") == 1
    assert await gcs_client.async_internal_kv_multi_get([b"A", b"B"], b"NS") == {
        b"A": b"B",
        b"B": b"C",
    }

    # Test internal_kv_multi_get where some keys don't exist
    assert await gcs_client.async_internal_kv_multi_get([b"A", b"B", b"C"], b"NS") == {
        b"A": b"B",
        b"B": b"C",
    }

    assert await gcs_client.async_internal_kv_multi_get([b"A", b"B"], b"NSS") == {}


@pytest.mark.skipif(sys.platform == "win32", reason="Windows doesn't have signals.")
@pytest.mark.asyncio
async def test_kv_timeout_aio(ray_start_regular):
    gcs_client = ray._private.worker.global_worker.gcs_client

    with stop_gcs_server():
        with pytest.raises(ray.exceptions.RpcError, match="Deadline Exceeded"):
            await gcs_client.async_internal_kv_put(b"A", b"B", False, b"NS", timeout=2)

        with pytest.raises(ray.exceptions.RpcError, match="Deadline Exceeded"):
            await gcs_client.async_internal_kv_get(b"A", b"NS", timeout=2)

        with pytest.raises(ray.exceptions.RpcError, match="Deadline Exceeded"):
            await gcs_client.async_internal_kv_keys(b"A", b"NS", timeout=2)

        with pytest.raises(ray.exceptions.RpcError, match="Deadline Exceeded"):
            await gcs_client.async_internal_kv_del(b"A", True, b"NS", timeout=2)


@pytest.mark.skipif(
    not external_redis_test_enabled(),
    reason="Only valid when start with an external redis",
)
def test_external_storage_namespace_isolation(shutdown_only):
    addr = ray.init(
        namespace="a", _system_config={"external_storage_namespace": "c1"}
    ).address_info["address"]
    gcs_client = GcsClient(address=addr)

    assert gcs_client.internal_kv_put(b"ABC", b"DEF", True, None) == 1

    assert gcs_client.internal_kv_get(b"ABC", None) == b"DEF"

    ray.shutdown()

    addr = ray.init(
        namespace="a", _system_config={"external_storage_namespace": "c2"}
    ).address_info["address"]
    gcs_client = GcsClient(address=addr)
    assert gcs_client.internal_kv_get(b"ABC", None) is None
    assert gcs_client.internal_kv_put(b"ABC", b"XYZ", True, None) == 1

    assert gcs_client.internal_kv_get(b"ABC", None) == b"XYZ"
    ray.shutdown()

    addr = ray.init(
        namespace="a", _system_config={"external_storage_namespace": "c1"}
    ).address_info["address"]
    gcs_client = GcsClient(address=addr)
    assert gcs_client.internal_kv_get(b"ABC", None) == b"DEF"


@pytest.mark.asyncio
@pytest.mark.parametrize(
    "ray_start_cluster",
    [
        generate_system_config_map(
            health_check_initial_delay_ms=0,
            health_check_period_ms=1000,
            health_check_failure_threshold=2,
        ),
    ],
    indirect=["ray_start_cluster"],
)
async def test_check_liveness(monkeypatch, ray_start_cluster):
    monkeypatch.setenv("RAY_health_check_initial_delay_ms", "0")
    monkeypatch.setenv("RAY_health_check_period_ms", "1000")
    monkeypatch.setenv("RAY_health_check_failure_threshold", "2")

    cluster = ray_start_cluster
    h = cluster.add_node(node_manager_port=find_free_port())
    n1 = cluster.add_node(node_manager_port=find_free_port())
    n2 = cluster.add_node(node_manager_port=find_free_port())
    gcs_client = GcsClient(address=cluster.address)
    node_ids = [NodeID.from_hex(n.node_id) for n in [h, n1, n2]]

    ret = await gcs_client.async_check_alive(node_ids)
    assert ret == [True, True, True]

    cluster.remove_node(n1)

    async def check(expect_liveness):
        ret = await gcs_client.async_check_alive(node_ids)
        return ret == expect_liveness

    await async_wait_for_condition(check, expect_liveness=[True, False, True])

    n2_raylet_process = n2.all_processes[ray_constants.PROCESS_TYPE_RAYLET][0].process
    n2_raylet_process.kill()

    # GCS hasn't marked it as dead yet.
    ret = await gcs_client.async_check_alive(node_ids)
    assert ret == [True, False, True]

    # GCS will notice node dead soon
    await async_wait_for_condition(check, expect_liveness=[True, False, False])


@pytest.mark.asyncio
async def test_gcs_client_is_async(ray_start_regular):
    gcs_client = ray._private.worker.global_worker.gcs_client

    await gcs_client.async_internal_kv_put(b"A", b"B", False, b"NS", timeout=2)
    async with asyncio_timeout(3):
        none, result = await asyncio.gather(
            asyncio.sleep(2), gcs_client.async_internal_kv_get(b"A", b"NS", timeout=2)
        )
        assert result == b"B"

    await gcs_client.async_internal_kv_keys(b"A", b"NS", timeout=2)

    await gcs_client.async_internal_kv_del(b"A", True, b"NS", timeout=2)


@pytest.fixture(params=[True, False])
def redis_replicas(request, monkeypatch):
    if request.param:
        monkeypatch.setenv("TEST_EXTERNAL_REDIS_REPLICAS", "3")
    yield


@pytest.mark.skipif(
    not external_redis_test_enabled(),
    reason="Only valid when start with an external redis",
)
def test_redis_cleanup(redis_replicas, shutdown_only):
    addr = ray.init(
        namespace="a", _system_config={"external_storage_namespace": "c1"}
    ).address_info["address"]
    gcs_client = GcsClient(address=addr)
    gcs_client.internal_kv_put(b"ABC", b"DEF", True, None)

    ray.shutdown()
    addr = ray.init(
        namespace="a", _system_config={"external_storage_namespace": "c2"}
    ).address_info["address"]
    gcs_client = GcsClient(address=addr)
    gcs_client.internal_kv_put(b"ABC", b"XYZ", True, None)
    ray.shutdown()
    redis_addr = os.environ["RAY_REDIS_ADDRESS"]
    host, port = redis_addr.split(":")
    if os.environ.get("TEST_EXTERNAL_REDIS_REPLICAS", "1") != "1":
        cli = redis.RedisCluster(host, int(port))
    else:
        cli = redis.Redis(host, int(port))

    table_names = ["KV", "WORKERS", "JobCounter", "NODE", "JOB"]
    c1_keys = [f"RAYc1@{name}".encode() for name in table_names]
    c2_keys = [f"RAYc2@{name}".encode() for name in table_names]
    assert set(cli.keys()) == set(c1_keys + c2_keys)
    gcs_utils.cleanup_redis_storage(host, int(port), "", False, "c1")
    assert set(cli.keys()) == set(c2_keys)
    gcs_utils.cleanup_redis_storage(host, int(port), "", False, "c2")
    assert len(cli.keys()) == 0


if __name__ == "__main__":
    sys.exit(pytest.main(["-sv", __file__]))
