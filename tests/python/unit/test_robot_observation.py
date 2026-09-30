"""Verify indexed RobotSpec observation schema generation."""

from __future__ import annotations

import pytest

from tests.python.unit.robot_shape_fixtures import generic_robot_observation_shapes
from uerl.core.config.robot import ConstraintFrame, JointTopology, ObservationSpec, ObsType, RobotSpec, RobotTopology
from uerl.core.direct.robot_observation import (
    robot_observation_schema,
    validate_robot_observation_groups,
    validate_robot_observation_schema,
)
from uerl.errors import ConfigError

_FRAME = ConstraintFrame((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))


def _topology() -> RobotTopology:
    return RobotTopology(
        body_names=("world", "cart", "pole"),
        body_motion_types=("kinematic", "simulated", "simulated"),
        root_body_index=0,
        fixed_base=True,
        joints=(
            JointTopology("cart", 0, 1, 1, "linear_x", "prismatic", "m", None, None, _FRAME, _FRAME, 0.0),
            JointTopology("pole", 1, 2, 1, "twist", "revolute", "rad", -3.14, 3.14, _FRAME, _FRAME, 0.0),
        ),
    )


def _spec(*observations: ObservationSpec) -> RobotSpec:
    return RobotSpec(
        actuators=(),
        observations=observations,
        reset=(),
        body_names=("world", "cart", "pole"),
        topology=_topology(),
    )


SHAPES = generic_robot_observation_shapes()


def test_ac_py_unit_generic_005_observation_schema_preserves_order_topology_indices_units_and_frame() -> None:
    descriptors = robot_observation_schema(
        _spec(
            ObservationSpec(ObsType.JOINT_POSITION, "cart", "joint", 1, 0, "m", 0),
            ObservationSpec(ObsType.JOINT_VELOCITY, "pole", "joint", 2, 1, "rad/s", 1),
            ObservationSpec(ObsType.BODY_POSE, "world", "body", 0, None, "m,quat_xyzw", 2),
            ObservationSpec(ObsType.BODY_LINEAR_VELOCITY, "cart", "body", 1, None, "m/s", 3),
            ObservationSpec(ObsType.BODY_ANGULAR_VELOCITY, "pole", "body", 2, None, "rad/s", 4),
        ),
        SHAPES,
    )

    assert all("index" not in descriptor for descriptor in descriptors)
    assert [descriptor["name"] for descriptor in descriptors] == [
        "robot.joint.cart.joint_position",
        "robot.joint.pole.joint_velocity",
        "robot.body.world.body_pose",
        "robot.body.cart.body_linear_velocity",
        "robot.body.pole.body_angular_velocity",
    ]
    assert [descriptor["shape"] for descriptor in descriptors] == [(), (), (7,), (3,), (3,)]
    assert [descriptor["unit"] for descriptor in descriptors] == ["m", "rad/s", "m,quat_xyzw", "m/s", "rad/s"]
    assert [descriptor["frame"] for descriptor in descriptors] == [
        "constraint",
        "constraint",
        "slot/local",
        "slot/local",
        "slot/local",
    ]
    assert descriptors[0]["extensions"] == {
        "target": "cart",
        "target_kind": "joint",
        "observation_type": "joint_position",
        "body_index": 1,
        "body_name": "cart",
        "joint_index": 0,
        "joint_name": "cart",
        "coordinate": "linear_x",
        "coordinate_type": "prismatic",
    }
    assert descriptors[2]["extensions"] == {
        "target": "world",
        "target_kind": "body",
        "observation_type": "body_pose",
        "body_index": 0,
        "body_name": "world",
    }


def test_contact_observation_has_fraction_descriptor() -> None:
    descriptor = robot_observation_schema(
        _spec(ObservationSpec(ObsType.CONTACT, "pole", "body", 2, None, "fraction", 0)),
        SHAPES,
    )[
        0
    ]

    assert descriptor["name"] == "robot.body.pole.contact"
    assert descriptor["dtype"] == "float32"
    assert descriptor["shape"] == ()
    assert descriptor["unit"] == "fraction"
    assert descriptor["frame"] == "coordinate-free"


def test_contact_force_observation_has_newton_descriptor() -> None:
    descriptor = robot_observation_schema(
        _spec(ObservationSpec(ObsType.CONTACT_FORCE, "pole", "body", 2, None, "N", 0)),
        SHAPES,
    )[0]

    assert descriptor["name"] == "robot.body.pole.contact_force"
    assert descriptor["dtype"] == "float32"
    assert descriptor["shape"] == ()
    assert descriptor["unit"] == "N"
    assert descriptor["frame"] == "coordinate-free"
    assert descriptor["semantic"] == "contact_force"


