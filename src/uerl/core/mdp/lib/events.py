"""Event terms: Session-mediated randomization at startup, reset, and intervals."""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Protocol

import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotConfig, RobotSpec


class EventSessionBridge(Protocol):
    """Session-facing sink for event effects (no direct UE/state writes)."""

    def write_robot_reset_values(self, values: torch.Tensor, mask: torch.Tensor) -> None:
        """Stage ``robot.reset.values`` for the next Session reset encode."""

    def write_terrain_levels(self, levels: torch.Tensor, mask: torch.Tensor) -> None:
        """Stage per-Slot terrain levels for the next Session reset encode."""

    def write_ground_friction(
        self,
        static_friction: torch.Tensor,
        dynamic_friction: torch.Tensor,
        mask: torch.Tensor,
    ) -> None:
        """Stage per-Slot static and dynamic ground friction."""

    def write_terrain_tier_params(self, roughness_scale: torch.Tensor, mask: torch.Tensor) -> None:
        """Stage per-Slot normalized terrain roughness controls."""

    def write_root_push(self, velocity_mps: torch.Tensor, mask: torch.Tensor) -> None:
        """Stage per-Slot root velocity changes in the Slot-local frame."""


@dataclass(frozen=True, slots=True)
class EventEffect:
    """One immutable, Session-bound event mutation for selected Slots."""

    kind: str
    values: torch.Tensor
    mask: torch.Tensor


@dataclass
class RecordingEventBridge:
    """Test/production buffer that records Session-mediated event mutations."""

    calls: list[tuple[str, torch.Tensor, torch.Tensor]] = field(default_factory=list)
    effects: list[EventEffect] = field(default_factory=list)
    robot_reset_values: torch.Tensor | None = None
    terrain_levels: torch.Tensor | None = None

    def write_robot_reset_values(self, values: torch.Tensor, mask: torch.Tensor) -> None:
        self.calls.append(("write_robot_reset_values", values.detach().clone(), mask.detach().clone()))
        staged = values.detach().clone()
        if self.robot_reset_values is None:
            self.robot_reset_values = staged
        else:
            self.robot_reset_values = self.robot_reset_values.clone()
            self.robot_reset_values[mask] = staged[mask]

    def write_terrain_levels(self, levels: torch.Tensor, mask: torch.Tensor) -> None:
        self.calls.append(("write_terrain_levels", levels.detach().clone(), mask.detach().clone()))
        staged = levels.detach().to(dtype=torch.int64).clone()
        if self.terrain_levels is None:
            self.terrain_levels = staged.to(dtype=torch.uint16)
            return
        merged = self.terrain_levels.to(dtype=torch.int64).clone()
        merged[mask] = staged[mask]
        self.terrain_levels = merged.to(dtype=torch.uint16)

    def write_ground_friction(
        self,
        static_friction: torch.Tensor,
        dynamic_friction: torch.Tensor,
        mask: torch.Tensor,
    ) -> None:
        """Record one full-batch ground-friction effect."""

        values = torch.stack(
            (static_friction.reshape(-1), dynamic_friction.reshape(-1)), dim=-1
        ).detach().clone()
        self._record_effect("ground_friction", "write_ground_friction", values, mask)

    def write_terrain_tier_params(self, roughness_scale: torch.Tensor, mask: torch.Tensor) -> None:
        """Record one full-batch terrain-tier parameter effect."""

        values = roughness_scale.detach().clone().reshape(-1, 1)
        self._record_effect("terrain_tier_params", "write_terrain_tier_params", values, mask)

    def write_root_push(self, velocity_mps: torch.Tensor, mask: torch.Tensor) -> None:
        """Record one full-batch Slot-local root push effect."""

        values = velocity_mps.detach().clone().reshape(-1, 3)
        self._record_effect("root_push", "write_root_push", values, mask)

    def drain_effects(self) -> tuple[EventEffect, ...]:
        """Return and clear effects waiting for the next Session exchange."""

        effects = tuple(self.effects)
        self.effects.clear()
        return effects

    def _record_effect(
        self,
        kind: str,
        call_name: str,
        values: torch.Tensor,
        mask: torch.Tensor,
    ) -> None:
        """Record a typed effect while preserving ticket30 call evidence."""

        effect_values = values.detach().clone()
        effect_mask = mask.detach().clone().to(dtype=torch.bool).reshape(-1)
        self.calls.append((call_name, effect_values.clone(), effect_mask.clone()))
        self.effects.append(EventEffect(kind, effect_values, effect_mask))


def _uniform_range(
    value_range: tuple[float, float],
    *,
    shape: tuple[int, ...],
    selected: torch.Tensor,
    generator: torch.Generator,
) -> torch.Tensor:
    """Sample a finite inclusive-exclusive uniform range into selected rows."""

    low, high = float(value_range[0]), float(value_range[1])
    if not math.isfinite(low) or not math.isfinite(high) or low > high:
        raise ValueError("event ranges must be finite and satisfy low <= high")
    values = torch.zeros(shape, dtype=torch.float32)
    if selected.numel():
        draws = torch.empty((int(selected.numel()), *shape[1:]), dtype=torch.float32)
        if low == high:
            draws.fill_(low)
        else:
            draws.uniform_(low, high, generator=generator)
        values[selected] = draws
    return values


