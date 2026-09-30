"""Parse the asset-adjacent YAML RobotConfig into immutable declarations.

The RobotConfig is a human-authored text file living next to a UE asset
(spec D2/D12). It declares the semantics that reflection cannot read: which
joints are actuators and how they are driven, which observations to collect,
and how reset randomizes the robot state. Topology comes from UE reflection
and is merged in a later step.
"""

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


@dataclass(frozen=True, slots=True)
class ConstraintFrame:
    """Hold one constraint frame in SI position and XYZW quaternion form."""

    position_metres: tuple[float, float, float]
    rotation_xyzw: tuple[float, float, float, float]


@dataclass(frozen=True, slots=True)
class JointTopology:
    """Hold one single-DOF joint reported by UE at initialization."""

    name: str
    parent_body_index: int
    child_body_index: int
    degrees_of_freedom: int
    coordinate: str
    coordinate_type: str
    unit: str
    lower_limit: float | None
    upper_limit: float | None
    child_frame: ConstraintFrame
    parent_frame: ConstraintFrame
    default_position: float


def _constraint_frame_from_payload(payload: Mapping[str, object]) -> ConstraintFrame:
    """Convert a validated UE constraint frame payload to its typed form."""
    try:
        if set(payload) != {"position_metres", "rotation_xyzw"}:
            raise ValueError("constraint frame keys are invalid")
        position_value = payload["position_metres"]
        rotation_value = payload["rotation_xyzw"]
        if not isinstance(position_value, list) or not isinstance(rotation_value, list):
            raise ValueError("constraint frame values must be arrays")
        position = cast(list[object], position_value)
        rotation = cast(list[object], rotation_value)
        if len(position) != 3 or len(rotation) != 4:
            raise ValueError("constraint frame arrays must have lengths 3 and 4")
        if not all(type(value) in {int, float} and not isinstance(value, bool) for value in position + rotation):
            raise ValueError("constraint frame values must be numbers")
        position_values = tuple(float(cast(float, value)) for value in position)
        rotation_values = tuple(float(cast(float, value)) for value in rotation)
        if not all(math.isfinite(value) for value in position_values + rotation_values):
            raise ValueError("constraint frame values must be finite")
        rotation_length = math.sqrt(sum(value * value for value in rotation_values))
        if not math.isclose(rotation_length, 1.0, rel_tol=1e-6, abs_tol=1e-6):
            raise ValueError("constraint frame quaternion must be normalized")
        return ConstraintFrame(
            position_metres=(position_values[0], position_values[1], position_values[2]),
            rotation_xyzw=(rotation_values[0], rotation_values[1], rotation_values[2], rotation_values[3]),
        )
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError("invalid constraint frame payload") from exc


