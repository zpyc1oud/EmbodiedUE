"""Parse YAML RobotConfig declarations into immutable Python semantics."""

from __future__ import annotations

import hashlib
import math
import random
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import cast

import yaml

from ...errors import ConfigError
from .yaml_loader import load_unique_yaml

ROBOT_CONFIG_FILENAME = "robot.yaml"


class ObsType(StrEnum):
    """Enumerate the closed observation-quantity list."""

    JOINT_POSITION = "joint_position"
    JOINT_VELOCITY = "joint_velocity"
    BODY_POSE = "body_pose"
    BODY_LINEAR_VELOCITY = "body_linear_velocity"
    BODY_ANGULAR_VELOCITY = "body_angular_velocity"
    GROUND_CLEARANCE = "ground_clearance"
    CONTACT = "contact"
    CONTACT_FORCE = "contact_force"
    TERRAIN_HEIGHT = "terrain_height"


class ResetTargetType(StrEnum):
    """Enumerate explicit Robot reset targets."""

    JOINT_POSITION = "joint_position"
    JOINT_VELOCITY = "joint_velocity"
    ROOT_POSE = "root_pose"
    ROOT_VELOCITY = "root_velocity"


@dataclass(frozen=True, slots=True)
class ActuatorConfig:
    """Declare one unified actuator."""

    joint: str
    stiffness: float
    damping: float
    effort_limit: float
    default_pos: float
    action_scale: float


@dataclass(frozen=True, slots=True)
class ObservationConfig:
    """Declare one observation with an explicit joint or body target."""

    type: ObsType
    target: str
    target_kind: str


@dataclass(frozen=True, slots=True)
class ResetTarget:
    """Declare one explicit reset target and its SI distribution.

    Joint distributions are offsets from the bound RobotSpec reference. Root
    pose vectors use ``[x, y, z, qx, qy, qz, qw]`` and root velocity vectors
    use ``[vx, vy, vz, wx, wy, wz]`` as absolute Slot-local values.
    """

    target_type: ResetTargetType
    joint: str | None
    distribution_type: str
    lower: float
    upper: float
    stream_id: str
    lower_values: tuple[float, ...] = ()
    upper_values: tuple[float, ...] = ()

    @property
    def wire_key(self) -> str:
        return f"{self.target_type.value}:{self.joint}" if self.joint else self.target_type.value

    @property
    def bounds(self) -> tuple[tuple[float, float], ...]:
        """Return one lower/upper pair per scalar in the reset target."""

        if not self.lower_values:
            return ((self.lower, self.upper),)
        return tuple(zip(self.lower_values, self.upper_values, strict=True))


@dataclass(frozen=True, slots=True)
class ResetConfig:
    """Declare explicit Robot reset distributions."""

    distributions: tuple[ResetTarget, ...]

    def sample(
        self,
        *,
        seed: int,
        episode_index: int,
        slot_ids: Sequence[int],
    ) -> dict[str, tuple[float | tuple[float, ...], ...]]:
        """Sample selected slots deterministically from each configured stream."""
        result: dict[str, tuple[float | tuple[float, ...], ...]] = {}
        for target in self.distributions:
            values: list[float | tuple[float, ...]] = []
            bounds = target.bounds
            for slot_id in slot_ids:
                stream_seed = _stable_reset_seed(seed, episode_index, slot_id, target.stream_id)
                generator = random.Random(stream_seed)
                sampled = tuple(
                    generator.uniform(lower, upper) if target.distribution_type == "uniform" else lower
                    for lower, upper in bounds
                )
                values.append(sampled[0] if len(sampled) == 1 else sampled)
            result[target.wire_key] = tuple(values)
        return result


def _stable_reset_seed(seed: int, episode_index: int, slot_id: int, stream_id: str) -> int:
    """Derive a process-independent seed for one reset stream and Slot."""
    digest = hashlib.blake2b(digest_size=8)
    digest.update(str(seed).encode("utf-8"))
    digest.update(b"/")
    digest.update(str(episode_index).encode("utf-8"))
    digest.update(b"/")
    digest.update(str(slot_id).encode("utf-8"))
    digest.update(b"/")
    digest.update(stream_id.encode("utf-8"))
    return int.from_bytes(digest.digest(), "little")


@dataclass(frozen=True, slots=True)
class RobotConfig:
    """Hold parsed, validated Robot semantics."""

    actuators: tuple[ActuatorConfig, ...]
    observations: tuple[ObservationConfig, ...]
    reset: ResetConfig


def load_robot_config(path: Path) -> RobotConfig:
    """Load and validate an explicit Robot semantics file."""
    config_path = path / ROBOT_CONFIG_FILENAME if path.is_dir() else path
    try:
        text = config_path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise ConfigError(
            f"robot config not found at {config_path}", code="ROBOT_CONFIG_NOT_FOUND", path=str(config_path)
        ) from exc
    return parse_robot_config(text, source=str(config_path))


