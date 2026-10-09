"""Expose task-neutral training assembly entry points."""

from typing import TYPE_CHECKING, Any

from .configuration import DEFAULT_TRAINING_MAP, WORKER_LOCKSTEP_PHYSICS_ARGS, build_launch_overrides, build_run_config
from .export import export_policy, robot_runtime_from_config
from .runs import (
    RunSummary,
    list_runs,
    make_run_directory,
    record_command,
    resolve_resume_checkpoint,
    resolve_run_directory,
)

__all__ = [
    "DEFAULT_TRAINING_MAP",
    "TrainingResult",
    "WORKER_LOCKSTEP_PHYSICS_ARGS",
    "build_launch_overrides",
    "build_rsl_rl_train_config",
    "build_run_config",
    "export_policy",
    "robot_runtime_from_config",
    "parse_config_options",
    "run_evaluation",
    "run_training",
    "RunSummary",
    "list_runs",
    "make_run_directory",
    "record_command",
    "resolve_resume_checkpoint",
    "resolve_run_directory",
]


if TYPE_CHECKING:
    from .runner import (
        TrainingResult as TrainingResult,
    )
    from .runner import (
        build_rsl_rl_train_config as build_rsl_rl_train_config,
    )
    from .runner import (
        parse_config_options as parse_config_options,
    )
    from .runner import (
        run_evaluation as run_evaluation,
    )
    from .runner import (
        run_training as run_training,
    )


def __getattr__(name: str) -> Any:
    """Load the learning runtime only for callers that request its entry points."""
    if name in {
        "TrainingResult", "build_rsl_rl_train_config", "parse_config_options", "run_evaluation", "run_training",
    }:
        from . import runner

        value = getattr(runner, name)
        globals()[name] = value
        return value
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
