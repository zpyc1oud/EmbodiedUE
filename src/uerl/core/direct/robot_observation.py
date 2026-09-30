"""Build the indexed RobotSpec observation descriptors for schema negotiation."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from types import MappingProxyType
from typing import Any, cast

from ...errors import ConfigError
from ..config.robot import ObsType, RobotSpec

ROBOT_OBSERVATION_SOURCE = "uerl.robot"
ROBOT_OBSERVATION_FRAME = "slot/local"
ROBOT_JOINT_OBSERVATION_FRAME = "constraint"
ROBOT_CONTACT_OBSERVATION_FRAME = "coordinate-free"


@dataclass(frozen=True, slots=True)
class ObservationShapeTable:
    """Hold UE-published shapes keyed by the closed Robot observation type."""

    shapes: Mapping[ObsType, tuple[int, ...]]

    def __post_init__(self) -> None:
        normalized: dict[ObsType, tuple[int, ...]] = {}
        for observation_type, shape in self.shapes.items():
            resolved_type = (
                observation_type
                if isinstance(observation_type, ObsType)
                else ObsType(str(observation_type))
            )
            normalized[resolved_type] = _validate_shape(shape, f"shapes.{resolved_type.value}")
        object.__setattr__(self, "shapes", MappingProxyType(normalized))

    @classmethod
    def from_descriptor(
        cls,
        descriptor: Mapping[str, Any] | list[Mapping[str, Any]],
    ) -> ObservationShapeTable:
        """Build the table from an Initialize descriptor returned by UE.

        ``descriptor`` may be the complete Initialize response or the raw
        ``available_state_schema`` array. Non-Robot State fields are ignored;
        every Robot semantic must agree wherever it is published.
        """

        fields = _descriptor_fields(descriptor)
        shapes: dict[ObsType, tuple[int, ...]] = {}
        for index, field in enumerate(fields):
            if not isinstance(field, Mapping):
                raise ConfigError(
                    "UE observation descriptor must contain objects",
                    code="ROBOT_OBSERVATION_DESCRIPTOR_INVALID",
                    path=f"available_state_schema[{index}]",
                )
            source = field.get("source")
            if source is not None and source != ROBOT_OBSERVATION_SOURCE:
                continue
            semantic = field.get("semantic")
            try:
                observation_type = ObsType(str(semantic))
            except ValueError as exc:
                raise ConfigError(
                    f"UE observation descriptor has unsupported semantic {semantic!r}",
                    code="UNSUPPORTED_OBSERVATION",
                    path=f"available_state_schema[{index}].semantic",
                ) from exc
            shape = _validate_shape(field.get("shape"), f"available_state_schema[{index}].shape")
            previous = shapes.get(observation_type)
            if previous is not None and previous != shape:
                raise ConfigError(
                    f"UE observation semantic '{observation_type.value}' publishes conflicting shapes "
                    f"{previous} and {shape}",
                    code="ROBOT_OBSERVATION_SHAPE_CONFLICT",
                    path=f"available_state_schema[{index}].shape",
                )
            shapes[observation_type] = shape
        return cls(shapes)

    def shape_for(self, observation_type: ObsType) -> tuple[int, ...]:
        """Return one UE-published shape or fail before schema commit."""

        try:
            return self.shapes[observation_type]
        except KeyError as exc:
            raise ConfigError(
                f"UE descriptor does not publish a shape for observation '{observation_type.value}'",
                code="ROBOT_OBSERVATION_SHAPE_MISSING",
                path=f"available_state_schema.{observation_type.value}",
            ) from exc


_OBSERVATION_SEMANTICS: dict[ObsType, str] = {
    ObsType.JOINT_POSITION: "joint_position",
    ObsType.JOINT_VELOCITY: "joint_velocity",
    ObsType.BODY_POSE: "body_pose",
    ObsType.BODY_LINEAR_VELOCITY: "body_linear_velocity",
    ObsType.BODY_ANGULAR_VELOCITY: "body_angular_velocity",
    ObsType.GROUND_CLEARANCE: "ground_clearance",
    ObsType.CONTACT: "contact",
    ObsType.CONTACT_FORCE: "contact_force",
    ObsType.TERRAIN_HEIGHT: "terrain_height",
}


def robot_observation_schema(
    spec: RobotSpec,
    observation_shapes: ObservationShapeTable,
) -> tuple[Mapping[str, Any], ...]:
    """Build ordered State descriptors from explicit indexed targets."""
    descriptors: list[Mapping[str, Any]] = []
    for index, observation in enumerate(spec.observations):
        if observation.type not in _OBSERVATION_SEMANTICS:
            raise ConfigError(
                f"unsupported observation type '{observation.type.value}'",
                code="UNSUPPORTED_OBSERVATION",
                path=f"observations[{index}].type",
            )
        semantic = _OBSERVATION_SEMANTICS[observation.type]
        shape = observation_shapes.shape_for(observation.type)
        extensions: dict[str, Any] = {
            "target": observation.target,
            "target_kind": observation.target_kind,
            "observation_type": observation.type.value,
        }
        if observation.body_index is not None:
            extensions["body_index"] = observation.body_index
            extensions["body_name"] = spec.body_names[observation.body_index]
        if observation.joint_index is not None:
            extensions["joint_index"] = observation.joint_index
            joint = spec.topology.joints[observation.joint_index]
            extensions["joint_name"] = joint.name
            extensions["coordinate"] = joint.coordinate
            extensions["coordinate_type"] = joint.coordinate_type
        target_index = observation.joint_index if observation.joint_index is not None else observation.body_index
        if target_index is None:
            raise ConfigError(
                "observation is missing its resolved topology index",
                code="UNRESOLVED_OBSERVATION",
                path=f"observations[{index}]",
            )
        target_namespace = "joint" if observation.target_kind == "joint" else "body"
        if observation.joint_index is not None:
            target_name = spec.topology.joints[observation.joint_index].name
        else:
            # target_index is body_index once joint_index is absent (checked above).
            target_name = spec.body_names[target_index]
        if "." in target_name:
            raise ConfigError(
                f"part name '{target_name}' must not contain '.'",
                code="INVALID_PART_NAME",
                path=f"observations[{index}]",
            )
        descriptors.append(
            MappingProxyType(
                {
                    "name": f"robot.{target_namespace}.{target_name}.{observation.type.value}",
                    "dtype": "float32",
                    "shape": shape,
                    "unit": observation.unit,
                    "frame": (
                        ROBOT_CONTACT_OBSERVATION_FRAME
                        if observation.type in {ObsType.CONTACT, ObsType.CONTACT_FORCE}
                        else ROBOT_JOINT_OBSERVATION_FRAME
                        if observation.target_kind == "joint"
                        else ROBOT_OBSERVATION_FRAME
                    ),
                    "semantic": semantic,
                    "source": ROBOT_OBSERVATION_SOURCE,
                    "extensions": MappingProxyType(extensions),
                }
            )
        )
    return tuple(descriptors)


def validate_robot_observation_groups(
    spec: RobotSpec,
    observation_groups: Mapping[str, tuple[str, ...]],
    observation_shapes: ObservationShapeTable,
) -> None:
    """Require Task groups to preserve the RobotConfig observation projection."""

    expected = _robot_observation_names(spec, observation_shapes)
    found_projection = False
    for group_name, fields in observation_groups.items():
        projection = tuple(field for field in fields if field.startswith("robot."))
        if not projection:
            continue
        found_projection = True
        expected_projection = tuple(name for name in expected if name in projection)
        if projection != expected_projection:
            raise ConfigError(
                f"observation group '{group_name}' changes the RobotConfig observation order",
                code="ROBOT_OBSERVATION_PROJECTION_MISMATCH",
                path=f"observation_groups.{group_name}",
            )
    if expected and not found_projection:
        raise ConfigError(
            "Task observation groups omit the RobotConfig projection",
            code="ROBOT_OBSERVATION_PROJECTION_MISMATCH",
            path="observation_groups",
        )


def validate_robot_observation_schema(
    spec: RobotSpec,
    state_requirements: tuple[Mapping[str, Any], ...],
    observation_shapes: ObservationShapeTable,
) -> None:
    """Require the committed Session columns to preserve RobotConfig order."""

    expected = _robot_observation_names(spec, observation_shapes)
    projection = tuple(
        name
        for descriptor in state_requirements
        if (name := cast(str, descriptor["name"])).startswith("robot.")
    )
    if projection != expected:
        raise ConfigError(
            "Session State schema changes the RobotConfig projection",
            code="ROBOT_OBSERVATION_PROJECTION_MISMATCH",
            path="state_requirements",
        )


def validate_robot_shape_agreement(
    spec: RobotSpec,
    descriptor: Mapping[str, Any] | list[Mapping[str, Any]],
    observation_shapes: ObservationShapeTable | None = None,
) -> ObservationShapeTable:
    """Validate the configured Robot projection against UE's available fields.

    The check runs after Describe and before Initialize commit. It compares the
    Python observation declaration's names, shapes, units, frames, semantics,
    and reflected binding metadata with the UE descriptor, so a mismatch names
    the concrete Robot field instead of surfacing later as a tensor error.
    """

    table = observation_shapes or ObservationShapeTable.from_descriptor(descriptor)
    state_fields = _index_descriptor_fields(descriptor, "available_state_schema")
    expected_state = robot_observation_schema(spec, table)
    _validate_declared_fields(expected_state, state_fields, "observation")
    return table


def _robot_observation_names(
    spec: RobotSpec,
    observation_shapes: ObservationShapeTable,
) -> tuple[str, ...]:
    return tuple(cast(str, descriptor["name"]) for descriptor in robot_observation_schema(spec, observation_shapes))


def _descriptor_fields(
    descriptor: Mapping[str, Any] | list[Mapping[str, Any]],
) -> list[Mapping[str, Any]]:
    if isinstance(descriptor, Mapping):
        if "available_state_schema" in descriptor:
            fields = descriptor["available_state_schema"]
        elif "semantic" in descriptor:
            fields = [descriptor]
        else:
            raise ConfigError(
                "UE observation descriptor is missing available_state_schema",
                code="ROBOT_OBSERVATION_DESCRIPTOR_INVALID",
                path="available_state_schema",
            )
    else:
        fields = descriptor
    if not isinstance(fields, list):
        raise ConfigError(
            "UE available_state_schema must be an array",
            code="ROBOT_OBSERVATION_DESCRIPTOR_INVALID",
            path="available_state_schema",
        )
    return fields


def _index_descriptor_fields(
    descriptor: Mapping[str, Any] | list[Mapping[str, Any]],
    key: str,
) -> dict[str, Mapping[str, Any]]:
    if isinstance(descriptor, Mapping):
        fields = descriptor.get(key)
        if not isinstance(fields, list):
            raise ConfigError(
                f"UE descriptor is missing {key}",
                code="ROBOT_OBSERVATION_DESCRIPTOR_INVALID",
                path=key,
            )
    elif key == "available_state_schema":
        fields = descriptor
    else:
        raise ConfigError(
            f"UE descriptor is missing {key}",
            code="ROBOT_OBSERVATION_DESCRIPTOR_INVALID",
            path=key,
        )
    indexed: dict[str, Mapping[str, Any]] = {}
    for index, field in enumerate(fields):
        if not isinstance(field, Mapping) or not isinstance(field.get("name"), str):
            raise ConfigError(
                "UE field descriptor must contain a string name",
                code="ROBOT_OBSERVATION_DESCRIPTOR_INVALID",
                path=f"{key}[{index}].name",
            )
        indexed[str(field["name"])] = field
    return indexed


def _validate_declared_fields(
    expected_fields: tuple[Mapping[str, Any], ...],
    available_fields: Mapping[str, Mapping[str, Any]],
    kind: str,
) -> None:
    for expected in expected_fields:
        name = cast(str, expected["name"])
        actual = available_fields.get(name)
        if actual is None:
            raise ConfigError(
                f"Robot {kind} field '{name}' is absent from the UE descriptor",
                code="ROBOT_SHAPE_AGREEMENT_MISMATCH",
                path=f"{kind}.{name}",
            )
        for key in ("dtype", "shape", "unit", "frame", "semantic", "source"):
            actual_value = actual.get(key)
            if key == "shape":
                actual_value = _validate_shape(actual_value, f"{kind}.{name}.shape")
            if actual_value != expected.get(key):
                raise ConfigError(
                    f"Robot {kind} field '{name}' has {key}={actual_value!r}; "
                    f"Python declared {expected.get(key)!r}",
                    code="ROBOT_SHAPE_AGREEMENT_MISMATCH",
                    path=f"{kind}.{name}.{key}",
                )
        expected_extensions = cast(Mapping[str, Any], expected.get("extensions", {}))
        actual_extensions = cast(Mapping[str, Any], actual.get("extensions", {}))
        for key in ("body_index", "body_name", "joint_index", "joint_name", "observation_type"):
            if key in expected_extensions and actual_extensions.get(key) != expected_extensions[key]:
                raise ConfigError(
                    f"Robot {kind} field '{name}' has extension {key}={actual_extensions.get(key)!r}; "
                    f"Python declared {expected_extensions[key]!r}",
                    code="ROBOT_SHAPE_AGREEMENT_MISMATCH",
                    path=f"{kind}.{name}.extensions.{key}",
                )


def _validate_shape(value: Any, path: str) -> tuple[int, ...]:
    if not isinstance(value, (list, tuple)):
        raise ConfigError(
            "UE observation shape must be an array",
            code="ROBOT_OBSERVATION_DESCRIPTOR_INVALID",
            path=path,
        )
    shape = tuple(value)
    if any(not isinstance(dimension, int) or isinstance(dimension, bool) or dimension < 1 for dimension in shape):
        raise ConfigError(
            "UE observation shape dimensions must be positive integers",
            code="ROBOT_OBSERVATION_DESCRIPTOR_INVALID",
            path=path,
        )
    return shape


__all__ = [
    "ObservationShapeTable",
    "ROBOT_CONTACT_OBSERVATION_FRAME",
    "ROBOT_JOINT_OBSERVATION_FRAME",
    "ROBOT_OBSERVATION_FRAME",
    "ROBOT_OBSERVATION_SOURCE",
    "robot_observation_schema",
    "validate_robot_shape_agreement",
    "validate_robot_observation_groups",
    "validate_robot_observation_schema",
]