def parse_robot_config(text: str, *, source: str = "<string>") -> RobotConfig:
    """Parse and validate explicit semantic YAML."""
    try:
        document = load_unique_yaml(text)
    except yaml.YAMLError as exc:
        raise ConfigError("robot config is not valid YAML", code="INVALID_ROBOT_CONFIG", path=source) from exc
    if not isinstance(document, dict):
        raise ConfigError("robot config must be a YAML mapping", code="INVALID_ROBOT_CONFIG", path=source)
    mapping = cast(Mapping[str, object], document)
    if set(mapping) != {"actuators", "observations", "reset"}:
        raise ConfigError(
            "robot config must contain exactly actuators, observations, and reset",
            code="INVALID_ROBOT_CONFIG",
            path=source,
        )
    return RobotConfig(
        actuators=_parse_actuators(mapping["actuators"]),
        observations=_parse_observations(mapping["observations"]),
        reset=_parse_reset(mapping["reset"]),
    )


def _parse_actuators(raw: object) -> tuple[ActuatorConfig, ...]:
    if not isinstance(raw, list) or not raw:
        raise ConfigError("actuators must be a non-empty list", code="INVALID_ROBOT_CONFIG", path="actuators")
    actuators: list[ActuatorConfig] = []
    seen: set[str] = set()
    required = {"joint", "stiffness", "damping", "effort_limit", "default_pos", "action_scale"}
    for index, item in enumerate(cast(list[object], raw)):
        path = f"actuators[{index}]"
        if not isinstance(item, dict):
            raise ConfigError("actuator must be a mapping", code="INVALID_ROBOT_CONFIG", path=path)
        entry = cast(Mapping[str, object], item)
        if set(entry) != required:
            missing = sorted(required - set(entry))
            raise ConfigError(f"actuator keys are invalid; missing={missing}", code="INVALID_ROBOT_CONFIG", path=path)
        joint = entry["joint"]
        if not isinstance(joint, str) or not joint:
            raise ConfigError(
                "actuator joint must be a non-empty string", code="INVALID_ROBOT_CONFIG", path=f"{path}.joint"
            )
        if joint in seen:
            raise ConfigError(
                f"actuator joint '{joint}' is declared more than once", code="DUPLICATE_ACTUATOR", path=f"{path}.joint"
            )
        seen.add(joint)
        stiffness = _non_negative(entry["stiffness"], path=f"{path}.stiffness")
        damping = _non_negative(entry["damping"], path=f"{path}.damping")
        if stiffness == 0.0 and damping == 0.0:
            raise ConfigError(
                "zero-gain actuator is invalid; omit passive joints", code="ZERO_GAIN_ACTUATOR", path=path
            )
        actuators.append(
            ActuatorConfig(
                joint=joint,
                stiffness=stiffness,
                damping=damping,
                effort_limit=_positive(entry["effort_limit"], path=f"{path}.effort_limit"),
                default_pos=_finite(entry["default_pos"], path=f"{path}.default_pos"),
                action_scale=_positive(entry["action_scale"], path=f"{path}.action_scale"),
            )
        )
    return tuple(actuators)


def _parse_observations(raw: object) -> tuple[ObservationConfig, ...]:
    if not isinstance(raw, list) or not raw:
        raise ConfigError("observations must be a non-empty list", code="INVALID_ROBOT_CONFIG", path="observations")
    observations: list[ObservationConfig] = []
    for index, item in enumerate(cast(list[object], raw)):
        path = f"observations[{index}]"
        if not isinstance(item, dict):
            raise ConfigError("observation must be a mapping", code="INVALID_ROBOT_CONFIG", path=path)
        entry = cast(Mapping[str, object], item)
        item_keys = set(entry)
        if item_keys not in ({"type", "joint"}, {"type", "body"}):
            raise ConfigError(
                "observation must contain exactly type and joint/body", code="INVALID_ROBOT_CONFIG", path=path
            )
        raw_type = entry["type"]
        if not isinstance(raw_type, str):
            raise ConfigError("observation type must be a string", code="INVALID_ROBOT_CONFIG", path=f"{path}.type")
        try:
            obs_type = ObsType(raw_type)
        except ValueError as exc:
            raise ConfigError(
                f"unsupported observation type '{raw_type}'", code="INVALID_ROBOT_CONFIG", path=f"{path}.type"
            ) from exc
        expected_key = "joint" if obs_type in {ObsType.JOINT_POSITION, ObsType.JOINT_VELOCITY} else "body"
        if expected_key not in entry:
            raise ConfigError(f"{raw_type} requires {expected_key}", code="INVALID_ROBOT_CONFIG", path=path)
        target = entry[expected_key]
        if not isinstance(target, str) or not target:
            raise ConfigError(
                f"observation {expected_key} must be a non-empty string",
                code="INVALID_ROBOT_CONFIG",
                path=f"{path}.{expected_key}",
            )
        observations.append(ObservationConfig(obs_type, target, expected_key))
    return tuple(observations)