@dataclass(frozen=True, slots=True)
class RobotTopology:
    """Hold the reflected asset topology reported by UE."""

    body_names: tuple[str, ...]
    body_motion_types: tuple[str, ...]
    root_body_index: int
    fixed_base: bool
    joints: tuple[JointTopology, ...]

    @classmethod
    def from_response(cls, payload: Mapping[str, object]) -> RobotTopology:
        """Build and validate a topology from the initialize response."""
        body_names_value = payload.get("body_names")
        body_motion_types_value = payload.get("body_motion_types")
        joints_value = payload.get("joints")
        root_body_index_value = payload.get("root_body_index")
        fixed_base_value = payload.get("fixed_base")
        if (
            not isinstance(body_names_value, list)
            or not isinstance(body_motion_types_value, list)
            or not isinstance(joints_value, list)
            or type(root_body_index_value) is not int
            or type(fixed_base_value) is not bool
        ):
            raise ValueError("invalid topology response types")
        if not body_names_value or not all(isinstance(name, str) and name for name in body_names_value):
            raise ValueError("topology body names must be non-empty strings")
        body_names = tuple(cast(list[str], body_names_value))
        if len(set(body_names)) != len(body_names):
            raise ValueError("topology body names must be unique")
        if len(body_motion_types_value) != len(body_names) or not all(
            isinstance(motion_type, str) and motion_type in {"simulated", "kinematic"}
            for motion_type in body_motion_types_value
        ):
            raise ValueError("topology body motion types are invalid")
        body_motion_types = tuple(cast(list[str], body_motion_types_value))
        root_body_index = root_body_index_value
        fixed_base = fixed_base_value
        if not 0 <= root_body_index < len(body_names):
            raise ValueError("topology root body index is out of range")
        kinematic_indices = [index for index, motion_type in enumerate(body_motion_types) if motion_type == "kinematic"]
        if len(kinematic_indices) > 1 or (kinematic_indices and kinematic_indices != [root_body_index]):
            raise ValueError("topology kinematic bodies must contain only the root")
        if fixed_base != (body_motion_types[root_body_index] == "kinematic"):
            raise ValueError("topology fixed_base does not match the root motion type")
        if len(joints_value) != len(body_names) - 1:
            raise ValueError("topology must contain one joint per non-root body")

        coordinate_metadata = {
            "linear_x": ("prismatic", "m"),
            "linear_y": ("prismatic", "m"),
            "linear_z": ("prismatic", "m"),
            "swing1": ("revolute", "rad"),
            "swing2": ("revolute", "rad"),
            "twist": ("revolute", "rad"),
        }
        joint_keys = {
            "name",
            "parent_body_index",
            "child_body_index",
            "degrees_of_freedom",
            "coordinate",
            "coordinate_type",
            "unit",
            "default_position",
            "lower_limit",
            "upper_limit",
            "child_frame",
            "parent_frame",
        }
        seen_joint_names: set[str] = set()
        parent_by_child: dict[int, int] = {}
        joints: list[JointTopology] = []
        for item in joints_value:
            if not isinstance(item, Mapping) or set(item) != joint_keys:
                raise ValueError("topology joint keys are invalid")
            joint = cast(Mapping[str, object], item)
            name = joint["name"]
            if not isinstance(name, str) or not name or name in seen_joint_names:
                raise ValueError("topology joint names must be unique non-empty strings")
            seen_joint_names.add(name)
            parent = joint["parent_body_index"]
            child = joint["child_body_index"]
            if (
                type(parent) is not int
                or type(child) is not int
                or not (0 <= parent < len(body_names) and 0 <= child < len(body_names))
            ):
                raise ValueError("topology joint body indices are invalid")
            if parent == child or child in parent_by_child:
                raise ValueError("topology joint parent/child wiring is invalid")
            parent_by_child[child] = parent
            degrees_of_freedom = joint["degrees_of_freedom"]
            if type(degrees_of_freedom) is not int or degrees_of_freedom != 1:
                raise ValueError("topology joints must have exactly one degree of freedom")
            coordinate = joint["coordinate"]
            coordinate_type = joint["coordinate_type"]
            unit = joint["unit"]
            if not isinstance(coordinate, str) or coordinate not in coordinate_metadata:
                raise ValueError("topology joint coordinate is invalid")
            expected_type, expected_unit = coordinate_metadata[coordinate]
            if coordinate_type != expected_type or unit != expected_unit:
                raise ValueError("topology joint coordinate metadata is inconsistent")
            lower = joint["lower_limit"]
            upper = joint["upper_limit"]
            default_position = joint["default_position"]
            if (
                type(default_position) not in {int, float}
                or isinstance(default_position, bool)
                or not math.isfinite(float(cast(float, default_position)))
            ):
                raise ValueError("topology joint default position is invalid")
            lower_value: float | None = None
            upper_value: float | None = None
            if (lower is None) != (upper is None):
                raise ValueError("topology joint limits must be both null or both numeric")
            if lower is not None:
                if any(type(value) not in {int, float} or isinstance(value, bool) for value in (lower, upper)):
                    raise ValueError("topology joint limits are invalid")
                lower_value = float(cast(float, lower))
                upper_value = float(cast(float, upper))
                if not math.isfinite(lower_value) or not math.isfinite(upper_value) or lower_value > upper_value:
                    raise ValueError("topology joint limits are invalid")
            joints.append(
                JointTopology(
                    name=name,
                    parent_body_index=parent,
                    child_body_index=child,
                    degrees_of_freedom=degrees_of_freedom,
                    coordinate=coordinate,
                    coordinate_type=coordinate_type,
                    unit=unit,
                    lower_limit=lower_value,
                    upper_limit=upper_value,
                    child_frame=_constraint_frame_from_payload(cast(Mapping[str, object], joint["child_frame"])),
                    parent_frame=_constraint_frame_from_payload(cast(Mapping[str, object], joint["parent_frame"])),
                    default_position=float(cast(float, default_position)),
                )
            )

        root_candidates = [index for index in range(len(body_names)) if index not in parent_by_child]
        if root_candidates != [root_body_index]:
            raise ValueError("topology must have exactly one reachable root")
        reachable = {root_body_index}
        changed = True
        while changed:
            changed = False
            for child, parent in parent_by_child.items():
                if parent in reachable and child not in reachable:
                    reachable.add(child)
                    changed = True
        if len(reachable) != len(body_names):
            raise ValueError("topology bodies must form one connected tree")
        return cls(body_names, body_motion_types, root_body_index, fixed_base, tuple(joints))


