"""In-process fake raylets for GCS scalability testing.

One gRPC server impersonates N raylets: it answers the GCS's health checks
(grpc.health.v1) and ``GetResourceLoad`` pulls, and can optionally grant
worker leases. N ``GcsNodeInfo`` entries with distinct node ids (all pointing
at this server's address) are registered into the GCS; the raylet client pool
keys channels by node id, so a single address serves thousands of fake nodes.

This lets GCS-side scalability work (ingest throughput, main-thread load,
scheduling fan-out) iterate at 10k-node semantics on one machine, without
starting a cluster. It does NOT emulate workers: actors scheduled onto fake
nodes are leased (if ``grant_leases``) but never start.

Example:
    ray.init()
    cluster = FakeRayletCluster(num_nodes=1000, cpus_per_node=4)
    cluster.start()
    ...  # drive load against the GCS
    cluster.stop()
"""
import os
import time
from concurrent import futures

import grpc

from ray.core.generated import (
    common_pb2,
    gcs_pb2,
    gcs_service_pb2,
    gcs_service_pb2_grpc,
    node_manager_pb2,
    node_manager_pb2_grpc,
)

# grpc.health.v1.HealthCheckResponse{status: SERVING} serialized.
_HEALTH_SERVING = b"\x08\x01"


class _HealthHandler(grpc.GenericRpcHandler):
    """Answers grpc.health.v1.Health/Check with SERVING for any service.

    Implemented as a generic handler over raw bytes so the fake cluster has no
    dependency on the grpcio-health-checking package.
    """

    def service(self, handler_call_details):
        if handler_call_details.method == "/grpc.health.v1.Health/Check":
            return grpc.unary_unary_rpc_method_handler(
                lambda request, context: _HEALTH_SERVING,
                request_deserializer=None,
                response_serializer=None,
            )
        return None


class _FakeNodeManager(node_manager_pb2_grpc.NodeManagerServiceServicer):
    def __init__(self, grant_leases: bool):
        self._grant_leases = grant_leases

    def GetResourceLoad(self, request, context):
        return node_manager_pb2.GetResourceLoadReply(
            resources=common_pb2.ResourcesData()
        )

    def RequestWorkerLease(self, request, context):
        if not self._grant_leases:
            context.abort(grpc.StatusCode.UNIMPLEMENTED, "lease granting disabled")
        # Grant with a unique fake worker. The worker never starts, so this is
        # only useful for exercising the GCS-side scheduling path.
        return node_manager_pb2.RequestWorkerLeaseReply(
            worker_address=common_pb2.Address(
                node_id=os.urandom(28),
                ip_address="127.0.0.1",
                port=1,
                worker_id=os.urandom(28),
            ),
            worker_pid=1,
        )


class FakeRayletCluster:
    """Registers N fake raylets (backed by one in-process server) into a GCS."""

    def __init__(
        self,
        num_nodes: int,
        gcs_address: str = None,
        cpus_per_node: float = 4,
        memory_per_node: float = 1e9,
        listen_port: int = 0,
        labels_fn=None,
        grant_leases: bool = False,
        server_threads: int = 64,
    ):
        self._num_nodes = num_nodes
        self._gcs_address = gcs_address
        self._cpus = cpus_per_node
        self._memory = memory_per_node
        self._listen_port = listen_port
        self._labels_fn = labels_fn
        self._grant_leases = grant_leases
        self._server_threads = server_threads
        self._server = None
        self.node_ids = []

    def start(self):
        if self._gcs_address is None:
            import ray

            self._gcs_address = ray.get_runtime_context().gcs_address
        self._server = grpc.server(
            futures.ThreadPoolExecutor(max_workers=self._server_threads),
            options=[("grpc.so_reuseport", 0)],
        )
        node_manager_pb2_grpc.add_NodeManagerServiceServicer_to_server(
            _FakeNodeManager(self._grant_leases), self._server
        )
        self._server.add_generic_rpc_handlers((_HealthHandler(),))
        self._listen_port = self._server.add_insecure_port(
            f"127.0.0.1:{self._listen_port}"
        )
        self._server.start()

        channel = grpc.insecure_channel(self._gcs_address)
        stub = gcs_service_pb2_grpc.NodeInfoGcsServiceStub(channel)
        for i in range(self._num_nodes):
            node_id = os.urandom(28)
            info = gcs_pb2.GcsNodeInfo(
                node_id=node_id,
                node_manager_address="127.0.0.1",
                node_manager_hostname=f"fake-raylet-{i}",
                node_manager_port=self._listen_port,
                object_manager_port=1,
                raylet_socket_name="/tmp/fake_raylet.sock",
                object_store_socket_name="/tmp/fake_plasma.sock",
                state=gcs_pb2.GcsNodeInfo.ALIVE,
                node_name=f"fake-raylet-{i}",
                start_time_ms=int(time.time() * 1000),
            )
            info.resources_total["CPU"] = self._cpus
            info.resources_total["memory"] = self._memory
            if self._labels_fn is not None:
                for k, v in self._labels_fn(i).items():
                    info.labels[k] = v
            reply = stub.RegisterNode(
                gcs_service_pb2.RegisterNodeRequest(node_info=info), timeout=30
            )
            if reply.status.code != 0:
                raise RuntimeError(
                    f"RegisterNode failed for fake node {i}: {reply.status}"
                )
            self.node_ids.append(node_id)
        channel.close()
        return self

    def stop(self):
        if self._server is not None:
            self._server.stop(grace=None)
            self._server = None
