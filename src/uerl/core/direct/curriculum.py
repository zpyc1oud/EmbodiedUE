"""Manage Python-owned curricula independently of Session I/O."""

from __future__ import annotations

import math
import random
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import TYPE_CHECKING

import torch

if TYPE_CHECKING:
    from uerl.core.mdp.terms import CurriculumCfg


@dataclass(frozen=True, slots=True)
class CurriculumStep:
    """Describe one completed environment transition to curriculum terms.

    ``command_velocity`` is the planar body-frame velocity command ``(x, y)``
    of the action that just completed, zero for Tasks that publish none. It is
    kept separate from the Task's next observation command so interval events
    cannot change this transition's time integral.
    """

    transition_state: Mapping[str, torch.Tensor]
    metrics: Mapping[str, torch.Tensor | float]
    state_valid: torch.Tensor
    terminated: torch.Tensor
    truncated: torch.Tensor
    episode_lengths: torch.Tensor
    transition_dt: float
    command_velocity: torch.Tensor


class CurriculumTerm:
    """Implement one named curriculum behind the manager seam.

    Terms may adapt task-visible state and update their own stage only from the
    transition context supplied by ``CurriculumManager``. They do not own a
    Session or perform resets.
    """

    def transform_state(self, values: Mapping[str, torch.Tensor]) -> Mapping[str, torch.Tensor]:
        """Return task-visible state for the term's current stage."""

        return values

    def update(self, step: CurriculumStep) -> Mapping[str, torch.Tensor | float]:
        """Consume one transition and return term-local logging values."""

        del step
        return {}

    def reset(
        self,
        reset_mask: torch.Tensor,
        post_reset_state: Mapping[str, torch.Tensor],
    ) -> None:
        """Apply the current stage after selected episodes have reset."""

        del reset_mask, post_reset_state

    def state_dict(self) -> Mapping[str, object]:
        """Return checkpoint-safe term state."""

        return {}

    def load_state_dict(self, state: Mapping[str, object]) -> None:
        """Restore checkpoint-safe term state."""

        if state:
            raise ValueError(f"{type(self).__name__} does not define checkpoint state")


class CurriculumManager:
    """Orchestrate named curriculum terms through one task-neutral interface."""

    def __init__(self, terms: Mapping[str, CurriculumTerm] | CurriculumCfg) -> None:
        from uerl.core.mdp.terms import CurriculumCfg as _CurriculumCfg

        if isinstance(terms, _CurriculumCfg):
            built = {
                name: term_cfg.term_class(**dict(term_cfg.params))
                for name, term_cfg in terms.terms.items()
            }
        else:
            built = dict(terms)
        if not built or any(not name for name in built):
            raise ValueError("curriculum terms require non-empty names")
        for name, term in built.items():
            if not isinstance(term, CurriculumTerm):
                raise TypeError(f"curriculum term {name!r} must be a CurriculumTerm")
        self._terms = built

    @property
    def terms(self) -> Mapping[str, CurriculumTerm]:
        """Return the registered terms (read-only view)."""

        return MappingProxyType(self._terms)

    def terrain_levels(self) -> torch.Tensor | None:
        """Return levels from a registered ``TerrainLevelTerm``, if any."""

        from uerl.core.mdp.lib.curriculum import TerrainLevelTerm

        for term in self._terms.values():
            if isinstance(term, TerrainLevelTerm):
                return term.levels()
        return None

    def transform_state(self, values: Mapping[str, torch.Tensor]) -> Mapping[str, torch.Tensor]:
        """Apply term state transforms in registration order."""

        transformed = values
        for term in self._terms.values():
            transformed = term.transform_state(transformed)
        return transformed

    def update(self, step: CurriculumStep) -> dict[str, torch.Tensor | float]:
        """Update every term and namespace its logging values."""

        metrics: dict[str, torch.Tensor | float] = {}
        for name, term in self._terms.items():
            for metric_name, value in term.update(step).items():
                metrics[f"Curriculum/{name}/{metric_name}"] = value
        return metrics

    def reset(
        self,
        reset_mask: torch.Tensor,
        post_reset_state: Mapping[str, torch.Tensor],
    ) -> None:
        """Notify all terms after selected Slots enter post-reset state."""

        for term in self._terms.values():
            term.reset(reset_mask, post_reset_state)

    def state_dict(self) -> dict[str, dict[str, object]]:
        """Return all named term states for checkpointing."""

        return {name: dict(term.state_dict()) for name, term in self._terms.items()}

    def load_state_dict(self, state: Mapping[str, object]) -> None:
        """Restore every registered term from a checkpoint mapping."""

        if set(state) != set(self._terms):
            raise ValueError("checkpoint curriculum terms do not match the registered terms")
        for name, term in self._terms.items():
            term_state = state[name]
            if not isinstance(term_state, Mapping):
                raise ValueError(f"checkpoint curriculum term {name!r} must be a mapping")
            term.load_state_dict(term_state)