@dataclass(frozen=True, slots=True)
class ActuatorSpec:
    """Bind one declared actuator to one scalar topology joint."""

    joint: str
    joint_index: int
    body_index: int
    coordinate_type: str
    coordinate: str
    target_mode: str
    target_unit: str
    stiffness: float
    damping: float
    effort_limit: float
    default_pos: float
    action_scale: float
    index: int


@dataclass(frozen=True, slots=True)
class ObservationSpec:
    """Bind one declared observation to an explicit joint or body index."""

    type: ObsType
    target: str
    target_kind: str
    body_index: int | None
    joint_index: int | None
    unit: str
    index: int


@dataclass(frozen=True, slots=True)
class BoundResetTarget:
    """Bind one explicit reset target to topology indices and SI unit."""

    target_type: ResetTargetType
    joint: str | None
    joint_index: int | None
    body_index: int | None
    unit: str
    reference: float
    lower: float
    upper: float
    stream_id: str
    index: int
    component_index: int = 0
    component_count: int = 1

    @property
    def sample_key(self) -> str:
        """Return the source distribution key used by Python reset sampling."""

        return f"{self.target_type.value}:{self.joint}" if self.joint else self.target_type.value

    @property
    def wire_key(self) -> str:
        if self.component_count == 1:
            return self.sample_key
        return f"{self.sample_key}:{self.component_index}"


@dataclass(frozen=True, slots=True)
class RobotSpec:
    """Hold merged semantics and reflected topology indices."""

    actuators: tuple[ActuatorSpec, ...]
    observations: tuple[ObservationSpec, ...]
    reset: tuple[BoundResetTarget, ...]
    body_names: tuple[str, ...]
    topology: RobotTopology

    def resolve_reset_values(
        self,
        sampled: Mapping[str, tuple[float | tuple[float, ...], ...]],
        slot_index: int,
    ) -> tuple[float, ...]:
        """Resolve one Slot's sampled offsets into the indexed absolute reset vector."""

        values: list[float] = []
        for target in self.reset:
            sampled_value = sampled[target.sample_key][slot_index]
            component = (
                cast(float, sampled_value)
                if target.component_count == 1
                else cast(tuple[float, ...], sampled_value)[target.component_index]
            )
            values.append(target.reference + component)
        return tuple(values)


