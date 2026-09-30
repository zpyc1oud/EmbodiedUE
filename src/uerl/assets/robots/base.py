"""Declarative building blocks for Python-owned Robot asset configurations."""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field, replace
from enum import StrEnum
from types import MappingProxyType
from typing import Any, Literal

from ...core.config.entity import match_names
from ...core.config.robot import (
    ActuatorConfig,
    ObservationConfig,
    ObsType,
    ResetConfig,
    ResetTarget,
    ResetTargetType,
    RobotConfig,
)
from ...errors import ConfigError

NameSelector = str | tuple[str, ...]
ResetBound = float | tuple[float, ...]


class _SelectorKind(StrEnum):
    JOINT = "joint"
    BODY = "body"


@dataclass(frozen=True, slots=True)
class RobotInitStateCfg:
    """Declare the calibrated initial pose before topology binding.

    ``joint_pos`` is an ordered regex mapping. If multiple patterns match one
    joint, the later pattern wins. Every pattern must match at least one
    declared joint and every declared joint must receive a value.
    """

    root_height_m: float | None = None
    joint_pos: Mapping[str, float] = field(default_factory=dict)

    def __post_init__(self) -> None:
        if self.root_height_m is not None and (
            isinstance(self.root_height_m, bool)
            or not math.isfinite(self.root_height_m)
            or self.root_height_m <= 0.0
        ):
            raise ConfigError(
                "root_height_m must be a positive finite number",
                code="CONFIG_OUT_OF_RANGE",
                path="init_state.root_height_m",
            )
        values = dict(self.joint_pos)
        for pattern, value in values.items():
            if not isinstance(pattern, str) or not pattern:
                raise ConfigError(
                    "joint position patterns must be non-empty strings",
                    code="CONFIG_OUT_OF_RANGE",
                    path="init_state.joint_pos",
                )
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                raise ConfigError(
                    "joint position defaults must be finite numbers",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"init_state.joint_pos[{pattern!r}]",
                )
        object.__setattr__(
            self,
            "joint_pos",
            MappingProxyType({pattern: float(value) for pattern, value in values.items()}),
        )

    def expand_joint_positions(self, joint_names: Sequence[str]) -> Mapping[str, float]:
        """Expand ordered regex defaults to one value for every joint."""

        names = tuple(joint_names)
        values_by_name: dict[str, list[float]] = {name: [] for name in names}
        for pattern, value in self.joint_pos.items():
            hits = match_names(
                pattern,
                names,
                preserve_order=False,
                path=f"init_state.joint_pos[{pattern!r}]",
            )
            for index in hits:
                values_by_name[names[index]].append(value)
        missing = tuple(name for name, values in values_by_name.items() if not values)
        if missing:
            raise ConfigError(
                f"no default position pattern matched joints: {', '.join(missing)}",
                code="CONFIG_OUT_OF_RANGE",
                path="init_state.joint_pos",
            )
        return MappingProxyType({name: values[-1] for name, values in values_by_name.items()})