@dataclass(slots=True)
class TerrainCurriculum:
    """Advance terrain levels from each episode's progress along its command.

    Progress integrates the walked displacement projected on the commanded
    direction, so a turning command path counts the same as a straight one;
    straight-line displacement would fall short of the path length on turns.
    A Slot moves up when its progress exceeds half the terrain width or, when
    that is shorter, 80 percent of the commanded path length. A Slot moves
    down when its progress is below half of the commanded path length, unless
    it already satisfies the promotion condition.
    """

    num_levels: int
    num_envs: int
    terrain_size_x: float = math.inf
    reachable_promotion_fraction: float = 0.8
    seed: int = 0
    _levels: list[int] = field(init=False, repr=False)
    _rollover_rng: random.Random = field(init=False, repr=False)

    def __post_init__(self) -> None:
        """Validate curriculum dimensions and initialize every Slot at level zero."""

        if self.num_levels < 1:
            raise ValueError("num_levels must be positive")
        if self.num_envs < 1:
            raise ValueError("num_envs must be positive")
        if not math.isfinite(self.terrain_size_x) and self.terrain_size_x != math.inf:
            raise ValueError("terrain_size_x must be finite or positive infinity")
        if self.terrain_size_x <= 0.0:
            raise ValueError("terrain_size_x must be positive")
        if not math.isfinite(self.reachable_promotion_fraction) or not 0.5 < self.reachable_promotion_fraction <= 1.0:
            raise ValueError("reachable_promotion_fraction must be in (0.5, 1]")
        self._levels = [0] * self.num_envs
        self._rollover_rng = random.Random(self.seed)

    @property
    def levels(self) -> tuple[int, ...]:
        """Return the current terrain level for every stable Slot."""

        return tuple(self._levels)

    def load_levels(self, levels: Sequence[int]) -> None:
        """Replace per-Slot levels from a checkpoint (validated)."""

        if len(levels) != self.num_envs:
            raise ValueError("terrain curriculum levels length does not match num_envs")
        loaded: list[int] = []
        for level in levels:
            if type(level) is not int:
                raise ValueError("terrain curriculum levels must be ints")
            if not 0 <= level < self.num_levels:
                raise ValueError("terrain curriculum levels are outside the configured range")
            loaded.append(level)
        self._levels = loaded

    def rollover_state(self) -> tuple[object, ...]:
        """Return the independent highest-level sampling stream for checkpoints."""

        return self._rollover_rng.getstate()

    def load_rollover_state(self, state: tuple[object, ...]) -> None:
        """Continue highest-level sampling from a checkpoint."""

        self._rollover_rng.setstate(state)

    def update(
        self,
        slot_ids: Sequence[int],
        progress: Sequence[float],
        commanded_distances: Sequence[float],
    ) -> tuple[int, ...]:
        """Apply per-Slot promotion/demotion before the next reset.

        ``progress`` is the horizontal displacement each Slot walked along its
        commanded direction; walking against the command makes it negative.
        ``commanded_distances`` integrate command speed over completed
        physical transition time for that Slot.
        """

        if not len(slot_ids) == len(progress) == len(commanded_distances):
            raise ValueError("terrain curriculum inputs must have equal lengths")
        for slot_id, advanced, commanded_distance in zip(
            slot_ids, progress, commanded_distances, strict=True
        ):
            if not 0 <= slot_id < self.num_envs:
                raise ValueError(f"invalid curriculum Slot {slot_id}")
            if not math.isfinite(advanced) or not math.isfinite(commanded_distance):
                raise ValueError("terrain curriculum distances must be finite")
            if commanded_distance < 0.0:
                raise ValueError("terrain curriculum commanded distance must be non-negative")
            promote_distance = self.terrain_size_x / 2.0
            if commanded_distance > 0.0:
                promote_distance = min(
                    promote_distance,
                    commanded_distance * self.reachable_promotion_fraction,
                )
            move_up = advanced > promote_distance
            move_down = advanced < commanded_distance * 0.5 and not move_up
            if move_up:
                if self._levels[slot_id] == self.num_levels - 1:
                    self._levels[slot_id] = self._rollover_rng.randrange(self.num_levels)
                else:
                    self._levels[slot_id] += 1
            elif move_down:
                self._levels[slot_id] = max(0, self._levels[slot_id] - 1)
        return self.levels


__all__ = [
    "CurriculumManager",
    "CurriculumStep",
    "CurriculumTerm",
    "TerrainCurriculum",
]