def merge_robot_spec(config: RobotConfig, topology: RobotTopology) -> RobotSpec:
    """Merge declared semantics with reflected topology into a ``RobotSpec``."""
    joint_by_name: dict[str, tuple[int, JointTopology]] = {}
    for joint_index, joint in enumerate(topology.joints):
        if joint.name in joint_by_name:
            raise ConfigError(
                f"topology declares joint '{joint.name}' more than once",
                code="AMBIGUOUS_TOPOLOGY_JOINT",
                path=f"topology.joints.{joint.name}",
            )
        joint_by_name[joint.name] = (joint_index, joint)
    body_index_by_name: dict[str, int] = {}
    for body_index, body_name in enumerate(topology.body_names):
        if body_name in body_index_by_name:
            raise ConfigError(
                f"topology declares body '{body_name}' more than once",
                code="AMBIGUOUS_TOPOLOGY_BODY",
                path=f"topology.body_names[{body_index}]",
            )
        body_index_by_name[body_name] = body_index

    actuators: list[ActuatorSpec] = []
    for index, actuator in enumerate(config.actuators):
        joint_index, joint = _resolve_joint(actuator.joint, joint_by_name, f"actuators[{index}].joint")
        if joint.degrees_of_freedom != 1:
            raise ConfigError(
                "scalar actuator config requires a one-degree-of-freedom joint",
                code="ACTUATOR_DIMENSION_MISMATCH",
                path=f"actuators[{index}].joint",
            )
        if actuator.stiffness == 0.0 and actuator.damping == 0.0:
            raise ConfigError(
                "zero-gain actuator is invalid; omit passive joints",
                code="ZERO_GAIN_ACTUATOR",
                path=f"actuators[{index}]",
            )
        target_mode = "position" if actuator.stiffness > 0.0 else "effort"
        target_unit = (
            joint.unit if target_mode == "position" else ("N" if joint.coordinate_type == "prismatic" else "N*m")
        )
        actuators.append(
            ActuatorSpec(
                joint=actuator.joint,
                joint_index=joint_index,
                body_index=joint.child_body_index,
                coordinate_type=joint.coordinate_type,
                coordinate=joint.coordinate,
                target_mode=target_mode,
                target_unit=target_unit,
                stiffness=actuator.stiffness,
                damping=actuator.damping,
                effort_limit=actuator.effort_limit,
                default_pos=actuator.default_pos,
                action_scale=actuator.action_scale,
                index=index,
            )
        )

    observations: list[ObservationSpec] = []
    seen_observation_keys: set[tuple[str, str, ObsType]] = set()
    for index, observation in enumerate(config.observations):
        observation_key = (observation.target_kind, observation.target, observation.type)
        if observation_key in seen_observation_keys:
            raise ConfigError(
                f"observation '{observation.type.value}:{observation.target}' is declared more than once",
                code="DUPLICATE_OBSERVATION",
                path=f"observations[{index}]",
            )
        seen_observation_keys.add(observation_key)
        if observation.target_kind == "joint":
            joint_index, joint = _resolve_joint(observation.target, joint_by_name, f"observations[{index}].joint")
            if observation.type not in {ObsType.JOINT_POSITION, ObsType.JOINT_VELOCITY}:
                raise ConfigError(
                    "body observation type must use body target",
                    code="OBSERVATION_TARGET_MISMATCH",
                    path=f"observations[{index}].target",
                )
            unit = joint.unit if observation.type is ObsType.JOINT_POSITION else f"{joint.unit}/s"
            observations.append(
                ObservationSpec(
                    observation.type,
                    observation.target,
                    "joint",
                    joint.child_body_index,
                    joint_index,
                    unit,
                    index,
                )
            )
        else:
            body_index = _resolve_body(observation.target, body_index_by_name, index)
            if observation.type in {ObsType.JOINT_POSITION, ObsType.JOINT_VELOCITY}:
                raise ConfigError(
                    "joint observation type must use joint target",
                    code="OBSERVATION_TARGET_MISMATCH",
                    path=f"observations[{index}].target",
                )
            if observation.type is ObsType.TERRAIN_HEIGHT and body_index != topology.root_body_index:
                raise ConfigError(
                    "terrain_height must target the topology root body",
                    code="OBSERVATION_TARGET_MISMATCH",
                    path=f"observations[{index}].target",
                )
            unit = {
                ObsType.BODY_POSE: "m,quat_xyzw",
                ObsType.BODY_LINEAR_VELOCITY: "m/s",
                ObsType.BODY_ANGULAR_VELOCITY: "rad/s",
                ObsType.GROUND_CLEARANCE: "m",
                ObsType.CONTACT: "fraction",
                ObsType.CONTACT_FORCE: "N",
                ObsType.TERRAIN_HEIGHT: "m",
            }[observation.type]
            observations.append(
                ObservationSpec(observation.type, observation.target, "body", body_index, None, unit, index)
            )

    position_reference_by_joint = {actuator.joint_index: actuator.default_pos for actuator in actuators}
    reset_targets: list[BoundResetTarget] = []
    seen_reset_keys: set[str] = set()
    for index, target in enumerate(config.reset.distributions):
        reset_joint_index: int | None = None
        reset_body_index: int | None = None
        resolved_joint: JointTopology | None = None
        if target.joint is not None:
            reset_joint_index, resolved_joint = _resolve_joint(
                target.joint, joint_by_name, f"reset.distributions[{index}].joint"
            )
            unit = (
                resolved_joint.unit
                if target.target_type is ResetTargetType.JOINT_POSITION
                else f"{resolved_joint.unit}/s"
            )
            bounds = target.bounds
            if len(bounds) != 1:
                raise ConfigError(
                    "joint reset targets must contain one scalar",
                    code="RESET_TARGET_DIMENSION_MISMATCH",
                    path=f"reset.distributions[{index}].distribution",
                )
            reference = (
                position_reference_by_joint.get(reset_joint_index, resolved_joint.default_position)
                if target.target_type is ResetTargetType.JOINT_POSITION
                else 0.0
            )
            if (
                target.target_type is ResetTargetType.JOINT_POSITION
                and resolved_joint.lower_limit is not None
                and resolved_joint.upper_limit is not None
                and (
                    reference + bounds[0][0] < resolved_joint.lower_limit
                    or reference + bounds[0][1] > resolved_joint.upper_limit
                )
            ):
                raise ConfigError(
                    "joint reset position is outside the reflected joint limits",
                    code="RESET_POSITION_OUT_OF_RANGE",
                    path=f"reset.distributions[{index}].distribution",
                )
        else:
            if topology.fixed_base:
                raise ConfigError(
                    "fixed base does not accept root reset",
                    code="FIXED_BASE_ROOT_RESET",
                    path=f"reset.distributions[{index}]",
                )
            reset_body_index = topology.root_body_index
            unit = "m,quat_xyzw" if target.target_type is ResetTargetType.ROOT_POSE else "m/s,rad/s"
            bounds = target.bounds
            reference = 0.0
            expected_width = 7 if target.target_type is ResetTargetType.ROOT_POSE else 6
            if len(bounds) != expected_width:
                raise ConfigError(
                    f"{target.target_type.value} reset requires {expected_width} scalars",
                    code="RESET_TARGET_DIMENSION_MISMATCH",
                    path=f"reset.distributions[{index}].distribution",
                )
        if target.wire_key in seen_reset_keys:
            raise ConfigError(
                "reset target is declared more than once",
                code="DUPLICATE_RESET_TARGET",
                path=f"reset.distributions[{index}]",
            )
        seen_reset_keys.add(target.wire_key)
        for component_index, (lower, upper) in enumerate(bounds):
            reset_targets.append(
                BoundResetTarget(
                    target_type=target.target_type,
                    joint=target.joint,
                    joint_index=reset_joint_index,
                    body_index=reset_body_index,
                    unit=unit,
                    reference=reference,
                    lower=lower,
                    upper=upper,
                    stream_id=target.stream_id,
                    index=len(reset_targets),
                    component_index=component_index,
                    component_count=len(bounds),
                )
            )

    for index, bound_actuator in enumerate(actuators):
        joint = topology.joints[bound_actuator.joint_index]
        if (
            joint.lower_limit is not None
            and joint.upper_limit is not None
            and not joint.lower_limit <= bound_actuator.default_pos <= joint.upper_limit
        ):
            raise ConfigError(
                "actuator default_pos is outside the reflected joint limits",
                code="DEFAULT_POSITION_OUT_OF_RANGE",
                path=f"actuators[{index}].default_pos",
            )

    return RobotSpec(tuple(actuators), tuple(observations), tuple(reset_targets), topology.body_names, topology)


def _resolve_joint(
    joint: str,
    joint_by_name: Mapping[str, tuple[int, JointTopology]],
    path: str,
) -> tuple[int, JointTopology]:
    if joint not in joint_by_name:
        raise ConfigError(f"joint '{joint}' is not present in the reflected topology", code="UNKNOWN_JOINT", path=path)
    return joint_by_name[joint]


def _resolve_body(body: str, body_index_by_name: Mapping[str, int], index: int) -> int:
    if body not in body_index_by_name:
        raise ConfigError(
            f"body '{body}' is not present in the reflected topology",
            code="UNKNOWN_BODY",
            path=f"observations[{index}].target",
        )
    return body_index_by_name[body]


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


__all__ = [
    "ActuatorConfig",
    "ActuatorSpec",
    "BoundResetTarget",
    "ConstraintFrame",
    "JointTopology",
    "ObsType",
    "ObservationConfig",
    "ObservationSpec",
    "ResetConfig",
    "ResetTarget",
    "ResetTargetType",
    "RobotConfig",
    "RobotSpec",
    "RobotTopology",
    "load_robot_config",
    "merge_robot_spec",
    "parse_robot_config",
]