@dataclass(frozen=True, slots=True)
class ActuatorGroupCfg:
    """Declare one actuator group selected by joint names or regex."""

    joint_names: NameSelector
    target_mode: Literal["position", "effort"]
    stiffness: float
    damping: float
    effort_limit: float
    action_scale: float

    def __post_init__(self) -> None:
        if isinstance(self.joint_names, str):
            if not self.joint_names:
                raise ConfigError(
                    "actuator joint selector must be non-empty",
                    code="CONFIG_OUT_OF_RANGE",
                    path="actuators.joint_names",
                )
        elif not self.joint_names:
            raise ConfigError(
                "actuator joint selector must be non-empty",
                code="CONFIG_OUT_OF_RANGE",
                path="actuators.joint_names",
            )
        for field_name, value in (
            ("stiffness", self.stiffness),
            ("damping", self.damping),
            ("effort_limit", self.effort_limit),
            ("action_scale", self.action_scale),
        ):
            if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
                raise ConfigError(
                    f"actuator {field_name} must be finite",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"actuators.{field_name}",
                )
        if self.stiffness < 0.0 or self.damping < 0.0:
            raise ConfigError(
                "actuator stiffness and damping must be non-negative",
                code="CONFIG_OUT_OF_RANGE",
                path="actuators",
            )
        if self.effort_limit <= 0.0 or self.action_scale <= 0.0:
            raise ConfigError(
                "actuator effort_limit and action_scale must be positive",
                code="CONFIG_OUT_OF_RANGE",
                path="actuators",
            )
        inferred_mode = "position" if self.stiffness > 0.0 else "effort"
        if self.target_mode != inferred_mode:
            raise ConfigError(
                f"target_mode {self.target_mode!r} does not match actuator gains {inferred_mode!r}",
                code="CONFIG_OUT_OF_RANGE",
                path="actuators.target_mode",
            )
        object.__setattr__(self, "stiffness", float(self.stiffness))
        object.__setattr__(self, "damping", float(self.damping))
        object.__setattr__(self, "effort_limit", float(self.effort_limit))
        object.__setattr__(self, "action_scale", float(self.action_scale))


@dataclass(frozen=True, slots=True)
class ObsSelectorCfg:
    """Declare one observation quantity and its target selector."""

    type: ObsType
    joint_names: NameSelector | None = None
    body_names: NameSelector | None = None
    preserve_order: bool = False


@dataclass(frozen=True, slots=True)
class ResetTargetCfg:
    """Declare one reset distribution expanded over selected joints."""

    target_type: ResetTargetType
    joint_names: NameSelector | None = None
    distribution_type: Literal["constant", "uniform"] = "constant"
    lower: ResetBound = 0.0
    upper: ResetBound = 0.0
    stream_id: str = ""
    preserve_order: bool = False

    def __post_init__(self) -> None:
        if not self.stream_id:
            raise ConfigError(
                "reset stream_id must be non-empty",
                code="CONFIG_OUT_OF_RANGE",
                path="reset.stream_id",
            )
        lower = self._normalise_bound(self.lower, "lower")
        upper = self._normalise_bound(self.upper, "upper")
        if len(lower) != len(upper) or any(
            lower_value > upper_value for lower_value, upper_value in zip(lower, upper, strict=True)
        ):
            raise ConfigError(
                "reset bounds are invalid",
                code="CONFIG_OUT_OF_RANGE",
                path="reset",
            )
        object.__setattr__(self, "lower", lower[0] if len(lower) == 1 else lower)
        object.__setattr__(self, "upper", upper[0] if len(upper) == 1 else upper)

    @staticmethod
    def _normalise_bound(value: ResetBound, name: str) -> tuple[float, ...]:
        values = (value,) if isinstance(value, (int, float)) and not isinstance(value, bool) else value
        if not isinstance(values, tuple) or not values:
            raise ConfigError(
                f"reset {name} must be a finite scalar or non-empty tuple",
                code="CONFIG_OUT_OF_RANGE",
                path=f"reset.{name}",
            )
        if not all(
            isinstance(item, (int, float)) and not isinstance(item, bool) and math.isfinite(item)
            for item in values
        ):
            raise ConfigError(
                f"reset {name} must contain finite numbers",
                code="CONFIG_OUT_OF_RANGE",
                path=f"reset.{name}",
            )
        return tuple(float(item) for item in values)

    @property
    def bounds(self) -> tuple[tuple[float, float], ...]:
        """Return one scalar lower/upper pair per reset component."""

        lower = self.lower if isinstance(self.lower, tuple) else (self.lower,)
        upper = self.upper if isinstance(self.upper, tuple) else (self.upper,)
        return tuple(zip(lower, upper, strict=True))


