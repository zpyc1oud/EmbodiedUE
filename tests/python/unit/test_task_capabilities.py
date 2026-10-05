"""Verify preflight status distinguishes runnable Tasks from exportable plans."""

from __future__ import annotations

from collections.abc import Mapping

import pytest
import torch

from tests.python.unit.test_cartpole_product_task import CARTPOLE_SHAPES, _robot_spec
from uerl import ConfigError, DirectTask, DirectTaskConfig, PhysicalCommandBatch
from uerl.core.direct.capabilities import CapabilityStatus
from uerl.core.direct.types import StepContext, TerminationResult
from uerl.tasks.cartpole import CartPoleTaskConfig, build_cartpole_composed_cfg, create_cartpole_task


class _PythonTask(DirectTask):
    """A minimal custom Task with Python action/observation/reward functions."""

    def __init__(self) -> None:
        super().__init__(DirectTaskConfig())

    def preprocess_actions(
        self,
        policy_actions: torch.Tensor,
        raw_state: Mapping[str, torch.Tensor],
    ) -> PhysicalCommandBatch:
        del raw_state
        return PhysicalCommandBatch({"test.command": policy_actions})

    def build_observations(
        self,
        raw_state: Mapping[str, torch.Tensor],
        state_valid: torch.Tensor,
        previous_policy_actions: torch.Tensor,
        control_frame_dt: torch.Tensor | None = None,
    ) -> dict[str, torch.Tensor]:
        del state_valid, previous_policy_actions, control_frame_dt
        return {"policy": raw_state["test.state"]}

    def compute_rewards(
        self,
        context: StepContext,
        terminations: TerminationResult,
    ) -> torch.Tensor:
        del context, terminations
        return torch.ones(1)

    def compute_terminations(self, context: StepContext) -> TerminationResult:
        mask = torch.zeros_like(context.episode_steps, dtype=torch.bool)
        return TerminationResult(mask, mask)


class _ManagerTaskWithPythonObservation(DirectTask):
    """Wrap a compiled observation path so equivalence cannot be inferred."""

    def __init__(self) -> None:
        config = CartPoleTaskConfig()
        super().__init__(
            task_config=config,
            cfg_factory=lambda spec: build_cartpole_composed_cfg(config, spec),
        )

    def build_observations(
        self,
        raw_state: Mapping[str, torch.Tensor],
        state_valid: torch.Tensor,
        previous_policy_actions: torch.Tensor,
        control_frame_dt: torch.Tensor | None = None,
    ) -> Mapping[str, torch.Tensor]:
        return super().build_observations(
            raw_state,
            state_valid,
            previous_policy_actions,
            control_frame_dt,
        )


def test_unassembled_manager_task_reports_unknown_export_until_robot_spec_is_bound() -> None:
    task = create_cartpole_task(CartPoleTaskConfig())

    report = task.capabilities

    assert report.train.status is CapabilityStatus.UNKNOWN
    assert report.evaluate.status is CapabilityStatus.UNKNOWN
    assert report.export.status is CapabilityStatus.UNKNOWN
    assert report.export.reason is not None
    assert "RobotSpec" in report.export.reason


def test_python_minimal_task_is_trainable_but_has_no_export_representation() -> None:
    report = _PythonTask().capabilities

    assert report.train.status is CapabilityStatus.SUPPORTED
    assert report.evaluate.status is CapabilityStatus.SUPPORTED
    assert report.export.status is CapabilityStatus.UNSUPPORTED
    assert report.export.reason is not None
    assert "Manager" in report.export.reason
    assert "observation_plan" in report.export.reason


def test_plain_direct_task_is_rejected_before_train_or_evaluate() -> None:
    report = DirectTask().capabilities

    assert report.train.status is CapabilityStatus.UNSUPPORTED
    assert report.evaluate.status is CapabilityStatus.UNSUPPORTED
    assert report.train.reason is not None
    assert "preprocess_actions" in report.train.reason
    assert "compute_terminations or termination_terms" in report.train.reason
    with pytest.raises(ConfigError) as raised:
        report.require("train")
    assert raised.value.code == "TASK_CAPABILITY_UNSUPPORTED"


def test_python_observation_wrapper_keeps_manager_export_capability_unknown() -> None:
    task = _ManagerTaskWithPythonObservation()
    task.bind_robot_spec(_robot_spec(), observation_shapes=CARTPOLE_SHAPES)

    report = task.capabilities

    assert report.train.status is CapabilityStatus.SUPPORTED
    assert report.evaluate.status is CapabilityStatus.SUPPORTED
    assert report.export.status is CapabilityStatus.UNKNOWN
    assert report.export.reason is not None
    assert "build_observations" in report.export.reason
    assert "equivalence" in report.export.reason


def test_capability_report_has_stable_json_and_readable_rendering() -> None:
    report = DirectTask().capabilities

    assert report.to_dict() == {
        "train": {
            "status": "unsupported",
            "reason": report.train.reason,
        },
        "evaluate": {
            "status": "unsupported",
            "reason": report.evaluate.reason,
        },
        "export": {
            "status": "unsupported",
            "reason": report.export.reason,
        },
    }
    assert report.format().startswith("train=unsupported")
    assert "evaluate=unsupported" in report.format()
    assert "export=unsupported" in report.format()
