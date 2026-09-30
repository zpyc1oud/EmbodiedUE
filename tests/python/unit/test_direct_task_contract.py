"""Verify the framework-independent DirectTask contract."""

from __future__ import annotations

from collections.abc import Mapping, MutableMapping
from typing import cast

import pytest
import torch

from uerl import (
    DirectTask,
    DirectTaskConfig,
    PhysicalCommandBatch,
    SessionSchema,
    StepContext,
    TerminationResult,
)


class _FakeTask(DirectTask):
    """Exercise the public contract with a minimal batch Task."""

    def __init__(self) -> None:
        super().__init__(
            DirectTaskConfig(
                state_requirements=("robot.position",),
                action_schema=("robot.force",),
                max_episode_steps=8,
                slot_fault_reward=-3.0,
            )
        )

    @property
    def schema(self) -> SessionSchema:
        """Return one scalar state and command descriptor for the contract test."""

        return SessionSchema(
            (
                {
                    "name": "robot.position",
                    "dtype": "float32",
                    "shape": [],
                    "unit": "m",
                    "frame": "slot/world-x",
                    "semantic": "position",
                    "source": "test.robot",
                },
            ),
            (
                {
                    "name": "robot.force",
                    "dtype": "float32",
                    "shape": [],
                    "unit": "N",
                    "frame": "slot/world-x",
                    "semantic": "force",
                    "source": "test.robot",
                },
            ),
        )

    @property
    def observation_groups(self) -> Mapping[str, tuple[str, ...]]:
        """Expose the named state field through the policy group."""

        return {"policy": ("robot.position",)}

    def preprocess_actions(
        self,
        policy_actions: torch.Tensor,
        raw_state: Mapping[str, torch.Tensor],
    ) -> PhysicalCommandBatch:
        """Combine policy actions with the named state batch for a fake command."""

        return PhysicalCommandBatch({"robot.force": policy_actions + raw_state["robot.position"]})

    def build_observations(
        self,
        raw_state: Mapping[str, torch.Tensor],
        state_valid: torch.Tensor,
        previous_policy_actions: torch.Tensor,
        control_frame_dt: torch.Tensor | None = None,
    ) -> Mapping[str, torch.Tensor]:
        """Clone the named state field into the policy observation group."""

        del state_valid, previous_policy_actions, control_frame_dt
        return {"policy": raw_state["robot.position"].clone()}

    def compute_terminations(self, context: StepContext) -> TerminationResult:
        """Terminate compact rows whose position exceeds the contract threshold."""

        terminated = context.transition_state["robot.position"].abs().flatten() > 1.0
        return TerminationResult(terminated, torch.zeros_like(terminated))

    def compute_rewards(
        self,
        context: StepContext,
        terminations: TerminationResult,
    ) -> torch.Tensor:
        """Return the pre-reset position as the deterministic test reward."""

        return context.transition_state["robot.position"].flatten()


def test_ac001_vc001_direct_task_exposes_named_batch_contract() -> None:
    """Verify AC-001 / VC-001: Task methods operate on named batched tensors."""

    task = _FakeTask()
    state = {"robot.position": torch.tensor([[0.0], [0.5]])}
    valid = torch.ones(2, dtype=torch.bool)
    actions = torch.tensor([[1.0], [2.0]])
    command = task.preprocess_actions(actions, state)
    context = StepContext(
        raw_state=state,
        previous_policy_actions=torch.zeros_like(actions),
        policy_actions=actions,
        physical_command=command,
        transition_state={"robot.position": torch.tensor([[0.2], [1.2]])},
        state_valid=valid,
        slot_fault_code=torch.zeros(2, dtype=torch.uint16),
        episode_steps=torch.ones(2, dtype=torch.long),
    )
    terminations = task.compute_terminations(context)
    rewards = task.compute_rewards(context, terminations)
    observations = task.build_observations(context.transition_state, valid, actions)

    assert task.state_requirements == ("robot.position",)
    assert task.action_schema == ("robot.force",)
    state_descriptor = task.schema.as_request()["state_requirements"][0]
    assert state_descriptor["name"] == "robot.position"
    assert {"unit", "frame", "semantic", "source"} <= set(state_descriptor)
    assert command["robot.force"].shape == (2, 1)
    assert task.max_episode_steps == 8
    assert task.slot_fault_reward == -3.0
    assert torch.equal(terminations.terminated, torch.tensor([False, True]))
    assert torch.equal(rewards, torch.tensor([0.2, 1.2]))
    assert observations["policy"].shape == (2, 1)
    print("[VERIFY] VC-001: task_contract=batch_only product_imports=direct")


def test_ac001_named_batch_mapping_is_not_replaceable() -> None:
    """Verify AC-001: DTO field maps are fixed after crossing the typed seam."""

    batch = PhysicalCommandBatch({"robot.force": torch.zeros(1, 1)})
    values = cast(MutableMapping[str, torch.Tensor], batch.values)

    with pytest.raises(TypeError):
        values["new.field"] = torch.ones(1, 1)


def test_ac001_schema_metadata_is_immutable() -> None:
    """Verify AC-001 / VC-001: schema metadata stays frozen after construction."""

    shape = [1]
    descriptor = {"name": "robot.position", "shape": shape}
    schema = SessionSchema((descriptor,), ())
    descriptor["name"] = "changed"
    shape.append(2)

    request = schema.as_request()
    assert request["state_requirements"][0] == {"name": "robot.position", "shape": [1]}
