"""Define PhantomX command progression behind the generic curriculum seam."""

from __future__ import annotations

from collections import deque
from collections.abc import Mapping

import torch

from ...core.direct.curriculum import CurriculumManager, CurriculumStep, CurriculumTerm
from .config import PhantomXCommandConfig, PhantomXCurriculumConfig


class PhantomXCommandCurriculum(CurriculumTerm):
    """Promote new episodes from straight walking to one commanded turn."""

    def __init__(
        self,
        curriculum: PhantomXCurriculumConfig,
        commands: PhantomXCommandConfig,
        *,
        num_envs: int,
        device: str | torch.device,
        run_seed: int,
    ) -> None:
        if num_envs < 1:
            raise ValueError("num_envs must be positive")
        self.config = curriculum
        self.command_config = commands
        self.device = torch.device(device)
        self.run_seed = run_seed
        self._stage = 0
        self._turn_enabled = torch.zeros(num_envs, dtype=torch.bool, device=self.device)
        self._error_integral = torch.zeros(num_envs, dtype=torch.float32, device=self.device)
        self._error_budget_integral = torch.zeros(num_envs, dtype=torch.float32, device=self.device)
        self._yaw_error_integral = torch.zeros(num_envs, dtype=torch.float32, device=self.device)
        self._valid_duration_s = torch.zeros(num_envs, dtype=torch.float32, device=self.device)
        self._straight_history: deque[bool] = deque(maxlen=curriculum.window_episodes)
        self._turn_history: deque[bool] = deque(maxlen=curriculum.window_episodes)

    @property
    def turn_enabled(self) -> torch.Tensor:
        """Whether each Slot's next reset should sample a turn command."""

        return self._turn_enabled

    def update(self, step: CurriculumStep) -> Mapping[str, torch.Tensor | float]:
        """Update rolling episode success and latch promotion at reset boundaries."""

        valid = step.state_valid
        if bool(valid.any()):
            error = step.metrics["phantomx/linear_velocity_error"]
            if not isinstance(error, torch.Tensor):
                raise TypeError("phantomx linear velocity error metric must be a tensor")
            self._error_integral[valid] += (
                error[valid].to(device=self.device, dtype=torch.float32) * step.transition_dt
            )
            speeds = torch.linalg.vector_norm(step.command_velocity[valid], dim=1).to(self.device)
            tolerance = (speeds * self.config.relative_velocity_error_threshold).clamp(
                min=self.config.standing_velocity_error_threshold,
                max=self.config.velocity_error_threshold,
            )
            self._error_budget_integral[valid] += tolerance * step.transition_dt
            self._valid_duration_s[valid] += step.transition_dt
            turning = valid & self._turn_enabled
            if bool(turning.any()):
                yaw_error = step.metrics["phantomx/yaw_rate_error"]
                if not isinstance(yaw_error, torch.Tensor):
                    raise TypeError("phantomx yaw rate error metric must be a tensor")
                self._yaw_error_integral[turning] += (
                    yaw_error[turning].to(device=self.device, dtype=torch.float32) * step.transition_dt
                )

        completed = step.terminated | step.truncated
        for slot_id in torch.nonzero(completed, as_tuple=False).flatten().tolist():
            duration = self._valid_duration_s[slot_id]
            mean_yaw_error = self._yaw_error_integral[slot_id] / duration.clamp_min(torch.finfo(torch.float32).tiny)
            # Only a pure timeout is a successful curriculum episode; a
            # physical failure must not be upgraded by an overlapping timeout.
            succeeded = (
                bool(step.truncated[slot_id])
                and not bool(step.terminated[slot_id])
                and bool(self._valid_duration_s[slot_id] > 0.0)
                and bool(self._error_integral[slot_id] < self._error_budget_integral[slot_id])
                and (
                    not bool(self._turn_enabled[slot_id])
                    or bool(mean_yaw_error < self.config.yaw_rate_error_threshold)
                )
            )
            history = self._turn_history if bool(self._turn_enabled[slot_id]) else self._straight_history
            history.append(succeeded)

        if (
            self._stage == 0
            and len(self._straight_history) >= self.config.minimum_episodes
            and _success_rate(self._straight_history) >= self.config.promotion_success_rate
        ):
            self._stage = 1

        history = self._turn_history if self._stage == 1 else self._straight_history
        projected_turn_enabled = self._turn_enabled.clone()
        projected_turn_enabled[completed] = self._stage == 1
        return {
            "stage": float(self._stage),
            "success_rate": _success_rate(history),
            "completed_episodes": float(len(history)),
            "turn_slot_fraction": float(projected_turn_enabled.float().mean().item()),
        }

    def reset(
        self,
        reset_mask: torch.Tensor,
        post_reset_state: Mapping[str, torch.Tensor],
    ) -> None:
        """Apply the promoted stage to resetting Slots. Sampling belongs to the command item."""

        del post_reset_state
        self._turn_enabled[reset_mask] = self._stage == 1
        self._error_integral[reset_mask] = 0.0
        self._yaw_error_integral[reset_mask] = 0.0
        self._error_budget_integral[reset_mask] = 0.0
        self._valid_duration_s[reset_mask] = 0.0

    def state_dict(self) -> Mapping[str, object]:
        """Persist global stage and rolling promotion evidence."""

        return {
            "stage": self._stage,
            "straight_history": list(self._straight_history),
            "turn_history": list(self._turn_history),
        }

    def load_state_dict(self, state: Mapping[str, object]) -> None:
        """Restore promotion state while the new Session owns fresh episodes."""

        if set(state) != {"stage", "straight_history", "turn_history"}:
            raise ValueError("PhantomX command curriculum checkpoint fields are invalid")
        stage = state["stage"]
        straight_history = state["straight_history"]
        turn_history = state["turn_history"]
        if stage not in (0, 1):
            raise ValueError("PhantomX command curriculum stage must be zero or one")
        if not isinstance(straight_history, list) or not isinstance(turn_history, list):
            raise ValueError("PhantomX command curriculum histories must be lists")
        if len(straight_history) > self.config.window_episodes or len(turn_history) > self.config.window_episodes:
            raise ValueError("PhantomX command curriculum history exceeds its configured window")
        if any(type(item) is not bool for item in (*straight_history, *turn_history)):
            raise ValueError("PhantomX command curriculum history values must be booleans")
        self._stage = int(stage)
        self._straight_history = deque(straight_history, maxlen=self.config.window_episodes)
        self._turn_history = deque(turn_history, maxlen=self.config.window_episodes)
        self._turn_enabled.fill_(self._stage == 1)
        self._error_integral.zero_()
        self._yaw_error_integral.zero_()
        self._error_budget_integral.zero_()
        self._valid_duration_s.zero_()


def create_phantomx_curriculum(
    curriculum: PhantomXCurriculumConfig,
    commands: PhantomXCommandConfig,
    *,
    num_envs: int,
    device: str | torch.device,
    run_seed: int,
) -> CurriculumManager:
    """Build the task's named terms behind the generic manager interface."""

    return CurriculumManager(
        {
            "command": PhantomXCommandCurriculum(
                curriculum,
                commands,
                num_envs=num_envs,
                device=device,
                run_seed=run_seed,
            )
        }
    )


def _success_rate(history: deque[bool]) -> float:
    return sum(history) / len(history) if history else 0.0


__all__ = ["PhantomXCommandCurriculum", "create_phantomx_curriculum"]
