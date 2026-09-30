"""Provide the explicit Cart-Pole Task registration and per-Run factories."""

from __future__ import annotations

from ...core.config import LoggingConfig, RslRlRunnerConfig, SessionConfig, WorkerConfig
from ...core.config.models import DirectTaskConfig
from ...core.config.robot import RobotSpec
from ...core.direct.robot_observation import ObservationShapeTable
from ...core.direct.task import DirectTask
from ..registry.models import TaskRegistration
from ..registry.tasks import TaskRegistry
from .composed import build_cartpole_composed_cfg
from .config import (
    CARTPOLE_CONTROL_DT,
    CARTPOLE_ENVIRONMENT_ID,
    CARTPOLE_ROBOT_ID,
    CARTPOLE_TASK_ID,
    CARTPOLE_TASK_VERSION,
    CartPoleTaskConfig,
    load_cartpole_training_config,
)


def create_cartpole_task_config() -> CartPoleTaskConfig:
    """Create one fresh typed Cart-Pole Task Config from canonical YAML."""

    return load_cartpole_training_config().task


def create_cartpole_worker_config() -> WorkerConfig:
    """Create one fresh Worker Config from the canonical Cart-Pole YAML."""

    return load_cartpole_training_config().worker


def create_cartpole_runner_config() -> RslRlRunnerConfig:
    """Create the Python-only runner settings from the canonical Cart-Pole YAML."""

    return load_cartpole_training_config().runner


def create_cartpole_task(
    config: DirectTaskConfig,
    *,
    robot_spec: RobotSpec | None = None,
    observation_shapes: ObservationShapeTable | None = None,
) -> DirectTask:
    """Create one Cart-Pole DirectTask from resolved Config and optional RobotSpec."""

    params = CartPoleTaskConfig.from_direct(config)
    training = load_cartpole_training_config()
    return DirectTask(
        robot_spec=robot_spec,
        observation_shapes=observation_shapes,
        control_dt=CARTPOLE_CONTROL_DT,
        batch_size=training.worker.slot_count,
        device=training.runner.device,
        cfg_factory=lambda spec: build_cartpole_composed_cfg(params, spec),
        task_config=params,
    )


def create_cartpole_registration() -> TaskRegistration:
    """Create the explicit registration metadata for Cart-Pole."""

    return TaskRegistration(
        task_id=CARTPOLE_TASK_ID,
        task_version=CARTPOLE_TASK_VERSION,
        environment_id=CARTPOLE_ENVIRONMENT_ID,
        robot_id=CARTPOLE_ROBOT_ID,
        task_factory=create_cartpole_task,
        worker_config_factory=create_cartpole_worker_config,
        task_config_factory=create_cartpole_task_config,
        runner_config_factory=create_cartpole_runner_config,
        session_config=SessionConfig(),
        logging_config=LoggingConfig(),
    )


def register_cartpole(registry: TaskRegistry) -> None:
    """Register Cart-Pole explicitly in a caller-owned Task Registry."""

    registry.register(create_cartpole_registration())


__all__ = [
    "create_cartpole_registration",
    "create_cartpole_runner_config",
    "create_cartpole_task",
    "create_cartpole_task_config",
    "create_cartpole_worker_config",
    "register_cartpole",
]
