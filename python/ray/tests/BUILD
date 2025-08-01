load("@rules_python//python:defs.bzl", "py_library", "py_test")
load("//bazel:python.bzl", "py_test_module_list")

py_library(
    name = "conftest",
    srcs = glob(["**/conftest.py"]),
    visibility = [
        "//python/ray/_common/tests:__subpackages__",
        "//python/ray/autoscaler/v2:__pkg__",
        "//python/ray/dashboard:__pkg__",
        "//python/ray/data:__pkg__",
        "//python/ray/tests:__subpackages__",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_dashboard.py",
    ],
    tags = [
        "exclusive",
        "manual",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_actor_cancel.py",
        "test_actor_group.py",
        "test_actor_lifetime.py",
        "test_actor_pool.py",
        "test_advanced.py",
        "test_advanced_2.py",
        "test_advanced_3.py",
        "test_advanced_4.py",
        "test_advanced_5.py",
        "test_advanced_6.py",
        "test_advanced_7.py",
        "test_advanced_8.py",
        "test_advanced_9.py",
        "test_aggregated_prometheus_metrics.py",
        "test_async.py",
        "test_asyncio.py",
        "test_component_failures_2.py",
        "test_component_failures_3.py",
        "test_dashboard_profiler.py",
        "test_exit_observability.py",
        "test_experimental_collective.py",
        "test_failure_3.py",
        "test_gcs_utils.py",
        "test_get_locations.py",
        "test_global_state.py",
        "test_healthcheck.py",
        "test_metrics_agent.py",
        "test_metrics_head.py",
        "test_multiprocessing.py",
        "test_multiprocessing_standalone.py",
        "test_node_label_scheduling_strategy.py",
        "test_object_spilling_2.py",
        "test_reference_counting_2.py",
        "test_reference_counting_standalone.py",
        "test_runtime_env_agent.py",
    ],
    tags = [
        "exclusive",
        "medium_size_python_tests_a_to_j",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    env = {
        "RAY_experimental_enable_open_telemetry_on_agent": "1",
        "RAY_experimental_enable_open_telemetry_on_core": "1",
    },
    files = [
        "test_metrics_agent.py",
    ],
    name_suffix = "_open_telemetry",
    tags = [
        "exclusive",
        "medium_size_python_tests_a_to_j",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_global_gc.py",
        "test_job.py",
        "test_kill_subprocesses.py",
        "test_memstat.py",
    ],
    tags = [
        "exclusive",
        "medium_size_python_tests_a_to_j",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_joblib",
    size = "medium",
    srcs = ["test_joblib.py"],
    data = ["mnist_784_100_samples.pkl"],
    tags = [
        "exclusive",
        "medium_size_python_tests_a_to_j",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

##### Begin Ray Client tests #####

py_test_module_list(
    size = "large",
    files = [
        "test_client.py",
        "test_client_reconnect.py",
    ],
    tags = [
        "exclusive",
        "ray_client",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_client_builder.py",
        "test_client_multi.py",
        "test_client_proxy.py",
        "test_client_references.py",
        "test_client_warnings.py",
    ],
    tags = [
        "exclusive",
        "ray_client",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_client_init.py",
    ],
    tags = [
        "exclusive",
        "no_windows",
        "ray_client",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    env = {
        "RAY_CLIENT_MODE": "1",
        "RAY_PROFILING": "1",
    },
    files = [
        "test_actor.py",
        "test_advanced.py",
        "test_asyncio.py",
        "test_basic.py",
        "test_basic_2.py",
        "test_basic_3.py",
        "test_basic_4.py",
        "test_basic_5.py",
        "test_list_actors.py",
        "test_list_actors_2.py",
        "test_list_actors_3.py",
        "test_list_actors_4.py",
        "test_multiprocessing.py",
        "test_placement_group.py",
        "test_placement_group_2.py",
        "test_placement_group_3.py",
        "test_placement_group_4.py",
        "test_placement_group_5.py",
        "test_scheduling.py",
        "test_scheduling_2.py",
        "test_wait.py",
    ],
    name_suffix = "_client_mode",
    tags = [
        "exclusive",
        "ray_client",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

# Ray Client runtime_env tests that *don't* require being part of the post-wheel build.
py_test_module_list(
    size = "large",
    data = ["pip_install_test-0.5-py3-none-any.whl"],
    env = {
        "RAY_CLIENT_MODE": "1",
    },
    files = [
        "test_runtime_env.py",
        "test_runtime_env_env_vars.py",
        "test_runtime_env_failure.py",
        "test_runtime_env_working_dir.py",
        "test_runtime_env_working_dir_uri.py",
    ],
    name_suffix = "_client_mode",
    tags = [
        "exclusive",
        "ray_client",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

# Ray Client runtime_env tests that *do* require being part of the post-wheel build.
py_test_module_list(
    size = "large",
    data = [
        "pip_install_test-0.5-py3-none-any.whl",
    ],
    env = {
        "RAY_CLIENT_MODE": "1",
    },
    files = [
        "test_runtime_env_conda_and_pip.py",
        "test_runtime_env_uv_run.py",
    ],
    name_suffix = "_client_mode",
    tags = [
        "exclusive",
        "post_wheel_build",
        "ray_client",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

##### End Ray Client tests #####

# Issue #33491
# Once test_memory_deadlock is fixed, remove this rule and move
# test_memory_deadlock.py to the files list in the rule below.
# Also, edit test_memory_deadlock and uncomment the last line
# (the pytest.main invocation in the if __name__ == "__main__":
# block)
py_test_module_list(
    size = "medium",
    files = [
        "test_memory_deadlock.py",  # Timing out right now. #33491
    ],
    tags = [
        "exclusive",
        "medium_size_python_tests_k_to_z",
        "no_main",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "accelerators/test_hpu.py",
        "accelerators/test_neuron.py",
        "test_actor_bounded_threads.py",
        "test_actor_retry_1.py",
        "test_actor_retry_2.py",
        "test_actor_state_metrics.py",
        "test_autoscaler_fake_scaledown.py",
        "test_draining.py",
        "test_logging.py",
        "test_logging_2.py",
        "test_metrics.py",
        "test_multi_node_2.py",
        "test_multi_tenancy.py",
        "test_multinode_failures.py",
        "test_namespace.py",
        "test_nested_task.py",
        "test_node_labels.py",
        "test_node_manager.py",
        "test_object_assign_owner.py",
        "test_object_store_metrics.py",
        "test_placement_group_2.py",
        "test_placement_group_4.py",
        "test_placement_group_failover.py",
        "test_ray_debugger.py",
        "test_ray_init.py",
        "test_ray_init_2.py",
        "test_ray_shutdown.py",
        "test_resource_metrics.py",
        "test_runtime_context.py",
        "test_runtime_env_env_vars.py",
        "test_runtime_env_packaging.py",
        "test_runtime_env_plugin.py",
        "test_runtime_env_setup_func.py",
        "test_runtime_env_strong_type.py",
        "test_scheduling.py",
        "test_serialization.py",
        "test_shuffle.py",
        "test_state_api_log.py",
        "test_streaming_generator.py",
        "test_streaming_generator_2.py",
        "test_streaming_generator_3.py",
        "test_streaming_generator_4.py",
        "test_streaming_generator_backpressure.py",
        "test_task_metrics.py",
        "test_tempdir.py",
        "test_tls_auth.py",
        "test_traceback.py",
        "test_worker_capping.py",
        "test_worker_state.py",
    ],
    tags = [
        "exclusive",
        "medium_size_python_tests_k_to_z",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_multi_node_3.py",
        "test_object_manager.py",
        "test_resource_demand_scheduler.py",
        "test_stress.py",
        "test_stress_sharded.py",
    ],
    tags = [
        "exclusive",
        "medium_size_python_tests_k_to_z",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

##### Begin 'minimal installation' tests #####

py_test_module_list(
    size = "small",
    files = [
        "test_basic_3.py",
        "test_label_utils.py",
        "test_utils.py",
    ],
    tags = [
        "basic_test",
        "exclusive",
        "minimal",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_basic_2.py",
        "test_basic_4.py",
        "test_basic_5.py",
    ],
    tags = [
        "basic_test",
        "exclusive",
        "minimal",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_basic.py",
    ],
    tags = [
        "basic_test",
        "exclusive",
        "minimal",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_bundle_label_selector.py",
        "test_label_scheduling.py",
        "test_minimal_install.py",
        "test_path_utils.py",
        "test_runtime_env_ray_minimal.py",
    ],
    tags = [
        "exclusive",
        "minimal",
        "no_basic_test",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_output.py",
    ],
    tags = [
        "exclusive",
        "minimal",
        "no_basic_test",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

##### End 'minimal installation' tests #####

py_test_module_list(
    size = "small",
    files = [
        "accelerators/test_accelerators.py",
        "accelerators/test_amd_gpu.py",
        "accelerators/test_intel_gpu.py",
        "accelerators/test_npu.py",
        "accelerators/test_nvidia_gpu.py",
        "accelerators/test_tpu.py",
        "test_actor_lineage_reconstruction.py",
        "test_actor_out_of_order.py",
        "test_annotations.py",
        "test_args.py",
        "test_async_compat.py",
        "test_asyncio_cluster.py",
        "test_autoscaling_policy.py",
        "test_bounded_unix_sockets.py",
        "test_component_failures.py",
        "test_concurrency_group.py",
        "test_core_worker_io_thread_stack_size.py",
        "test_cross_language.py",
        "test_debug_tools.py",
        "test_distributed_sort.py",
        "test_environ.py",
        "test_error_ray_not_initialized.py",
        "test_exceptiongroup.py",
        "test_gcs_pubsub.py",
        "test_get_or_create_actor.py",
        "test_grpc_client_credentials.py",
        "test_ids.py",
        "test_kill_raylet_signal_log.py",
        "test_list_actors.py",
        "test_list_actors_2.py",
        "test_list_actors_3.py",
        "test_list_actors_4.py",
        "test_log_dedup.py",
        "test_memory_scheduling.py",
        "test_metrics_agent_2.py",
        "test_microbenchmarks.py",
        "test_mini.py",
        "test_mpi.py",
        "test_multinode_failures_2.py",
        "test_node_death.py",
        "test_numba.py",
        "test_object_spilling_no_asan.py",
        "test_open_telemetry_metric_recorder.py",
        "test_placement_group_metrics.py",
        "test_protobuf_compatibility.py",
        "test_queue.py",
        "test_raylet_output.py",
        "test_reconstruction_stress.py",
        "test_reconstruction_stress_spill.py",
        "test_reference_counting.py",
        "test_runtime_env_fork_process.py",
        "test_runtime_env_get_wheel_names.py",
        "test_runtime_env_py_executable.py",
        "test_state_api_summary.py",
        "test_streaming_generator_regression.py",
        "test_system_metrics.py",
        "test_task_events_3.py",
        "test_task_metrics_reconstruction.py",
        "test_top_level_api.py",
        "test_tqdm.py",
        "test_unhandled_error.py",
        "test_wait.py",
        "test_widgets.py",
        "test_worker_graceful_shutdown.py",
    ],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "small",
    files = [
        "test_channel_serialization.py",
    ],
    tags = [
        "compiled_graphs",
        "exclusive",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_channel.py",
        "test_nccl_channel.py",
    ],
    tags = [
        "compiled_graphs",
        "exclusive",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_gpu_objects_gloo.py",
    ],
    tags = [
        "exclusive",
        "gpu_objects",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    env = {"RAY_PYTEST_USE_GPU": "1"},
    files = [
        "test_gpu_objects_nccl.py",
    ],
    tags = [
        "exclusive",
        "gpu_objects",
        "multi_gpu",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "small",
    data = glob(["tls/*"]),
    files = [
        "test_redis_tls.py",
    ],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_gcs_ha_e2e.py",
        "test_gcs_ha_e2e_2.py",
    ],
    tags = [
        "exclusive",
        "ha_integration",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_network_failure_e2e.py",
    ],
    tags = [
        "exclusive",
        "ha_integration",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = ["test_memory_pressure.py"],
    tags = [
        "exclusive",
        "mem_pressure",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_autoscaler_e2e.py",
        "test_autoscaler_fake_multinode.py",  # Temporarily owned by core.
    ],
    tags = [
        "exclusive",
        "medium_size_python_tests_k_to_z",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_autoscaler.py",
    ],
    tags = [
        "exclusive",
        "no_windows",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "small",
    files = [
        "test_autoscaler_gcp.py",
        "test_autoscaler_util.py",
    ],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "small",
    files = [
        "autoscaler/test_providers.py",
    ],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "small",
    files = [
        "kuberay/test_autoscaling_config.py",
        "kuberay/test_kuberay_node_provider.py",
        "test_cli_logger.py",
        "test_client_metadata.py",
        "test_client_terminate.py",
        "test_coordinator_server.py",
        "test_monitor.py",
        "test_node_provider_availability_tracker.py",
        "test_response_cache.py",
    ],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "small",
    files = [
        "test_batch_node_provider_integration.py",
        "test_batch_node_provider_unit.py",
        "test_command_runner.py",
    ],
    tags = [
        "exclusive",
        "no_windows",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_autoscaler_yaml",
    size = "small",
    srcs = ["test_autoscaler_yaml.py"],
    data = [
        "additional_property.yaml",
        "test_cli_patterns/test_multi_node.yaml",
        "test_cli_patterns/test_no_head.yaml",
        "test_cli_patterns/test_no_workers.yaml",
        "//python/ray/autoscaler/aws:example",
        "//python/ray/autoscaler/azure:example",
        "//python/ray/autoscaler/gcp:example",
        "//python/ray/autoscaler/local:example",
    ],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_actor_resources.py",
        "test_autoscaler_drain_node_api.py",
        "test_dataclient_disconnect.py",
        "test_iter.py",
        "test_placement_group.py",
        "test_state_api_2.py",
        "test_task_events.py",
        "test_unavailable_actors.py",
    ],
    tags = [
        "exclusive",
        "medium_size_python_tests_a_to_j",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_actor.py",
        "test_actor_failures.py",
        "test_cancel.py",
        "test_chaos.py",
        "test_failure.py",
        "test_failure_2.py",
        "test_generators.py",
        "test_multi_node.py",
        "test_placement_group_3.py",
        "test_placement_group_5.py",
        "test_reconstruction.py",
        "test_reconstruction_2.py",
        "test_runtime_env_working_dir_uri.py",
        "test_state_api.py",
        "test_task_events_2.py",
    ],
    tags = [
        "exclusive",
        "large_size_python_tests_shard_0",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_actor_advanced.py",
        "test_gcs_fault_tolerance.py",
    ],
    tags = [
        "exclusive",
        "large_size_python_tests_shard_0",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_cli",
    size = "large",
    srcs = ["test_cli.py"],
    data = glob([
        "test_cli_patterns/*.txt",
        "test_cli_patterns/*.yaml",
    ]),
    tags = [
        "exclusive",
        "large_size_python_tests_shard_0",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_out_of_disk_space.py",
    ],
    tags = [
        "exclusive",
        "team:core",
        "tmpfs",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_failure_4.py",
        "test_object_spilling.py",
        "test_object_spilling_3.py",
        "test_placement_group_mini_integration.py",
        "test_scheduling_2.py",
    ],
    tags = [
        "exclusive",
        "large_size_python_tests_shard_1",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    files = [
        "test_implicit_resource.py",
        "test_plasma_unlimited.py",
        "test_threaded_actor.py",
    ],
    tags = [
        "exclusive",
        "large_size_python_tests_shard_1",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    data = ["pip_install_test-0.5-py3-none-any.whl"],
    files = [
        "test_runtime_env_failure.py",
        "test_runtime_env_profiler.py",
    ],
    tags = [
        "exclusive",
        "medium_size_python_tests_a_to_j",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "medium",
    data = ["pip_install_test-0.5-py3-none-any.whl"],
    files = [
        "test_runtime_env.py",
        "test_runtime_env_working_dir_2.py",
        "test_runtime_env_working_dir_3.py",
    ],
    tags = [
        "exclusive",
        "medium_size_python_tests_a_to_j",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    data = ["pip_install_test-0.5-py3-none-any.whl"],
    files = [
        "test_runtime_env_standalone.py",
        "test_runtime_env_working_dir.py",
        "test_runtime_env_working_dir_4.py",
    ],
    tags = [
        "exclusive",
        "large_size_python_tests_shard_2",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_runtime_env_complicated.py",
        "test_runtime_env_conda_and_pip.py",
        "test_runtime_env_conda_and_pip_2.py",
        "test_runtime_env_conda_and_pip_3.py",
        "test_runtime_env_conda_and_pip_4.py",
        "test_runtime_env_conda_and_pip_5.py",
        "test_runtime_env_uv.py",
        "test_runtime_env_uv_run.py",
    ],
    tags = [
        "exclusive",
        "post_wheel_build",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_resource_isolation_config",
    size = "medium",
    srcs = ["test_resource_isolation_config.py"],
    tags = [
        "exclusive",
        "no_windows",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_runtime_env_container",
    size = "large",
    srcs = ["test_runtime_env_container.py"],
    tags = [
        "container",
        "exclusive",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

# TODO(barakmich): aws/ might want its own buildfile, or
#    py_test_module_list should support subdirectories.
py_test(
    name = "test_autoscaler_aws",
    size = "small",
    srcs = ["aws/test_autoscaler_aws.py"],
    tags = [
        "exclusive",
        "no_windows",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_aws_batch_tag_update",
    size = "small",
    srcs = ["aws/test_aws_batch_tag_update.py"],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_gcp_node_provider",
    size = "small",
    srcs = ["gcp/test_gcp_node_provider.py"],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_gcp_tpu_command_runner",
    size = "small",
    srcs = ["gcp/test_gcp_tpu_command_runner.py"],
    tags = [
        "exclusive",
        "no_windows",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "vsphere/test_cluster_operator",
    size = "small",
    srcs = ["vsphere/test_cluster_operator.py"],
    tags = [
        "exclusive",
        "no_windows",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "vsphere/test_vmray_node_provider",
    size = "small",
    srcs = ["vsphere/test_vmray_node_provider.py"],
    tags = [
        "exclusive",
        "no_windows",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

# Note(simon): typing tests are not included in module list
#    because they requires globs and it might be refactored in the future.
py_test(
    name = "test_typing",
    size = "small",
    srcs = [
        "test_typing.py",
        "typing_files/check_typing_bad.py",
        "typing_files/check_typing_good.py",
    ],
    # Note(can): known issue of mypy and latest torch on windows
    # (https://github.com/python/mypy/issues/17189)
    tags = [
        "exclusive",
        "no_windows",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

# TODO: use py_test(env = ...) in the build file with bazel 4.0
py_test(
    name = "test_tracing",
    size = "medium",
    srcs = ["test_tracing.py"],
    tags = [
        "exclusive",
        "medium_size_python_tests_k_to_z",
        "no_windows",
        "serve_tracing",
        "team:serve",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_pydantic_serialization",
    size = "small",
    srcs = [
        "pydantic_module.py",
        "test_pydantic_serialization.py",
    ],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:serve",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "test_collections_utils",
    size = "small",
    srcs = ["test_collections_utils.py"],
    tags = [
        "exclusive",
        "small_size_python_tests",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test(
    name = "kuberay/test_autoscaling_e2e",
    size = "large",
    srcs = ["kuberay/test_autoscaling_e2e.py"],
    tags = [
        "exclusive",
        "no_windows",
        "team:kuberay",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    env = {
        "RAY_DEBUG_MODE": "1",
    },
    files = [
        "test_object_spilling.py",
        "test_object_spilling_2.py",
        "test_object_spilling_3.py",
        "test_scheduling.py",
        "test_scheduling_2.py",
    ],
    name_suffix = "_debug_mode",
    tags = [
        "debug_tests",
        "exclusive",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_object_spilling.py",
        "test_object_spilling_2.py",
        "test_object_spilling_3.py",
        "test_scheduling.py",
        "test_scheduling_2.py",
    ],
    name_suffix = "_asan",
    tags = [
        "asan_tests",
        "exclusive",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "enormous",
    data = [
        "spark/discover_2_gpu.sh",
        "spark/discover_4_gpu.sh",
    ],
    files = [
        "spark/test_GPU.py",
        "spark/test_basic.py",
        "spark/test_databricks_hook.py",
        "spark/test_multicores_per_task.py",
        "spark/test_utils.py",
    ],
    tags = [
        "exclusive",
        "spark_on_ray",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)
