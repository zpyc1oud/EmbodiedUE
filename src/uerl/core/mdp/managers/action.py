"""Two-phase action manager: process once per control step, re-apply per physics frame.

``process`` evaluates the compiled action plan and caches the physical command.
``applied`` returns that cached batch without re-evaluating the plan — matching
the UE provider contract of re-applying the same command each physics substep
rather than recomputing it.
"""

from __future__ import annotations

import torch

from uerl.core.config.robot import RobotSpec
from uerl.core.direct.types import PhysicalCommandBatch
from uerl.core.mdp.compile_action import compile_action_plan
from uerl.core.mdp.executor import ActionPlanExecutor
from uerl.core.mdp.plan import ActionPlan
from uerl.core.mdp.terms import ActionCfg
from uerl.errors import ConfigError


class ActionManager:
    """Compile action terms once; separate control-step decode from physics apply."""

    def __init__(self, cfg: ActionCfg, spec: RobotSpec) -> None:
        self._plan = compile_action_plan(cfg, spec)
        self._executor = ActionPlanExecutor(self._plan)
        self._raw = torch.zeros(1, self._plan.policy_width)
        self._commands: PhysicalCommandBatch | None = None

    @property
    def plan(self) -> ActionPlan:
        """Return the compiled plan object that export must ship unchanged."""

        return self._plan

    @property
    def policy_width(self) -> int:
        return self._plan.policy_width

    @property
    def previous_action(self) -> torch.Tensor:
        """Raw policy output from the last ``process`` (pre clip/scale/offset)."""

        return self._raw

    def process(self, policy_actions: torch.Tensor) -> PhysicalCommandBatch:
        """Control-step decode: run the plan once and cache the command batch."""

        self._raw = policy_actions.detach().clone()
        self._commands = self._executor.execute(policy_actions)
        return self._commands

    def applied(self) -> PhysicalCommandBatch:
        """Physics-frame re-apply: return the cached command without re-evaluation."""

        if self._commands is None:
            raise ConfigError(
                "ActionManager.applied() called before process()",
                code="CONFIG_OUT_OF_RANGE",
                path="applied",
            )
        return self._commands


__all__ = ["ActionManager"]