def test_observation_requires_a_resolved_index() -> None:
    with pytest.raises(ConfigError, match="resolved topology index"):
        robot_observation_schema(
            _spec(ObservationSpec(ObsType.JOINT_POSITION, "cart", "joint", None, None, "m", 0)),
            SHAPES,
        )


def test_ac_py_unit_generic_006_task_groups_must_preserve_robot_config_order() -> None:
    spec = _spec(
        ObservationSpec(ObsType.JOINT_POSITION, "cart", "joint", 1, 0, "m", 0),
        ObservationSpec(ObsType.JOINT_VELOCITY, "pole", "joint", 2, 1, "rad/s", 1),
    )
    first, second = (descriptor["name"] for descriptor in robot_observation_schema(spec, SHAPES))

    validate_robot_observation_groups(
        spec,
        {
            "policy": ("environment.height", first, "task.phase", second),
            "terrain": ("environment.height",),
        },
        SHAPES,
    )
    validate_robot_observation_groups(spec, {"policy": (first,)}, SHAPES)

    for invalid_projection in ((second, first), (first, "robot.unconfigured", second)):
        with pytest.raises(ConfigError) as raised:
            validate_robot_observation_groups(spec, {"policy": invalid_projection}, SHAPES)

        assert raised.value.code == "ROBOT_OBSERVATION_PROJECTION_MISMATCH"
        assert raised.value.path == "observation_groups.policy"


def test_ac_py_unit_generic_007_session_schema_must_preserve_the_robot_config_column_order() -> None:
    spec = _spec(
        ObservationSpec(ObsType.JOINT_POSITION, "cart", "joint", 1, 0, "m", 0),
        ObservationSpec(ObsType.JOINT_VELOCITY, "pole", "joint", 2, 1, "rad/s", 1),
    )
    reversed_schema = tuple(reversed(robot_observation_schema(spec, SHAPES)))

    with pytest.raises(ConfigError) as raised:
        validate_robot_observation_schema(spec, reversed_schema, SHAPES)

    assert raised.value.code == "ROBOT_OBSERVATION_PROJECTION_MISMATCH"
    assert raised.value.path == "state_requirements"


def test_ac_py_unit_fieldname_001_field_names_use_part_names_and_are_unique() -> None:
    descriptors = robot_observation_schema(
        _spec(
            ObservationSpec(ObsType.JOINT_POSITION, "cart", "joint", 1, 0, "m", 0),
            ObservationSpec(ObsType.JOINT_VELOCITY, "pole", "joint", 2, 1, "rad/s", 1),
            ObservationSpec(ObsType.BODY_POSE, "world", "body", 0, None, "m,quat_xyzw", 2),
        ),
        SHAPES,
    )
    names = [descriptor["name"] for descriptor in descriptors]
    assert names == [
        "robot.joint.cart.joint_position",
        "robot.joint.pole.joint_velocity",
        "robot.body.world.body_pose",
    ]
    assert len(names) == len(set(names))
    assert descriptors[0]["extensions"]["joint_name"] == "cart"
    assert "joint_index" in descriptors[0]["extensions"]
    assert "body_index" in descriptors[0]["extensions"]
    assert "body_name" in descriptors[0]["extensions"]


def test_ac_py_unit_fieldname_002_dotted_part_names_are_rejected() -> None:
    topology = _topology()
    dotted = JointTopology(
        "cart.slider", 0, 1, 1, "linear_x", "prismatic", "m", None, None, _FRAME, _FRAME, 0.0
    )
    bad_topology = RobotTopology(
        body_names=topology.body_names,
        body_motion_types=topology.body_motion_types,
        root_body_index=topology.root_body_index,
        fixed_base=topology.fixed_base,
        joints=(dotted, topology.joints[1]),
    )
    spec = RobotSpec(
        actuators=(),
        observations=(ObservationSpec(ObsType.JOINT_POSITION, "cart.slider", "joint", 1, 0, "m", 0),),
        reset=(),
        body_names=bad_topology.body_names,
        topology=bad_topology,
    )
    with pytest.raises(ConfigError, match="cart.slider") as raised:
        robot_observation_schema(spec, SHAPES)
    assert raised.value.code == "INVALID_PART_NAME"