def randomize_ground_friction(
    bridge: EventSessionBridge,
    mask: torch.Tensor,
    *,
    entity: RobotEntityCfg | None = None,
    static_range: tuple[float, float],
    dynamic_range: tuple[float, float],
    generator: torch.Generator,
) -> None:
    """Sample and stage per-Slot static/dynamic ground friction.

    Each range is sampled independently for selected Slots. Values are carried
    to the Session as ``[static_friction, dynamic_friction]`` rows.
    """

    del entity
    selected = torch.nonzero(mask.to(device="cpu", dtype=torch.bool).reshape(-1), as_tuple=False).flatten()
    static = _uniform_range(
        static_range,
        shape=(int(mask.numel()),),
        selected=selected,
        generator=generator,
    )
    dynamic = _uniform_range(
        dynamic_range,
        shape=(int(mask.numel()),),
        selected=selected,
        generator=generator,
    )
    bridge.write_ground_friction(static, dynamic, mask)


def randomize_terrain_tier_params(
    bridge: EventSessionBridge,
    mask: torch.Tensor,
    *,
    roughness_scale_range: tuple[float, float],
    generator: torch.Generator,
) -> None:
    """Sample and stage one normalized roughness scale per selected Slot."""

    selected = torch.nonzero(mask.to(device="cpu", dtype=torch.bool).reshape(-1), as_tuple=False).flatten()
    values = _uniform_range(
        roughness_scale_range,
        shape=(int(mask.numel()),),
        selected=selected,
        generator=generator,
    )
    bridge.write_terrain_tier_params(values, mask)


def push_root(
    bridge: EventSessionBridge,
    mask: torch.Tensor,
    *,
    entity: RobotEntityCfg | None = None,
    velocity_range_mps: tuple[float, float],
    generator: torch.Generator,
) -> None:
    """Sample a 3-D Slot-local root velocity delta per selected Slot.

    The scalar range applies independently to X, Y, and Z. The Worker applies
    the resulting vector as a velocity change, so no mass-dependent impulse
    interpretation is introduced at the event boundary.
    """

    del entity
    selected = torch.nonzero(mask.to(device="cpu", dtype=torch.bool).reshape(-1), as_tuple=False).flatten()
    values = _uniform_range(
        velocity_range_mps,
        shape=(int(mask.numel()), 3),
        selected=selected,
        generator=generator,
    )
    bridge.write_root_push(values, mask)


def sample_robot_reset_distributions(
    *,
    robot_spec: RobotSpec,
    robot_semantics: RobotConfig,
    run_seed: int,
    episode_index: torch.Tensor,
    reset_mask: torch.Tensor,
) -> torch.Tensor | None:
    """Sample Python-owned Robot reset targets into the fixed wire vector.

    Bit-stable with the pre-EventManager ``UERLSessionAdapter._sample_reset_values``
    path: same ``ResetConfig.sample`` seeds and ``resolve_reset_values`` mapping.
    """

    if not robot_spec.reset:
        return None
    selected_ids = torch.nonzero(
        reset_mask.to(device="cpu", dtype=torch.bool).reshape(-1), as_tuple=False
    ).flatten()
    num_slots = int(reset_mask.numel())
    values = torch.zeros((num_slots, len(robot_spec.reset)), dtype=torch.float32)
    for slot_tensor in selected_ids:
        slot_id = int(slot_tensor)
        episode = int(episode_index[slot_id].item()) + 1
        sampled = robot_semantics.reset.sample(
            seed=run_seed,
            episode_index=episode,
            slot_ids=(slot_id,),
        )
        values[slot_id] = torch.tensor(robot_spec.resolve_reset_values(sampled, 0))
    return values


def sample_robot_reset_event(
    bridge: EventSessionBridge,
    mask: torch.Tensor,
    *,
    robot_spec: RobotSpec,
    robot_semantics: RobotConfig,
    run_seed: int,
    episode_index: torch.Tensor,
    generator: torch.Generator | None = None,
) -> None:
    """Reset-mode term: sample distributions and stage them on the Session bridge."""

    del generator
    values = sample_robot_reset_distributions(
        robot_spec=robot_spec,
        robot_semantics=robot_semantics,
        run_seed=run_seed,
        episode_index=episode_index,
        reset_mask=mask,
    )
    if values is not None:
        bridge.write_robot_reset_values(values, mask)


def randomize_initial_terrain_levels(
    bridge: EventSessionBridge,
    mask: torch.Tensor,
    *,
    num_levels: int,
    generator: torch.Generator,
) -> None:
    """Startup-mode term: sample terrain levels into ``[0, num_levels)`` per Slot."""

    if num_levels < 1:
        raise ValueError("num_levels must be positive")
    num_slots = int(mask.numel())
    levels = torch.zeros(num_slots, dtype=torch.int64)
    selected = torch.nonzero(mask.to(dtype=torch.bool).reshape(-1), as_tuple=False).flatten()
    if selected.numel():
        draws = torch.randint(
            0,
            num_levels,
            (int(selected.numel()),),
            dtype=torch.int64,
            generator=generator,
        )
        levels[selected] = draws
    bridge.write_terrain_levels(levels.to(dtype=torch.uint16), mask)


__all__ = [
    "EventEffect",
    "EventSessionBridge",
    "RecordingEventBridge",
    "push_root",
    "randomize_ground_friction",
    "randomize_initial_terrain_levels",
    "randomize_terrain_tier_params",
    "sample_robot_reset_distributions",
    "sample_robot_reset_event",
]
