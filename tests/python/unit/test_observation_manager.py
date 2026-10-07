"""Verify ObservationManager scale-before-clip, group ordering, and compile errors."""

from __future__ import annotations

from collections.abc import Callable
from typing import cast

import pytest
import torch

from tests.python.unit.robot_shape_fixtures import (
    generic_robot_observation_shapes,
    scalar_robot_observation_shapes,
)
from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec, RobotTopology, merge_robot_spec, parse_robot_config
from uerl.core.mdp.compile_observation import compile_observation_plan
from uerl.core.mdp.executor import PlanInputs
from uerl.core.mdp.managers import ObservationManager
from uerl.core.mdp.terms import ObservationCfg, ObsGroupCfg, ObsTermCfg
from uerl.errors import ConfigError
from uerl.tasks.phantomx.config import PHANTOMX_FEET, PHANTOMX_JOINTS, load_phantomx_training_config
from uerl.tasks.phantomx.observation_plan import (
    PHANTOMX_JOINT_DEFAULTS,
    build_phantomx_observation_plan,
)

SCALAR_SHAPES = scalar_robot_observation_shapes()
PHANTOMX_SHAPES = generic_robot_observation_shapes()


def _two_joint_spec() -> RobotSpec:
    config = parse_robot_config(
        """
actuators:
  - joint: hip
    stiffness: 1.0
    damping: 0.1
    effort_limit: 1.0
    default_pos: 0.15
    action_scale: 0.2
  - joint: knee
    stiffness: 1.0
    damping: 0.1
    effort_limit: 1.0
    default_pos: -0.3
    action_scale: 0.5
observations:
  - type: joint_position
    joint: hip
  - type: joint_position
    joint: knee
  - type: joint_velocity
    joint: hip
  - type: joint_velocity
    joint: knee
reset:
  distributions:
    - type: joint_position
      joint: hip
      distribution:
        type: constant
        lower: 0.0
        upper: 0.0
        stream_id: reset.hip
"""
    )
    frame = {
        "position_metres": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    topology = RobotTopology.from_response(
        {
            "asset_path": "/Game/Test/SK_Test",
            "body_names": ["base", "thigh", "shin"],
            "body_motion_types": ["simulated", "simulated", "simulated"],
            "root_body_index": 0,
            "fixed_base": False,
            "joints": [
                {
                    "name": "hip",
                    "parent_body_index": 0,
                    "child_body_index": 1,
                    "degrees_of_freedom": 1,
                    "coordinate": "twist",
                    "coordinate_type": "revolute",
                    "unit": "rad",
                    "default_position": 0.0,
                    "lower_limit": -1.0,
                    "upper_limit": 1.0,
                    "child_frame": frame,
                    "parent_frame": frame,
                },
                {
                    "name": "knee",
                    "parent_body_index": 1,
                    "child_body_index": 2,
                    "degrees_of_freedom": 1,
                    "coordinate": "twist",
                    "coordinate_type": "revolute",
                    "unit": "rad",
                    "default_position": 0.0,
                    "lower_limit": -1.0,
                    "upper_limit": 1.0,
                    "child_frame": frame,
                    "parent_frame": frame,
                },
            ],
        }
    )
    return merge_robot_spec(config, topology)


def _phantomx_spec() -> RobotSpec:
    training = load_phantomx_training_config()
    body_names = ["base_link", *PHANTOMX_JOINTS]
    joints: list[dict[str, object]] = []
    frame = {
        "position_metres": [0.0, 0.0, 0.0],
        "rotation_xyzw": [0.0, 0.0, 0.0, 1.0],
    }
    for index, joint in enumerate(PHANTOMX_JOINTS):
        parent_body_index = 0 if index % 3 == 0 else index
        joints.append(
            {
                "name": joint,
                "parent_body_index": parent_body_index,
                "child_body_index": index + 1,
                "degrees_of_freedom": 1,
                "coordinate": "twist",
                "coordinate_type": "revolute",
                "unit": "rad",
                "default_position": 0.0,
                "lower_limit": -3.14,
                "upper_limit": 3.14,
                "child_frame": frame,
                "parent_frame": frame,
            }
        )
    topology = RobotTopology.from_response(
        {
            "asset_path": training.robot_asset_path,
            "body_names": body_names,
            "body_motion_types": ["simulated"] * len(body_names),
            "root_body_index": 0,
            "fixed_base": False,
            "joints": joints,
        }
    )
    return merge_robot_spec(training.robot_config, topology)


def test_ac_py_unit_obsmgr_001_scale_then_clip_order() -> None:
    """AC_PY_UNIT_OBSMGR_001: scale→clip; 0.6 with scale=2 clip[0,1] → 1.0 ≠ reverse 1.2."""

    spec = _two_joint_spec()
    manager = ObservationManager(
        ObservationCfg(
            groups={
                "policy": ObsGroupCfg(
                    terms={
                        "hip": ObsTermCfg(
                            op="select",
                            params={"field": "robot.joint.hip.joint_position"},
                            scale=2.0,
                            clip=(0.0, 1.0),
                        )
                    }
                )
            }
        ),
        spec,
        policy_width=1,
        observation_shapes=SCALAR_SHAPES,
    )
    ops = [op.op for op in manager.plan.ops]
    assert ops == ["select", "scale", "clip"]
    result = manager.compute(
        PlanInputs(
            raw_state={"robot.joint.hip.joint_position": torch.tensor([[0.6]])},
        )
    )["policy"]
    assert torch.allclose(result, torch.tensor([[1.0]]))
    # Reverse order would be clip then scale → 1.2
    assert not torch.allclose(result, torch.tensor([[1.2]]))


def test_ac_py_unit_obsmgr_002_entity_expands_in_resolve_order() -> None:
    """AC_PY_UNIT_OBSMGR_002: entity → selects + concat in entity resolve order."""

    spec = _two_joint_spec()
    entity = RobotEntityCfg(joint_names=("knee", "hip"), preserve_order=True)
    plan = compile_observation_plan(
        ObservationCfg(
            groups={
                "policy": ObsGroupCfg(
                    terms={
                        "joints": ObsTermCfg(
                            op="select",
                            params={"entity": entity, "field": "joint_position"},
                        )
                    }
                )
            }
        ),
        spec,
        policy_width=2,
        observation_shapes=SCALAR_SHAPES,
    )
    selects = [op for op in plan.ops if op.op == "select"]
    assert [op.params["field"] for op in selects] == [
        "robot.joint.knee.joint_position",
        "robot.joint.hip.joint_position",
    ]
    assert any(op.op == "concat" for op in plan.ops)
    manager = ObservationManager(
        ObservationCfg(
            groups={
                "policy": ObsGroupCfg(
                    terms={
                        "joints": ObsTermCfg(
                            op="select",
                            params={
                                "entity": RobotEntityCfg(
                                    joint_names=("knee", "hip"),
                                    preserve_order=True,
                                ),
                                "field": "joint_position",
                            },
                        )
                    }
                )
            }
        ),
        spec,
        policy_width=2,
        observation_shapes=SCALAR_SHAPES,
    )
    out = manager.compute(
        PlanInputs(
            raw_state={
                "robot.joint.hip.joint_position": torch.tensor([[0.1]]),
                "robot.joint.knee.joint_position": torch.tensor([[0.2]]),
            }
        )
    )["policy"]
    assert torch.allclose(out, torch.tensor([[0.2, 0.1]]))


def test_ac_py_unit_obsmgr_004_policy_and_critic_groups() -> None:
    """AC_PY_UNIT_OBSMGR_004: policy and critic coexist with independent widths."""

    spec = _two_joint_spec()
    manager = ObservationManager(
        ObservationCfg(
            groups={
                "policy": ObsGroupCfg(
                    terms={
                        "hip": ObsTermCfg(
                            op="select",
                            params={"field": "robot.joint.hip.joint_position"},
                        )
                    }
                ),
                "critic": ObsGroupCfg(
                    terms={
                        "hip_v": ObsTermCfg(
                            op="select",
                            params={"field": "robot.joint.hip.joint_velocity"},
                        ),
                        "knee_v": ObsTermCfg(
                            op="select",
                            params={"field": "robot.joint.knee.joint_velocity"},
                        ),
                    }
                ),
            }
        ),
        spec,
        policy_width=1,
        observation_shapes=SCALAR_SHAPES,
    )
    assert manager.group_widths == {"policy": 1, "critic": 2}
    groups = manager.compute(
        PlanInputs(
            raw_state={
                "robot.joint.hip.joint_position": torch.tensor([[1.0]]),
                "robot.joint.hip.joint_velocity": torch.tensor([[2.0]]),
                "robot.joint.knee.joint_velocity": torch.tensor([[3.0]]),
            }
        )
    )
    assert groups["policy"].shape == (1, 1)
    assert groups["critic"].shape == (1, 2)
    assert torch.allclose(groups["critic"], torch.tensor([[2.0, 3.0]]))


def test_ac_py_unit_obsmgr_005_reset_is_noop_without_history() -> None:
    """AC_PY_UNIT_OBSMGR_005 (superseded by ticket 10): reset is a documented no-op."""

    spec = _two_joint_spec()
    manager = ObservationManager(
        ObservationCfg(
            groups={
                "policy": ObsGroupCfg(
                    terms={
                        "hip": ObsTermCfg(
                            op="select",
                            params={"field": "robot.joint.hip.joint_position"},
                        )
                    }
                )
            }
        ),
        spec,
        policy_width=1,
        observation_shapes=SCALAR_SHAPES,
    )
    manager.reset(torch.tensor([True, False]))
    manager.reset(None)


@pytest.mark.parametrize(
    ("build_cfg", "match"),
    [
        (
            lambda: ObservationCfg(
                groups={"policy": ObsGroupCfg(terms={"x": ObsTermCfg(op="not_an_operator", params={})})}
            ),
            "unknown observation operator",
        ),
        (
            lambda: ObservationCfg(
                groups={
                    "policy": ObsGroupCfg(
                        terms={
                            "x": ObsTermCfg(
                                op="select",
                                params={
                                    "entity": RobotEntityCfg(joint_names="missing_joint"),
                                    "field": "joint_position",
                                },
                            )
                        }
                    )
                }
            ),
            "matched no names",
        ),
        (
            lambda: ObservationCfg(
                groups={
                    "policy": ObsGroupCfg(
                        terms={
                            "hip": ObsTermCfg(
                                op="select",
                                params={"field": "robot.joint.hip.joint_position"},
                            )
                        },
                        members=("missing_term",),
                    )
                }
            ),
            "unknown term",
        ),
        (
            lambda: ObservationCfg(groups={"policy": ObsGroupCfg(terms={"empty": ObsTermCfg(op="concat", inputs=())})}),
            "width derivation failed",
        ),
    ],
)
def test_ac_py_unit_obsmgr_006_compile_errors(
    build_cfg: object,
    match: str,
) -> None:
    """AC_PY_UNIT_OBSMGR_006: missing op / entity miss / bad member / width fail."""

    spec = _two_joint_spec()
    cfg_factory = cast(Callable[[], ObservationCfg], build_cfg)
    with pytest.raises(ConfigError, match=match):
        compile_observation_plan(cfg_factory(), spec, policy_width=1, observation_shapes=SCALAR_SHAPES)


def _phantomx_observation_cfg() -> ObservationCfg:
    """Term declaration reverse-engineered from ticket 11's plan structure."""

    terms: dict[str, ObsTermCfg] = {
        "pose": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.body_pose"},
        ),
        "lin_vel_w": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.body_linear_velocity"},
        ),
        "ang_vel_w": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.body_angular_velocity"},
        ),
        "quat": ObsTermCfg(op="slice", inputs=("pose",), params={"start": 3, "width": 4}),
        "lin_vel_b": ObsTermCfg(op="rotate_inverse", inputs=("quat", "lin_vel_w")),
        "ang_vel_b": ObsTermCfg(op="rotate_inverse", inputs=("quat", "ang_vel_w")),
        "gravity_b": ObsTermCfg(
            op="projected_gravity",
            inputs=("quat",),
            params={"gravity": (0.0, 0.0, -1.0)},
        ),
        "velocity_cmd": ObsTermCfg(
            op="command",
            params={"channel": "velocity", "width": 3},
        ),
        "terrain": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.terrain_height"},
        ),
        "clearance": ObsTermCfg(
            op="select",
            params={"field": "robot.body.base_link.ground_clearance"},
        ),
    }
    contact_slots: list[str] = []
    for foot in PHANTOMX_FEET:
        slot = f"contact_{foot}"
        terms[slot] = ObsTermCfg(
            op="select",
            params={"field": f"robot.body.{foot}.contact"},
        )
        contact_slots.append(slot)
    terms["contacts"] = ObsTermCfg(op="concat", inputs=tuple(contact_slots))

    joint_pos_slots: list[str] = []
    joint_vel_slots: list[str] = []
    for joint in PHANTOMX_JOINTS:
        pos = f"jp_{joint}"
        vel = f"jv_{joint}"
        terms[pos] = ObsTermCfg(
            op="select",
            params={"field": f"robot.joint.{joint}.joint_position"},
        )
        terms[vel] = ObsTermCfg(
            op="select",
            params={"field": f"robot.joint.{joint}.joint_velocity"},
        )
        joint_pos_slots.append(pos)
        joint_vel_slots.append(vel)
    terms["joint_pos"] = ObsTermCfg(op="concat", inputs=tuple(joint_pos_slots))
    terms["joint_pos_rel"] = ObsTermCfg(
        op="joint_pos_rel",
        inputs=("joint_pos",),
        params={"default": PHANTOMX_JOINT_DEFAULTS},
    )
    terms["joint_vel"] = ObsTermCfg(op="concat", inputs=tuple(joint_vel_slots))
    terms["prev_action"] = ObsTermCfg(op="previous_action", params={"width": 18})
    terms["control_frame_dt"] = ObsTermCfg(op="control_frame_dt", params={"scale": 100.0})

    members = (
        "lin_vel_b",
        "ang_vel_b",
        "gravity_b",
        "velocity_cmd",
        "terrain",
        "clearance",
        "contacts",
        "joint_pos_rel",
        "joint_vel",
        "prev_action",
        "control_frame_dt",
    )
    return ObservationCfg(
        groups={"policy": ObsGroupCfg(terms=terms, members=members)},
    )


def test_ticket11_plan_structure_equivalence() -> None:
    """Declaration layer compiles to the same op/width/member structure as ticket 11."""

    expected = build_phantomx_observation_plan(PHANTOMX_SHAPES)
    actual = compile_observation_plan(
        _phantomx_observation_cfg(),
        _phantomx_spec(),
        policy_width=18,
        observation_shapes=PHANTOMX_SHAPES,
    )
    assert [op.op for op in actual.ops] == [op.op for op in expected.ops]
    assert [op.width for op in actual.ops] == [op.width for op in expected.ops]
    assert actual.groups["policy"] == expected.groups["policy"]
    assert actual.group_widths == expected.group_widths
    # Slot names and params must also align for true structural equivalence.
    for got, want in zip(actual.ops, expected.ops, strict=True):
        assert got.op == want.op
        assert got.inputs == want.inputs
        assert got.output == want.output
        assert got.width == want.width
        assert dict(got.params) == dict(want.params)