@dataclass(frozen=True, slots=True)
class RobotAssetCfg:
    """Declare a reusable Robot asset and materialise its runtime semantics."""

    name: str
    asset_path: str
    joint_names: tuple[str, ...]
    body_names: tuple[str, ...]
    init_state: RobotInitStateCfg
    actuators: tuple[ActuatorGroupCfg, ...]
    observations: tuple[ObsSelectorCfg, ...]
    reset: tuple[ResetTargetCfg, ...]

    def __post_init__(self) -> None:
        if not self.name or not self.asset_path:
            raise ConfigError(
                "robot asset name and asset_path must be non-empty",
                code="CONFIG_OUT_OF_RANGE",
                path="robot_asset",
            )
        for field_name in ("joint_names", "body_names"):
            values = tuple(getattr(self, field_name))
            if not values or any(not isinstance(value, str) or not value for value in values):
                raise ConfigError(
                    f"{field_name} must contain non-empty strings",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"robot_asset.{field_name}",
                )
            if len(set(values)) != len(values):
                raise ConfigError(
                    f"{field_name} must contain unique names",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"robot_asset.{field_name}",
                )
            object.__setattr__(self, field_name, values)
        for field_name in ("actuators", "observations", "reset"):
            object.__setattr__(self, field_name, tuple(getattr(self, field_name)))

    def to_robot_config(self) -> RobotConfig:
        """Expand selectors into the concrete immutable RobotConfig contract."""

        joint_names = self.joint_names
        body_names = self.body_names
        default_positions = self.init_state.expand_joint_positions(joint_names)

        actuators: list[ActuatorConfig] = []
        assigned_joints: set[int] = set()
        for group_index, group in enumerate(self.actuators):
            joint_ids = match_names(
                group.joint_names,
                joint_names,
                preserve_order=False,
                path=f"actuators[{group_index}].joint_names",
            )
            for joint_id in joint_ids:
                if joint_id in assigned_joints:
                    raise ConfigError(
                        f"joint '{joint_names[joint_id]}' is selected by more than one actuator group",
                        code="DUPLICATE_ACTUATOR",
                        path=f"actuators[{group_index}].joint_names",
                    )
                assigned_joints.add(joint_id)
                joint = joint_names[joint_id]
                actuators.append(
                    ActuatorConfig(
                        joint=joint,
                        stiffness=group.stiffness,
                        damping=group.damping,
                        effort_limit=group.effort_limit,
                        default_pos=default_positions[joint],
                        action_scale=group.action_scale,
                    )
                )
        if not actuators:
            raise ConfigError(
                "robot asset must declare at least one actuator",
                code="CONFIG_OUT_OF_RANGE",
                path="actuators",
            )

        observations: list[ObservationConfig] = []
        for observation_index, selector in enumerate(self.observations):
            selector_kind, selected_names = self._resolve_observation_selector(
                selector,
                joint_names,
                body_names,
                observation_index,
            )
            for target_name in selected_names:
                observations.append(ObservationConfig(selector.type, target_name, selector_kind.value))
        if not observations:
            raise ConfigError(
                "robot asset must declare at least one observation",
                code="CONFIG_OUT_OF_RANGE",
                path="observations",
            )

        distributions: list[ResetTarget] = []
        seen_reset_keys: set[str] = set()
        for reset_index, reset_target_cfg in enumerate(self.reset):
            selected_joints: tuple[str | None, ...]
            if reset_target_cfg.joint_names is None:
                if reset_target_cfg.target_type in {ResetTargetType.JOINT_POSITION, ResetTargetType.JOINT_VELOCITY}:
                    raise ConfigError(
                        "joint reset target requires a joint selector",
                        code="CONFIG_OUT_OF_RANGE",
                        path=f"reset[{reset_index}].joint_names",
                    )
                selected_joints = (None,)
            else:
                if reset_target_cfg.target_type not in {
                    ResetTargetType.JOINT_POSITION,
                    ResetTargetType.JOINT_VELOCITY,
                }:
                    raise ConfigError(
                        "root reset target cannot contain a joint selector",
                        code="CONFIG_OUT_OF_RANGE",
                        path=f"reset[{reset_index}].joint_names",
                    )
                joint_ids = match_names(
                    reset_target_cfg.joint_names,
                    joint_names,
                    preserve_order=reset_target_cfg.preserve_order,
                    path=f"reset[{reset_index}].joint_names",
                )
                selected_joints = tuple(joint_names[index] for index in joint_ids)

            bounds = reset_target_cfg.bounds
            if selected_joints != (None,) and len(bounds) != 1:
                raise ConfigError(
                    "joint reset targets must contain one scalar",
                    code="RESET_TARGET_DIMENSION_MISMATCH",
                    path=f"reset[{reset_index}]",
                )
            for reset_joint in selected_joints:
                stream_id = (
                    reset_target_cfg.stream_id.format(joint=reset_joint)
                    if reset_joint is not None
                    else reset_target_cfg.stream_id
                )
                reset_target = ResetTarget(
                    target_type=reset_target_cfg.target_type,
                    joint=reset_joint,
                    distribution_type=reset_target_cfg.distribution_type,
                    lower=bounds[0][0],
                    upper=bounds[0][1],
                    stream_id=stream_id,
                    lower_values=tuple(lower for lower, _ in bounds) if len(bounds) > 1 else (),
                    upper_values=tuple(upper for _, upper in bounds) if len(bounds) > 1 else (),
                )
                if reset_target.wire_key in seen_reset_keys:
                    raise ConfigError(
                        "reset target is declared more than once",
                        code="DUPLICATE_RESET_TARGET",
                        path=f"reset[{reset_index}]",
                    )
                seen_reset_keys.add(reset_target.wire_key)
                distributions.append(reset_target)
        if not distributions:
            raise ConfigError(
                "robot asset must declare at least one reset distribution",
                code="CONFIG_OUT_OF_RANGE",
                path="reset",
            )

        return RobotConfig(
            actuators=tuple(actuators),
            observations=tuple(observations),
            reset=ResetConfig(tuple(distributions)),
        )

    @staticmethod
    def _resolve_observation_selector(
        selector: ObsSelectorCfg,
        joint_names: Sequence[str],
        body_names: Sequence[str],
        observation_index: int,
    ) -> tuple[_SelectorKind, tuple[str, ...]]:
        joint_observation = selector.type in {ObsType.JOINT_POSITION, ObsType.JOINT_VELOCITY}
        if (selector.joint_names is None) == (selector.body_names is None):
            raise ConfigError(
                "observation must select exactly one of joint_names or body_names",
                code="CONFIG_OUT_OF_RANGE",
                path=f"observations[{observation_index}]",
            )
        if joint_observation and selector.joint_names is None:
            raise ConfigError(
                "joint observation type requires joint_names",
                code="OBSERVATION_TARGET_MISMATCH",
                path=f"observations[{observation_index}]",
            )
        if not joint_observation and selector.body_names is None:
            raise ConfigError(
                "body observation type requires body_names",
                code="OBSERVATION_TARGET_MISMATCH",
                path=f"observations[{observation_index}]",
            )
        if selector.joint_names is not None:
            selected = match_names(
                selector.joint_names,
                joint_names,
                preserve_order=selector.preserve_order,
                path=f"observations[{observation_index}].joint_names",
            )
            return _SelectorKind.JOINT, tuple(joint_names[index] for index in selected)
        assert selector.body_names is not None
        selected = match_names(
            selector.body_names,
            body_names,
            preserve_order=selector.preserve_order,
            path=f"observations[{observation_index}].body_names",
        )
        return _SelectorKind.BODY, tuple(body_names[index] for index in selected)

    def replace_actuators(self, **overrides: Any) -> RobotAssetCfg:
        """Return a new declaration with the same selectors and new group values."""

        return replace(
            self,
            actuators=tuple(replace(group, **overrides) for group in self.actuators),
        )


def replace_actuators(asset: RobotAssetCfg, **overrides: Any) -> RobotAssetCfg:
    """Derive a Robot declaration without mutating the source asset."""

    return asset.replace_actuators(**overrides)


__all__ = [
    "ActuatorGroupCfg",
    "NameSelector",
    "ObsSelectorCfg",
    "ResetBound",
    "ResetTargetCfg",
    "RobotAssetCfg",
    "RobotInitStateCfg",
    "replace_actuators",
]
