from typing import Callable

from ray.data._internal.dataset_logger import DatasetLogger
from ray.data.context import DataContext
from ray.util import log_once
from ray.util.annotations import DeveloperAPI


@DeveloperAPI
class UserCodeException(Exception):
    """Represents an Exception originating from user code, e.g.
    user-specified UDF used in a Ray Data transformation.

    By default, the frames corresponding to Ray Data internal files are
    omitted from the stack trace logged to stdout, but will still be
    emitted to the Ray Data specific log file. To emit all stack frames to stdout,
    set `DataContext.log_internal_stack_trace_to_stdout` to True."""

    pass


@DeveloperAPI
class SystemException(Exception):
    """Represents an Exception originating from Ray Data internal code
    or Ray Core private code paths, as opposed to user code. When
    Exceptions of this form are raised, it likely indicates a bug
    in Ray Data or Ray Core."""

    pass


# Logger used by Ray Data to log Exceptions while skip internal stack frames.
data_exception_logger = DatasetLogger(__name__)


@DeveloperAPI
def omit_traceback_stdout(fn: Callable) -> Callable:
    """Decorator which runs the function, and if there is an exception raised,
    drops the stack trace before re-raising the exception. The original exception,
    including the full unmodified stack trace, is always written to the Ray Data
    log file at `data_exception_logger._log_path`.

    This is useful for stripping long stack traces of internal Ray Data code,
    which can otherwise obfuscate user code errors."""

    def handle_trace(*args, **kwargs):
        try:
            return fn(*args, **kwargs)
        except Exception as e:
            # Only log the full internal stack trace to stdout when configured.
            # The full stack trace will always be emitted to the Ray Data log file.
            log_to_stdout = DataContext.get_current().log_internal_stack_trace_to_stdout

            is_user_code_exception = isinstance(e, UserCodeException)
            if is_user_code_exception:
                # Exception has occurred in user code.
                if not log_to_stdout and log_once("ray_data_exception_internal_hidden"):
                    data_exception_logger.get_logger().error(
                        "Exception occurred in user code, with the abbreviated stack "
                        "trace below. By default, the Ray Data internal stack trace "
                        "is omitted from stdout, and only written to the Ray Data log "
                        f"file at {data_exception_logger._datasets_log_path}. To "
                        "output the full stack trace to stdout, set "
                        "`DataContext.log_internal_stack_trace_to_stdout` to True."
                    )
            else:
                # Exception has occurred in internal Ray Data / Ray Core code.
                data_exception_logger.get_logger().error(
                    "Exception occurred in Ray Data or Ray Core internal code. "
                    "If you continue to see this error, please open an issue on "
                    "the Ray project GitHub page with the full stack trace below: "
                    "https://github.com/ray-project/ray/issues/new/choose"
                )

            data_exception_logger.get_logger(log_to_stdout=log_to_stdout).exception(
                "Full stack trace:"
            )

            if is_user_code_exception:
                raise e.with_traceback(None)
            else:
                raise e.with_traceback(None) from SystemException()

    return handle_trace
