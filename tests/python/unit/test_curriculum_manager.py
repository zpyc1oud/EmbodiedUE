"""Verify the task-neutral curriculum manager seam."""

from __future__ import annotations

from collections.abc import Mapping
from typing import cast

import pytest
import torch

from uerl import CurriculumManager, CurriculumStep, CurriculumTerm
from uerl.core.mdp.executor import PlanExecutor
from uerl.core.mdp.plan import ObservationPlan, PlanOp
from uerl.core.mdp.terms import CurriculumCfg, CurrTermCfg
from uerl.errors import ConfigError


class _ScaleTerm(CurriculumTerm):
    def __init__(self, scale: float) -> None:
        self.scale = scale
        self.updates = 0
        self.reset_masks: list[torch.Tensor] = []

    def transform_state(self, values: Mapping[str, torch.Tensor]) -> Mapping[str, torch.Tensor]:
        return {**values, "state.value": values["state.value"] * self.scale}

    def update(self, step: CurriculumStep) -> Mapping[str, torch.Tensor | float]:
        self.updates += 1
        return {"updates": float(self.updates), "done": step.truncated.float().mean()}

    def reset(
        self,
        reset_mask: torch.Tensor,
        post_reset_state: Mapping[str, torch.Tensor],
    ) -> None:
        del post_reset_state
        self.reset_masks.append(reset_mask.clone())

    def state_dict(self) -> Mapping[str, object]:
        return {"updates": self.updates}

    def load_state_dict(self, state: Mapping[str, object]) -> None:
        self.updates = int(cast(int, state["updates"]))


def _step() -> CurriculumStep:
    return CurriculumStep(
        transition_state={"state.value": torch.ones(2, 1)},
        metrics={},
        state_valid=torch.ones(2, dtype=torch.bool),
        terminated=torch.tensor([False, True]),
        truncated=torch.tensor([True, False]),
        episode_lengths=torch.tensor([10, 4]),
        transition_dt=0.02,
        command_velocity=torch.zeros(2, 2),
    )


def test_manager_composes_named_terms_and_namespaces_metrics() -> None:
    first = _ScaleTerm(2.0)
    second = _ScaleTerm(3.0)
    manager = CurriculumManager({"first": first, "second": second})

    transformed = manager.transform_state({"state.value": torch.ones(2, 1)})
    metrics = manager.update(_step())
    manager.reset(torch.tensor([True, False]), {"state.value": torch.ones(2, 1)})

    assert torch.equal(transformed["state.value"], torch.full((2, 1), 6.0))
    assert set(metrics) == {
        "Curriculum/first/updates",
        "Curriculum/first/done",
        "Curriculum/second/updates",
        "Curriculum/second/done",
    }
    assert torch.equal(first.reset_masks[0], torch.tensor([True, False]))
    assert torch.equal(second.reset_masks[0], torch.tensor([True, False]))


def test_manager_checkpoint_restores_each_registered_term() -> None:
    source = CurriculumManager({"command": _ScaleTerm(2.0)})
    source.update(_step())
    checkpoint = source.state_dict()
    restored_term = _ScaleTerm(2.0)
    restored = CurriculumManager({"command": restored_term})

    restored.load_state_dict(checkpoint)

    assert restored_term.updates == 1
    with pytest.raises(ValueError, match="do not match"):
        restored.load_state_dict({"another": {}})


def test_curriculum_does_not_publish_command_channels() -> None:
    """Command channels are not gathered from curriculum terms."""

    class _CommandTerm(CurriculumTerm):
        def channels(self) -> Mapping[str, int]:
            return {"cmd.a": 2}

        def current(self) -> Mapping[str, torch.Tensor]:
            return {"cmd.a": torch.zeros(2, 2)}

    manager = CurriculumManager({"command": _CommandTerm()})
    transformed = manager.transform_state({"state.value": torch.ones(2, 1)})

    assert set(transformed) == {"state.value"}
    assert not hasattr(manager, "command_source")

    plan = ObservationPlan(
        state_requirements=(),
        ops=(PlanOp("command", (), "a", 3, {"channel": "cmd.a", "width": 3}),),
        groups={"policy": ("a",)},
        group_widths={"policy": 3},
    )
    with pytest.raises(ConfigError) as exc_info:
        PlanExecutor(plan, command_channels={"cmd.a": 2})
    assert exc_info.value.code == "OP_WIDTH"


def test_manager_accepts_curriculum_cfg() -> None:
    manager = CurriculumManager(
        CurriculumCfg(terms={"scale": CurrTermCfg(term_class=_ScaleTerm, params={"scale": 2.0})})
    )
    transformed = manager.transform_state({"state.value": torch.ones(2, 1)})
    assert torch.equal(transformed["state.value"], torch.full((2, 1), 2.0))
