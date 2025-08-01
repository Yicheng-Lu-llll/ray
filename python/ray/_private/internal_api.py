import warnings
from typing import List, Tuple

import ray
import ray._private.profiling as profiling
import ray._private.services as services
import ray._private.utils as utils
import ray._private.worker
from ray._private.state import GlobalState
from ray._raylet import GcsClientOptions
from ray.core.generated import common_pb2

__all__ = ["free", "global_gc"]
MAX_MESSAGE_LENGTH = ray._config.max_grpc_message_size()


def global_gc():
    """Trigger gc.collect() on all workers in the cluster."""

    worker = ray._private.worker.global_worker
    worker.core_worker.global_gc()


def get_state_from_address(address=None):
    address = services.canonicalize_bootstrap_address_or_die(address)

    state = GlobalState()
    options = GcsClientOptions.create(
        address, None, allow_cluster_id_nil=True, fetch_cluster_id_if_nil=False
    )
    state._initialize_global_state(options)
    return state


def memory_summary(
    address=None,
    group_by="NODE_ADDRESS",
    sort_by="OBJECT_SIZE",
    units="B",
    line_wrap=True,
    stats_only=False,
    num_entries=None,
):
    from ray.dashboard.memory_utils import memory_summary

    state = get_state_from_address(address)
    reply = get_memory_info_reply(state)

    if stats_only:
        return store_stats_summary(reply)
    return memory_summary(
        state, group_by, sort_by, line_wrap, units, num_entries
    ) + store_stats_summary(reply)


def get_memory_info_reply(state, node_manager_address=None, node_manager_port=None):
    """Returns global memory info."""

    from ray.core.generated import node_manager_pb2, node_manager_pb2_grpc

    # We can ask any Raylet for the global memory info, that Raylet internally
    # asks all nodes in the cluster for memory stats.
    if node_manager_address is None or node_manager_port is None:
        # We should ask for a raylet that is alive.
        raylet = None
        for node in state.node_table():
            if node["Alive"]:
                raylet = node
                break
        assert raylet is not None, "Every raylet is dead"
        raylet_address = "{}:{}".format(
            raylet["NodeManagerAddress"], raylet["NodeManagerPort"]
        )
    else:
        raylet_address = "{}:{}".format(node_manager_address, node_manager_port)

    channel = utils.init_grpc_channel(
        raylet_address,
        options=[
            ("grpc.max_send_message_length", MAX_MESSAGE_LENGTH),
            ("grpc.max_receive_message_length", MAX_MESSAGE_LENGTH),
        ],
    )

    stub = node_manager_pb2_grpc.NodeManagerServiceStub(channel)
    reply = stub.FormatGlobalMemoryInfo(
        node_manager_pb2.FormatGlobalMemoryInfoRequest(include_memory_info=False),
        timeout=60.0,
    )
    return reply


def node_stats(
    node_manager_address=None, node_manager_port=None, include_memory_info=True
):
    """Returns NodeStats object describing memory usage in the cluster."""

    from ray.core.generated import node_manager_pb2, node_manager_pb2_grpc

    # We can ask any Raylet for the global memory info.
    assert node_manager_address is not None and node_manager_port is not None
    raylet_address = "{}:{}".format(node_manager_address, node_manager_port)
    channel = utils.init_grpc_channel(
        raylet_address,
        options=[
            ("grpc.max_send_message_length", MAX_MESSAGE_LENGTH),
            ("grpc.max_receive_message_length", MAX_MESSAGE_LENGTH),
        ],
    )

    stub = node_manager_pb2_grpc.NodeManagerServiceStub(channel)
    node_stats = stub.GetNodeStats(
        node_manager_pb2.GetNodeStatsRequest(include_memory_info=include_memory_info),
        timeout=30.0,
    )
    return node_stats


