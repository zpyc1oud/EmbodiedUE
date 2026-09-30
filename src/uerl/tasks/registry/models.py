"""Define typed metadata and factories for one registered Task."""

from collections.abc import Callable
from dataclasses import dataclass
from typing import Protocol, TypeAlias

from ...core.config.models import (
    DirectTaskConfig,
    LoggingConfig,
    RslRlRunnerConfig,
    SessionConfig,
    WorkerConfig,
)
from ...core.config.robot import RobotSpec
from ...core.direct.curriculum import CurriculumManager
from ...core.direct.task import DirectTask
from ...core.mdp.managers.event import EventManager
from ..evaluation import TaskEvaluator


class TaskFactory(Protocol):
    """Build a Task and optionally bind its reflected RobotSpec."""

    def __call__(self, config: DirectTaskConfig, *, robot_spec: RobotSpec | None = None) -> DirectTask:
        ...

WorkerConfigFactory: TypeAlias = Callable[[], WorkerConfig]
TaskConfigFactory: TypeAlias = Callable[[], DirectTaskConfig]
RunnerConfigFactory: TypeAlias = Callable[[], RslRlRunnerConfig]
CurriculumFactory: TypeAlias = Callable[[DirectTaskConfig, int, str, int], CurriculumManager]
EventManagerFactory: TypeAlias = Callable[..., EventManager]
EvaluationFactory: TypeAlias = Callable[[float], TaskEvaluator]
EvaluationFormatter: TypeAlias = Callable[[object], str]


@dataclass(frozen=True, slots=True)
class TaskRegistration:
    """Register a stable Task ID with per-Run Task and Config factories.

    Attributes:
        task_id: Non-empty stable lookup key.
        task_version: Version used to detect conflicting registrations.
        environment_id: Authoritative UE Environment identifier for this Task.
        robot_id: Authoritative UE Robot identifier for this Task.
        task_factory: Callable that builds a fresh DirectTask for one Config.
        worker_config_factory: Callable that builds one fresh UE projection.
        task_config_factory: Callable that builds one fresh DirectTask Config.
        runner_config_factory: Callable that builds one fresh Python runner Config.
        session_config: Default process, endpoint, and protocol settings.
        logging_config: Default Run evidence settings.
    """

    task_id: str
    task_version: str
    environment_id: str
    robot_id: str
    task_factory: TaskFactory
    worker_config_factory: WorkerConfigFactory
    task_config_factory: TaskConfigFactory
    runner_config_factory: RunnerConfigFactory
    session_config: SessionConfig
    logging_config: LoggingConfig
    curriculum_factory: CurriculumFactory | None = None
    evaluation_factory: EvaluationFactory | None = None
    evaluation_formatter: EvaluationFormatter | None = None
    event_manager_factory: EventManagerFactory | None = None
