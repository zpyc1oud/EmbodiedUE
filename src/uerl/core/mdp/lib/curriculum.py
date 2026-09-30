"""Curriculum terms shared across tasks (terrain level progression)."""

from __future__ import annotations

import math
from collections.abc import Mapping

import torch

from uerl.core.direct.curriculum import CurriculumStep, CurriculumTerm, TerrainCurriculum

_STATE_FIELDS = {
    "levels",
    "previous_xy",
    "elapsed",
    "commanded_distance",
    "progress",
    "rollover_rng_state",
}


class TerrainLevelTerm(CurriculumTerm):
    """Advance terrain levels from command-direction progress (Isaac Lab ``terrain_levels_vel``).

    Wraps :class:`TerrainCurriculum` so the same promote/demote/cap rules live behind
    the curriculum-manager seam. Every valid transition adds the root's
    horizontal displacement projected on the commanded direction, which is the
    body-frame command rotated by the root heading at the end of the
    transition. Log keys under a manager name of ``terrain`` match the
    pre-migration ``Curriculum/terrain/...`` namespace.
    """

    def __init__(
        self,
        *,
        num_levels: int,
        num_envs: int,
        terrain_size_x: float,
        device: str | torch.device = "cpu",
        reachable_promotion_fraction: float = 0.8,
        seed: int = 0,
    ) -> None:
        self._device = torch.device(device)
        self._curriculum = TerrainCurriculum(
            num_levels=num_levels,
            num_envs=num_envs,
            terrain_size_x=terrain_size_x,
            reachable_promotion_fraction=reachable_promotion_fraction,
            seed=seed,
        )
        self._previous_xy = torch.zeros((num_envs, 2), dtype=torch.float32, device=self._device)
        self._elapsed = torch.zeros(num_envs, dtype=torch.float32, device=self._device)
        self._commanded_distance = torch.zeros(num_envs, dtype=torch.float32, device=self._device)
        self._progress = torch.zeros(num_envs, dtype=torch.float32, device=self._device)

    def levels(self) -> torch.Tensor:
        """Return the current terrain level for every Slot as uint16."""

        return torch.tensor(self._curriculum.levels, dtype=torch.uint16, device=self._device)

    def elapsed(self) -> torch.Tensor:
        """Return accumulated transition time for every Slot."""

        return self._elapsed.clone()

    def commanded_distances(self) -> torch.Tensor:
        """Return accumulated ``sum(|command| × transition_dt)`` per Slot."""

        return self._commanded_distance.clone()

    def progress(self) -> torch.Tensor:
        """Return accumulated displacement along the commanded direction per Slot."""

        return self._progress.clone()

    def update(self, step: CurriculumStep) -> Mapping[str, torch.Tensor | float]:
        completed = step.terminated | step.truncated
        completed_episode_level: float | None = None
        if bool(completed.any()):
            completed_levels = self.levels()[completed].to(dtype=torch.float32)
            completed_episode_level = float(completed_levels.mean().item())
        transition_dt = float(step.transition_dt)
        if not math.isfinite(transition_dt) or transition_dt <= 0.0:
            raise ValueError("CurriculumStep transition_dt must be finite and positive")
        command = step.command_velocity
        num_envs = self._curriculum.num_envs
        if not isinstance(command, torch.Tensor) or command.shape != (num_envs, 2):
            raise ValueError("CurriculumStep command_velocity must be a tensor of shape (num_envs, 2)")
        command = command.to(device=self._device, dtype=torch.float32)
        if not bool(torch.isfinite(command).all()):
            raise ValueError("CurriculumStep command_velocity must be finite")
        valid = step.state_valid.to(device=self._device, dtype=torch.bool).reshape(-1)
        speed = torch.linalg.vector_norm(command, dim=1)
        self._elapsed[valid] += transition_dt
        self._commanded_distance[valid] += speed[valid] * transition_dt
        pose = self._body_pose(step.transition_state)
        if pose is not None:
            xy = pose[:, :2]
            moving = speed > 0.0
            direction = torch.zeros_like(command)
            direction[moving] = command[moving] / speed[moving].unsqueeze(1)
            heading = _heading(pose[:, 3:7])
            cos_heading, sin_heading = heading.cos(), heading.sin()
            world_direction = torch.stack(
                (
                    cos_heading * direction[:, 0] - sin_heading * direction[:, 1],
                    sin_heading * direction[:, 0] + cos_heading * direction[:, 1],
                ),
                dim=1,
            )
            advanced = ((xy - self._previous_xy) * world_direction).sum(dim=1)
            self._progress[valid] += advanced[valid]
            self._previous_xy[valid] = xy[valid]
        # The terrain term evaluates every completed episode, including a
        # physical termination, as in Isaac Lab's velocity curriculum.
        eligible = completed & step.state_valid
        if bool(eligible.any()):
            reset_ids = torch.nonzero(eligible, as_tuple=False).flatten().tolist()
            self._curriculum.update(
                reset_ids,
                self._progress[eligible].to(device="cpu").tolist(),
                self._commanded_distance[eligible].to(device="cpu").tolist(),
            )
        metrics: dict[str, torch.Tensor | float] = {
            "next_level": float(self.levels().to(dtype=torch.float32).mean().item()),
        }
        if completed_episode_level is not None:
            metrics["completed_episode_level"] = completed_episode_level
        return metrics

    def reset(
        self,
        reset_mask: torch.Tensor,
        post_reset_state: Mapping[str, torch.Tensor],
    ) -> None:
        pose = self._body_pose(post_reset_state)
        if pose is not None:
            self._previous_xy[reset_mask] = pose[reset_mask, :2]
        self._elapsed[reset_mask] = 0.0
        self._commanded_distance[reset_mask] = 0.0
        self._progress[reset_mask] = 0.0

    def state_dict(self) -> Mapping[str, object]:
        return {
            "levels": list(self._curriculum.levels),
            "previous_xy": self._previous_xy.detach().cpu().tolist(),
            "elapsed": self._elapsed.detach().cpu().tolist(),
            "commanded_distance": self._commanded_distance.detach().cpu().tolist(),
            "progress": self._progress.detach().cpu().tolist(),
            "rollover_rng_state": self._curriculum.rollover_state(),
        }

    def load_state_dict(self, state: Mapping[str, object]) -> None:
        if set(state) != _STATE_FIELDS:
            raise ValueError("TerrainLevelTerm checkpoint fields are invalid")
        num_envs = self._curriculum.num_envs
        levels = state["levels"]
        previous_xy = state["previous_xy"]
        if not isinstance(levels, list) or len(levels) != num_envs:
            raise ValueError("TerrainLevelTerm levels length does not match num_envs")
        if any(type(level) is not int for level in levels):
            raise ValueError("TerrainLevelTerm levels must be ints")
        if any(not 0 <= int(level) < self._curriculum.num_levels for level in levels):
            raise ValueError("TerrainLevelTerm levels are outside the configured range")
        if not isinstance(previous_xy, list) or len(previous_xy) != num_envs:
            raise ValueError("TerrainLevelTerm previous_xy length does not match num_envs")
        self._curriculum.load_levels([int(level) for level in levels])
        rollover_rng_state = state["rollover_rng_state"]
        if not isinstance(rollover_rng_state, tuple):
            raise ValueError("TerrainLevelTerm rollover state is invalid")
        self._curriculum.load_rollover_state(rollover_rng_state)
        self._previous_xy = torch.tensor(previous_xy, dtype=torch.float32, device=self._device)
        elapsed = _finite_values(state["elapsed"], num_envs, non_negative=True)
        if elapsed is None:
            raise ValueError("TerrainLevelTerm elapsed state is invalid")
        commanded_distance = _finite_values(state["commanded_distance"], num_envs, non_negative=True)
        if commanded_distance is None:
            raise ValueError("TerrainLevelTerm commanded distance state is invalid")
        progress = _finite_values(state["progress"], num_envs, non_negative=False)
        if progress is None:
            raise ValueError("TerrainLevelTerm progress state is invalid")
        self._elapsed = torch.tensor(elapsed, dtype=torch.float32, device=self._device)
        self._commanded_distance = torch.tensor(commanded_distance, dtype=torch.float32, device=self._device)
        self._progress = torch.tensor(progress, dtype=torch.float32, device=self._device)

    @staticmethod
    def _body_pose(values: Mapping[str, torch.Tensor]) -> torch.Tensor | None:
        pose_names = [name for name in values if name.endswith(".body_pose")]
        if len(pose_names) != 1:
            return None
        pose = values[pose_names[0]].reshape(values[pose_names[0]].shape[0], -1)
        if pose.shape[1] < 7:
            return None
        return pose


def _heading(quat_xyzw: torch.Tensor) -> torch.Tensor:
    x, y, z, w = quat_xyzw.unbind(dim=1)
    return torch.atan2(2.0 * (w * z + x * y), 1.0 - 2.0 * (y.square() + z.square()))


def _finite_values(value: object, length: int, *, non_negative: bool) -> list[float] | None:
    if not isinstance(value, list) or len(value) != length:
        return None
    if any(
        not isinstance(item, (int, float))
        or not math.isfinite(float(item))
        or (non_negative and float(item) < 0.0)
        for item in value
    ):
        return None
    return [float(item) for item in value]


__all__ = ["TerrainLevelTerm"]
