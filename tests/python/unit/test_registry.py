"""Verify explicit Task registration and stable lookup."""

from dataclasses import replace
from pathlib import Path
from typing import cast
from unittest.mock import Mock

import pytest

from uerl import DirectTaskConfig, LoggingConfig, RslRlRunnerConfig, SessionConfig, WorkerConfig
from uerl.errors import RegistryError
from uerl.tasks.evaluation import EvaluationSummary, format_generic_evaluation
from uerl.tasks.registry import TaskRegistration, TaskRegistry, create_default_registry
from uerl.tasks.registry.models import CurriculumFactory, TaskFactory


def _registration(task_id: str, version: str = "1.0") -> TaskRegistration:
    """Build a minimal registration for registry tests."""

    return TaskRegistration(
        task_id=task_id,
        task_version=version,
        environment_id="environment",
        robot_id="robot",
        task_factory=Mock(spec=TaskFactory),
        worker_config_factory=lambda: WorkerConfig(environment_id="environment", robot_id="robot"),
        task_config_factory=lambda: DirectTaskConfig(),
        runner_config_factory=lambda: RslRlRunnerConfig(),
        session_config=SessionConfig(),
        logging_config=LoggingConfig(),
    )


def test_registered_factories_create_fresh_run_partitions() -> None:
    """Create independent Config objects for separate Runs."""

    registry = TaskRegistry()
    registry.register(_registration("task"))

    first_task_config = registry.create_task_config("task")
    second_task_config = registry.create_task_config("task")
    first_worker_config = registry.create_worker_config("task")
    second_worker_config = registry.create_worker_config("task")
    first_runner_config = registry.create_runner_config("task")
    second_runner_config = registry.create_runner_config("task")

    assert first_task_config is not second_task_config
    assert first_worker_config is not second_worker_config
    assert first_runner_config is not second_runner_config
    print("[VERIFY] factory_partitions=FRESH task_id=task")


def test_register_resolve_and_list_are_stable() -> None:
    """Resolve the registered object and list IDs in stable order."""

    registry = TaskRegistry()
    registry.register(_registration("z-task"))
    registry.register(_registration("a-task"))

    assert registry.resolve("a-task").task_version == "1.0"
    assert [item.task_id for item in registry.list()] == ["a-task", "z-task"]
    print("[VERIFY] VC-003: duplicate=REJECTED unknown=REJECTED order=STABLE")


def test_duplicate_task_id_is_rejected() -> None:
    """Reject a second registration with the same ID and version."""

    registry = TaskRegistry()
    registry.register(_registration("task"))

    with pytest.raises(RegistryError) as raised:
        registry.register(_registration("task"))

    assert raised.value.code == "DUPLICATE_TASK_ID"
    assert raised.value.task_id == "task"


def test_task_version_conflict_is_rejected() -> None:
    """Reject a second version under an already registered Task ID."""

    registry = TaskRegistry()
    registry.register(_registration("task", "1.0"))

    with pytest.raises(RegistryError) as raised:
        registry.register(_registration("task", "2.0"))

    assert raised.value.code == "TASK_VERSION_CONFLICT"


def test_unknown_task_id_is_rejected() -> None:
    """Reject lookup of a Task ID that was not explicitly registered."""

    with pytest.raises(RegistryError) as raised:
        TaskRegistry().resolve("missing")

    assert raised.value.code == "UNKNOWN_TASK_ID"


def test_default_registry_assigns_evaluator_only_to_locomotion_task() -> None:
    """Keep generic Tasks free of PhantomX-specific evaluation requirements."""

    from uerl.tasks.cartpole import CARTPOLE_TASK_ID
    from uerl.tasks.phantomx import PHANTOMX_TASK_ID

    registry = create_default_registry()
    assert registry.create_evaluator(CARTPOLE_TASK_ID, sample_hz=50.0) is None
    assert registry.create_evaluator(PHANTOMX_TASK_ID, sample_hz=50.0).__class__.__name__ == "PhantomXEvaluator"


def test_cartpole_uses_generic_evaluation_summary() -> None:
    """Format generic CartPole evaluation without locomotion metrics."""

    result = EvaluationSummary(
        task_id="uerl.task.cartpole.v1",
        checkpoint=Path("model.pt"),
        steps=10,
        completed_episodes=2,
        mean_episode_length=5.0,
        mean_reward=1.25,
    )

    assert "mean_reward=1.250000" in format_generic_evaluation(result)
    assert "gait" not in format_generic_evaluation(result)


def test_non_callable_factory_is_rejected_at_registration_boundary() -> None:
    """Reject malformed factory inputs before they can reach a Run."""

    registration = replace(_registration("task"), task_factory=cast(TaskFactory, None))

    with pytest.raises(RegistryError) as raised:
        TaskRegistry().register(registration)

    assert raised.value.code == "INVALID_FACTORY"
    assert raised.value.task_id == "task"

    registration = replace(
        _registration("curriculum-task"),
        curriculum_factory=cast(CurriculumFactory, "invalid"),
    )
    with pytest.raises(RegistryError) as curriculum_raised:
        TaskRegistry().register(registration)
    assert curriculum_raised.value.code == "INVALID_FACTORY"