def store_stats_summary(reply):
    """Returns formatted string describing object store stats in all nodes."""
    store_summary = "--- Aggregate object store stats across all nodes ---\n"
    # TODO(ekl) it would be nice if we could provide a full memory usage
    # breakdown by type (e.g., pinned by worker, primary, etc.)
    store_summary += (
        "Plasma memory usage {} MiB, {} objects, {}% full, {}% "
        "needed\n".format(
            int(reply.store_stats.object_store_bytes_used / (1024 * 1024)),
            reply.store_stats.num_local_objects,
            round(
                100
                * reply.store_stats.object_store_bytes_used
                / reply.store_stats.object_store_bytes_avail,
                2,
            ),
            round(
                100
                * reply.store_stats.object_store_bytes_primary_copy
                / reply.store_stats.object_store_bytes_avail,
                2,
            ),
        )
    )
    if reply.store_stats.object_store_bytes_fallback > 0:
        store_summary += "Plasma filesystem mmap usage: {} MiB\n".format(
            int(reply.store_stats.object_store_bytes_fallback / (1024 * 1024))
        )
    if reply.store_stats.spill_time_total_s > 0:
        store_summary += (
            "Spilled {} MiB, {} objects, avg write throughput {} MiB/s\n".format(
                int(reply.store_stats.spilled_bytes_total / (1024 * 1024)),
                reply.store_stats.spilled_objects_total,
                int(
                    reply.store_stats.spilled_bytes_total
                    / (1024 * 1024)
                    / reply.store_stats.spill_time_total_s
                ),
            )
        )
    if reply.store_stats.restore_time_total_s > 0:
        store_summary += (
            "Restored {} MiB, {} objects, avg read throughput {} MiB/s\n".format(
                int(reply.store_stats.restored_bytes_total / (1024 * 1024)),
                reply.store_stats.restored_objects_total,
                int(
                    reply.store_stats.restored_bytes_total
                    / (1024 * 1024)
                    / reply.store_stats.restore_time_total_s
                ),
            )
        )
    if reply.store_stats.consumed_bytes > 0:
        store_summary += "Objects consumed by Ray tasks: {} MiB.\n".format(
            int(reply.store_stats.consumed_bytes / (1024 * 1024))
        )
    if reply.store_stats.object_pulls_queued:
        store_summary += "Object fetches queued, waiting for available memory."

    return store_summary


def free(object_refs: list, local_only: bool = False):
    """
    DeprecationWarning: `free` is a deprecated API and will be
    removed in a future version of Ray. If you have a use case
    for this API, please open an issue on GitHub.

    Free a list of IDs from the in-process and plasma object stores.

    This function is a low-level API which should be used in restricted
    scenarios.

    If local_only is false, the request will be send to all object stores.

    This method will not return any value to indicate whether the deletion is
    successful or not. This function is an instruction to the object store. If
    some of the objects are in use, the object stores will delete them later
    when the ref count is down to 0.

    Examples:

        .. testcode::

            import ray

            @ray.remote
            def f():
                return 0

            obj_ref = f.remote()
            ray.get(obj_ref)  # wait for object to be created first
            free([obj_ref])  # unpin & delete object globally

    Args:
        object_refs (List[ObjectRef]): List of object refs to delete.
        local_only: Whether only deleting the list of objects in local
            object store or all object stores.
    """
    warnings.warn(
        "`free` is a deprecated API and will be removed in a future version of Ray. "
        "If you have a use case for this API, please open an issue on GitHub.",
        DeprecationWarning,
    )
    worker = ray._private.worker.global_worker

    if isinstance(object_refs, ray.ObjectRef):
        object_refs = [object_refs]

    if not isinstance(object_refs, list):
        raise TypeError(
            "free() expects a list of ObjectRef, got {}".format(type(object_refs))
        )

    # Make sure that the values are object refs.
    for object_ref in object_refs:
        if not isinstance(object_ref, ray.ObjectRef):
            raise TypeError(
                "Attempting to call `free` on the value {}, "
                "which is not an ray.ObjectRef.".format(object_ref)
            )

    worker.check_connected()
    with profiling.profile("ray.free"):
        if len(object_refs) == 0:
            return

        worker.core_worker.free_objects(object_refs, local_only)


def get_local_ongoing_lineage_reconstruction_tasks() -> List[
    Tuple[common_pb2.LineageReconstructionTask, int]
]:
    """Return the locally submitted ongoing retry tasks
       triggered by lineage reconstruction.

    NOTE: for the lineage reconstruction task status,
    this method only returns the status known to the submitter
    (i.e. it returns SUBMITTED_TO_WORKER instead of RUNNING).

    The return type is a list of pairs where pair.first is the
    lineage reconstruction task info and pair.second is the number
    of ongoing lineage reconstruction tasks of this type.
    """

    worker = ray._private.worker.global_worker
    worker.check_connected()
    return worker.core_worker.get_local_ongoing_lineage_reconstruction_tasks()
