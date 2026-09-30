"""Test-owned UE descriptor projections used by offline RobotSpec tests."""

from __future__ import annotations

from uerl.core.direct.robot_observation import ObservationShapeTable


def scalar_robot_observation_shapes() -> ObservationShapeTable:
    """Return the scalar-only descriptor used by small manager fixtures."""

    return ObservationShapeTable.from_descriptor(
        {
            "available_state_schema": [
                {
                    "name": "robot.joint.hip.joint_position",
                    "source": "uerl.robot",
                    "semantic": "joint_position",
                    "shape": [],
                },
                {
                    "name": "robot.joint.knee.joint_position",
                    "source": "uerl.robot",
                    "semantic": "joint_position",
                    "shape": [],
                },
                {
                    "name": "robot.joint.hip.joint_velocity",
                    "source": "uerl.robot",
                    "semantic": "joint_velocity",
                    "shape": [],
                },
                {
                    "name": "robot.joint.knee.joint_velocity",
                    "source": "uerl.robot",
                    "semantic": "joint_velocity",
                    "shape": [],
                },
            ]
        }
    )


def generic_robot_observation_shapes() -> ObservationShapeTable:
    """Return the UE Generic Robot semantic shapes for PhantomX fixtures."""

    return ObservationShapeTable.from_descriptor(
        {
            "available_state_schema": [
                {"source": "uerl.robot", "semantic": "joint_position", "shape": []},
                {"source": "uerl.robot", "semantic": "joint_velocity", "shape": []},
                {"source": "uerl.robot", "semantic": "body_pose", "shape": [7]},
                {"source": "uerl.robot", "semantic": "body_linear_velocity", "shape": [3]},
                {"source": "uerl.robot", "semantic": "body_angular_velocity", "shape": [3]},
                {"source": "uerl.robot", "semantic": "ground_clearance", "shape": []},
                {"source": "uerl.robot", "semantic": "contact", "shape": []},
                {"source": "uerl.robot", "semantic": "contact_force", "shape": []},
                {"source": "uerl.robot", "semantic": "terrain_height", "shape": [35]},
            ]
        }
    )