def _parse_reset(raw: object) -> ResetConfig:
    if not isinstance(raw, dict) or set(cast(Mapping[str, object], raw)) != {"distributions"}:
        raise ConfigError("reset must contain exactly distributions", code="INVALID_ROBOT_CONFIG", path="reset")
    raw_distributions = cast(Mapping[str, object], raw)["distributions"]
    if not isinstance(raw_distributions, list):
        raise ConfigError("reset.distributions must be a list", code="INVALID_ROBOT_CONFIG", path="reset.distributions")
    distributions: list[ResetTarget] = []
    for index, raw_distribution in enumerate(cast(list[object], raw_distributions)):
        path = f"reset.distributions[{index}]"
        if not isinstance(raw_distribution, Mapping):
            raise ConfigError("reset distribution must be a mapping", code="INVALID_ROBOT_CONFIG", path=path)
        distribution = cast(Mapping[str, object], raw_distribution)
        required_keys = {"type", "distribution"}
        if set(distribution) != required_keys and set(distribution) != required_keys | {"joint"}:
            raise ConfigError("reset target keys are invalid", code="INVALID_ROBOT_CONFIG", path=path)
        raw_type = distribution["type"]
        if not isinstance(raw_type, str):
            raise ConfigError("reset target type is invalid", code="INVALID_ROBOT_CONFIG", path=f"{path}.type")
        try:
            target_type = ResetTargetType(raw_type)
        except ValueError as exc:
            raise ConfigError("reset target type is invalid", code="INVALID_ROBOT_CONFIG", path=f"{path}.type") from exc
        joint = distribution.get("joint")
        if target_type in {ResetTargetType.JOINT_POSITION, ResetTargetType.JOINT_VELOCITY}:
            if not isinstance(joint, str) or not joint:
                raise ConfigError("joint reset target requires joint", code="INVALID_ROBOT_CONFIG", path=path)
        elif joint is not None:
            raise ConfigError("root reset target cannot contain joint", code="INVALID_ROBOT_CONFIG", path=path)
        raw_distribution_value = distribution["distribution"]
        if not isinstance(raw_distribution_value, Mapping):
            raise ConfigError(
                "reset distribution must be a mapping", code="INVALID_ROBOT_CONFIG", path=f"{path}.distribution"
            )
        inner = cast(Mapping[str, object], raw_distribution_value)
        if set(inner) != {"type", "lower", "upper", "stream_id"}:
            raise ConfigError(
                "reset distribution keys are invalid", code="INVALID_ROBOT_CONFIG", path=f"{path}.distribution"
            )
        distribution_type = inner["type"]
        stream_id = inner["stream_id"]
        if (
            not isinstance(distribution_type, str)
            or distribution_type not in {"constant", "uniform"}
            or not isinstance(stream_id, str)
            or not stream_id
        ):
            raise ConfigError(
                "reset distribution type or stream_id is invalid",
                code="INVALID_ROBOT_CONFIG",
                path=f"{path}.distribution",
            )
        expected_width = 1 if target_type in {
            ResetTargetType.JOINT_POSITION,
            ResetTargetType.JOINT_VELOCITY,
        } else None
        lower_values = _reset_values(
            inner["lower"],
            path=f"{path}.distribution.lower",
            expected_width=expected_width,
        )
        upper_values = _reset_values(
            inner["upper"],
            path=f"{path}.distribution.upper",
            expected_width=expected_width,
        )
        if len(lower_values) != len(upper_values) or any(
            lower > upper for lower, upper in zip(lower_values, upper_values, strict=True)
        ):
            raise ConfigError(
                "reset distribution bounds are invalid", code="INVALID_ROBOT_CONFIG", path=f"{path}.distribution"
            )
        distributions.append(
            ResetTarget(
                target_type,
                joint,
                distribution_type,
                lower_values[0],
                upper_values[0],
                stream_id,
                lower_values if len(lower_values) > 1 else (),
                upper_values if len(upper_values) > 1 else (),
            )
        )
    return ResetConfig(tuple(distributions))


def _finite(value: object, *, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ConfigError("expected a finite number", code="INVALID_ROBOT_CONFIG", path=path)
    return float(value)


def _reset_values(value: object, *, path: str, expected_width: int | None) -> tuple[float, ...]:
    """Parse a scalar or explicitly sized reset vector."""

    if isinstance(value, list):
        if expected_width == 1 or not value:
            raise ConfigError("reset bound vector has the wrong width", code="INVALID_ROBOT_CONFIG", path=path)
        if expected_width is not None and len(value) != expected_width:
            raise ConfigError("reset bound vector has the wrong width", code="INVALID_ROBOT_CONFIG", path=path)
        return tuple(_finite(item, path=f"{path}[{index}]") for index, item in enumerate(value))
    if expected_width is not None and expected_width != 1:
        raise ConfigError("reset bound must be a vector", code="INVALID_ROBOT_CONFIG", path=path)
    return (_finite(value, path=path),)


def _non_negative(value: object, *, path: str) -> float:
    number = _finite(value, path=path)
    if number < 0:
        raise ConfigError("expected a non-negative number", code="INVALID_ROBOT_CONFIG", path=path)
    return number


def _positive(value: object, *, path: str) -> float:
    number = _finite(value, path=path)
    if number <= 0:
        raise ConfigError("expected a positive number", code="INVALID_ROBOT_CONFIG", path=path)
    return number
