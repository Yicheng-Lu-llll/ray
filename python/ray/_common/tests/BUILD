load("@rules_python//python:defs.bzl", "py_library")
load("//bazel:python.bzl", "py_test_module_list")

py_library(
    name = "conftest",
    srcs = glob(["**/conftest.py"]),
    visibility = [
        "//python/ray/_common/tests:__subpackages__",
    ],
    deps = ["//python/ray/tests:conftest"],
)

# Small tests.
py_test_module_list(
    size = "small",
    files = [
        "test_ray_option_utils.py",
        "test_signal_semaphore_utils.py",
        "test_signature.py",
        "test_utils.py",
        "test_wait_for_condition.py",
    ],
    tags = [
        "exclusive",
        "team:core",
    ],
    deps = [
        "//:ray_lib",
    ],
)

py_test_module_list(
    size = "large",
    files = [
        "test_usage_stats.py",
    ],
    tags = [
        "exclusive",
        "team:core",
    ],
    deps = [
        ":conftest",
        "//:ray_lib",
    ],
)
