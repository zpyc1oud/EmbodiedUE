"""Verify Describe-time Robot schema agreement before Initialize commit."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

import pytest
import torch

from uerl import (
    ConfigError,
    DirectTaskConfig,
    InitialState,
    ObservationShapeTable,
    PhysicalCommandBatch,
    PostResetState,
    RobotSpec,
    SessionSchema,
    TransitionState,
    UERLDirectEnv,
    robot_observation_schema,
)
from uerl.core.config.robot import ObsType, RobotTopology, merge_robot_spec, parse_robot_config
from uerl.tasks.cartpole import create_cartpole_task


def _robot_spec() -> RobotSpec:
    config = parse_robot_config(
        """
actuators:
  - joint: cart
    stiffness: 0.0
    damping: 1.0
    effort_limit: 100.0
    default_pos: 0.0
    action_scale: 1.0
observations:
  - type: joint_position
    joint: cart
reset:
  distributions: []
"""
    )
    topology = RobotTopology.from_response(
        {
            "asset_path": "/Game/Test/SK_Cart",
            "body_names": ["base", "cart"],
            "body_motion_types": ["kinematic", "simulated"],
            "root_body_index": 0,
            "fixed_base": True,
            "joints": [
                {
                    "name": "cart",
                    "parent_body_index": 0,
                    "child_body_index": 1,
                    "degrees_of_freedom": 1,
                    "coordinate": "linear_x",
                    "coordinate_type": "prismatic",
                    "unit": "m",
                    "default_position": 0.0,
                    "lower_limit": -1.0,
                    "upper_limit": 1.0,
                    "child_frame": {
                        "position_metres": [0.0, 0.0, 0.0],
                        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                    "parent_frame": {
                        "position_metres": [0.0, 0.0, 0.0],
                        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
                    },
                }
            ],
        }
    )
    return merge_robot_spec(config, topology)


class _AgreementSession:
    """Invoke the production bind callback with a reflected but inconsistent descriptor."""

    num_slots = 1

    def __init__(self) -> None:
        spec = _robot_spec()
        shapes = ObservationShapeTable({ObsType.JOINT_POSITION: ()})
        state = [dict(field) for field in robot_observation_schema(spec, shapes)]
        state[0]["shape"] = []
        state[0]["unit"] = "rad"
        self.descriptor: dict[str, Any] = {
            "available_state_schema": state,
            "available_action_schema": [],
        }
        self.close_calls = 0
        self.initialize_calls = 0
        self.ready_calls = 0
        self.step_calls = 0

    def initialize_with_robot_spec(
        self,
        schema: SessionSchema,
        bind_robot_spec: Callable[[RobotSpec, ObservationShapeTable], SessionSchema],
    ) -> InitialState:
        del schema
        self.initialize_calls += 1
        spec = _robot_spec()
        shapes = ObservationShapeTable.from_descriptor(self.descriptor)
        return_value = bind_robot_spec(spec, shapes)
        del return_value
        raise AssertionError("shape agreement should reject before Initialize returns")

    def acknowledge_ready(self) -> None:
        self.ready_calls += 1

    def step(self, command: PhysicalCommandBatch, step_decimation: int | None = None) -> TransitionState:
        del command, step_decimation
        self.step_calls += 1
        raise AssertionError("Step must not run after shape agreement failure")

    def reset(
        self,
        reset_mask: torch.Tensor,
        terrain_levels: torch.Tensor | None = None,
    ) -> PostResetState:
        del reset_mask, terrain_levels
        raise AssertionError("Reset must not run after shape agreement failure")

    def close(self, reason: str = "session_close") -> None:
        del reason
        self.close_calls += 1


def test_direct_env_rejects_reflected_robot_field_mismatch_before_ready() -> None:
    session = _AgreementSession()
    task = create_cartpole_task(DirectTaskConfig())

    with pytest.raises(ConfigError) as raised:
        UERLDirectEnv(session, task)  # type: ignore[arg-type]

    assert raised.value.code == "ROBOT_SHAPE_AGREEMENT_MISMATCH"
    assert raised.value.path == "observation.robot.joint.cart.joint_position.unit"
    assert "robot.joint.cart.joint_position" in str(raised.value)
    assert "unit" in str(raised.value)
    assert session.initialize_calls == 1
    assert session.ready_calls == 0
    assert session.step_calls == 0
    assert session.close_calls == 1
