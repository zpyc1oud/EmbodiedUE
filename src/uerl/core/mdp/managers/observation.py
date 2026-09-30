"""Observation manager: compile term declarations once; evaluate the shared plan.

``manager.plan`` is the object export must ship — training and deploy share the
same ``ObservationPlan`` instance (object identity, not a rebuild).
"""

from __future__ import annotations

from collections.abc import Mapping

import torch

from uerl.core.config.robot import RobotSpec
from uerl.core.direct.robot_observation import ObservationShapeTable
from uerl.core.mdp.compile_observation import compile_observation_plan
from uerl.core.mdp.executor import PlanExecutor, PlanInputs
from uerl.core.mdp.plan import ObservationPlan
from uerl.core.mdp.terms import ObservationCfg


class ObservationManager:
    """Compile observation terms into a plan and evaluate it each control step."""

    def __init__(
        self,
        cfg: ObservationCfg,
        spec: RobotSpec,
        *,
        policy_width: int,
        observation_shapes: ObservationShapeTable,
        command_channels: Mapping[str, int] | None = None,
    ) -> None:
        self._plan = compile_observation_plan(
            cfg,
            spec,
            policy_width=policy_width,
            observation_shapes=observation_shapes,
        )
        self._executor = PlanExecutor(self._plan, command_channels=command_channels)

    @property
    def plan(self) -> ObservationPlan:
        """Return the compiled plan object that export must ship unchanged."""

        return self._plan

    @property
    def group_widths(self) -> Mapping[str, int]:
        return self._plan.group_widths

    def compute(self, inputs: PlanInputs) -> Mapping[str, torch.Tensor]:
        return self._executor.execute(inputs)

    def reset(self, mask: torch.Tensor | None = None) -> None:
        """No-op: observation history was cancelled (ticket 10); kept for API stability."""

        del mask


__all__ = ["ObservationManager"]
