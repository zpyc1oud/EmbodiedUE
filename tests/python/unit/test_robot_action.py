"""Verify indexed RobotSpec action resolution and column metadata."""

from __future__ import annotations

import pytest
import torch

from uerl import (
    ROBOT_ACTUATOR_TARGET_FIELD,
    ActuatorSpec,
    ConstraintFrame,
    JointTopology,
    RobotSpec,
    RobotTopology,
    resolve_robot_actions,
    robot_actuator_action_schema,
)

_FRAME = ConstraintFrame((0.0, 0.0, 0.0), (0.0, 0.0, 0.0, 1.0))


def _robot_spec() -> RobotSpec:
    topology = RobotTopology(
        body_names=("base", "position", "effort"),
        body_motion_types=("simulated", "simulated", "simulated"),
        root_body_index=0,
        fixed_base=False,
        joints=(
            JointTopology("position_joint", 0, 1, 1, "twist", "revolute", "rad", -3.14, 3.14, _FRAME, _FRAME, 0.0),
            JointTopology("effort_joint", 0, 2, 1, "linear_x", "prismatic", "m", None, None, _FRAME, _FRAME, 0.0),
        ),
    )
    return RobotSpec(
        actuators=(
            ActuatorSpec("position_joint", 0, 1, "revolute", "twist", "position", "rad", 10.0, 1.0, 20.0, 0.2, 0.5, 0),
            ActuatorSpec("effort_joint", 1, 2, "prismatic", "linear_x", "effort", "N", 0.0, 1.0, 5.0, 0.0, 3.0, 1),
        ),
        observations=(),
        reset=(),
        body_names=("base", "position", "effort"),
        topology=topology,
    )


def test_action_schema_is_one_target_vector_with_column_metadata() -> None:
    descriptor = robot_actuator_action_schema(_robot_spec())[0]

    assert descriptor["name"] == ROBOT_ACTUATOR_TARGET_FIELD
    assert descriptor["shape"] == (2,)
    assert descriptor["unit"] == "mixed"
    assert descriptor["extensions"]["columns"] == (
        {
            "index": 0,
            "joint": "position_joint",
            "joint_index": 0,
            "coordinate_type": "revolute",
            "unit": "rad",
            "target_mode": "position",
        },
        {
            "index": 1,
            "joint": "effort_joint",
            "joint_index": 1,
            "coordinate_type": "prismatic",
            "unit": "N",
            "target_mode": "effort",
        },
    )


def test_position_and_effort_actions_share_one_target_vector() -> None:
    command = resolve_robot_actions(_robot_spec(), torch.tensor([[2.0, -1.0], [-2.0, 0.5]]))

    assert tuple(command.values) == (ROBOT_ACTUATOR_TARGET_FIELD,)
    assert torch.allclose(command[ROBOT_ACTUATOR_TARGET_FIELD], torch.tensor([[1.2, -3.0], [-0.8, 1.5]]))


def test_action_resolution_preserves_device_and_returns_float32() -> None:
    command = resolve_robot_actions(_robot_spec(), torch.ones(4, 2, dtype=torch.float64))

    assert command[ROBOT_ACTUATOR_TARGET_FIELD].dtype is torch.float32
    assert command[ROBOT_ACTUATOR_TARGET_FIELD].device.type == "cpu"
    assert command[ROBOT_ACTUATOR_TARGET_FIELD].shape == (4, 2)


@pytest.mark.parametrize("shape", [(2,), (2, 3)])
def test_action_resolution_rejects_wrong_width_or_rank(shape: tuple[int, ...]) -> None:
    with pytest.raises(ValueError, match="shape"):
        resolve_robot_actions(_robot_spec(), torch.zeros(shape))
