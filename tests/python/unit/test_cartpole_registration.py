"""Verify explicit Cart-Pole Task registration and Resolver integration."""

from __future__ import annotations

from uerl import RunConfigResolver
from uerl.core.direct.task import DirectTask
from uerl.tasks.cartpole import (
    CARTPOLE_ENVIRONMENT_ID,
    CARTPOLE_ROBOT_ID,
    CARTPOLE_TASK_ID,
    CartPoleTaskConfig,
)
from uerl.tasks.registry import create_default_registry


def test_default_registry_resolves_cartpole_factories_per_run() -> None:
    """Resolve Cart-Pole Config, Task, and UE binding from one explicit ID."""

    registry = create_default_registry()
    resolver = RunConfigResolver(registry)
    first = resolver.resolve(CARTPOLE_TASK_ID, {"worker.slot_count": "2"})
    second = resolver.resolve(CARTPOLE_TASK_ID, {"worker.slot_count": "2"})

    assert first.worker.environment_id == CARTPOLE_ENVIRONMENT_ID
    assert first.worker.robot_id == CARTPOLE_ROBOT_ID
    assert first.worker.slot_count == 2
    assert isinstance(first.task, CartPoleTaskConfig)
    assert first.runner.max_iterations == 150
    assert "max_iterations" not in first.runner.parameters
    assert first.task is not second.task
    assert first.worker is not second.worker
    assert first.runner is not second.runner

    first_task = registry.create_task(CARTPOLE_TASK_ID)
    second_task = registry.create_task(CARTPOLE_TASK_ID)
    resolved_task = registry.create_task(CARTPOLE_TASK_ID, first.task)
    assert isinstance(first_task, DirectTask)
    assert first_task is not second_task
    assert isinstance(resolved_task, DirectTask)
    assert resolved_task.config is first.task
    assert isinstance(resolved_task.config, CartPoleTaskConfig)
    assert resolved_task.config.action_clip == 1.0
    assert first_task.schema.as_request() == {"state_requirements": [], "action_schema": []}
    print(f"[VERIFY] VC-007: task_id={CARTPOLE_TASK_ID} factory=PASS binding=PASS")
