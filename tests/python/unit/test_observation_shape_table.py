"""Verify descriptor-owned Robot observation shape negotiation."""

from __future__ import annotations

import pytest

from uerl.core.config.robot import ConstraintFrame, ObservationSpec, ObsType, RobotSpec, RobotTopology
from uerl.core.direct.robot_observation import ObservationShapeTable, robot_observation_schema
from uerl.errors import ConfigError

_FRAME = ConstraintFrame((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))


def _terrain_spec() -> RobotSpec:
    topology = RobotTopology(
        body_names=("base",),
        body_motion_types=("kinematic",),
        root_body_index=0,
        fixed_base=True,
        joints=(),
    )
    return RobotSpec(
        actuators=(),
        observations=(ObservationSpec(ObsType.TERRAIN_HEIGHT, "base", "body", 0, None, "m", 0),),
        reset=(),
        body_names=("base",),
        topology=topology,
    )


def test_shape_table_uses_the_initialize_descriptor_for_schema_widths() -> None:
    descriptor = {
        "available_state_schema": [
            {"name": "environment.height", "source": "uerl.environment", "semantic": "height", "shape": [2]},
            {
                "name": "robot.body.base.terrain_height",
                "source": "uerl.robot",
                "semantic": "terrain_height",
                "shape": [11],
            },
        ]
    }

    shapes = ObservationShapeTable.from_descriptor(descriptor)
    schema = robot_observation_schema(_terrain_spec(), shapes)

    assert shapes.shape_for(ObsType.TERRAIN_HEIGHT) == (11,)
    assert schema[0]["shape"] == (11,)


def test_shape_table_rejects_conflicting_publications_with_a_concrete_path() -> None:
    with pytest.raises(ConfigError) as raised:
        ObservationShapeTable.from_descriptor(
            {
                "available_state_schema": [
                    {"source": "uerl.robot", "semantic": "terrain_height", "shape": [11]},
                    {"source": "uerl.robot", "semantic": "terrain_height", "shape": [13]},
                ]
            }
        )

    assert raised.value.code == "ROBOT_OBSERVATION_SHAPE_CONFLICT"
    assert raised.value.path == "available_state_schema[1].shape"


def test_shape_table_reports_missing_robot_semantics_before_schema_commit() -> None:
    shapes = ObservationShapeTable.from_descriptor(
        {"available_state_schema": [{"source": "uerl.robot", "semantic": "joint_position", "shape": []}]}
    )

    with pytest.raises(ConfigError) as raised:
        shapes.shape_for(ObsType.TERRAIN_HEIGHT)

    assert raised.value.code == "ROBOT_OBSERVATION_SHAPE_MISSING"
    assert raised.value.path == "available_state_schema.terrain_height"
