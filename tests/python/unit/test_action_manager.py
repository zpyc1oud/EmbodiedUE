"""Verify ActionManager two-phase process/applied and action plan compile."""

from __future__ import annotations

import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec, RobotTopology, merge_robot_spec, parse_robot_config
from uerl.core.direct.types import PhysicalCommandBatch
from uerl.core.mdp.executor import ActionPlanExecutor
from uerl.core.mdp.managers import ActionManager
from uerl.core.mdp.terms import ActionCfg, ActionTermCfg


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
    stiffness: 0.0
    damping: 0.1
    effort_limit: 1.0
    default_pos: -0.3
    action_scale: 0.5
observations:
  - type: joint_position
    joint: hip
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


def test_ac_py_unit_actmgr_001_process_caches_for_applied() -> None:
    """AC_PY_UNIT_ACTMGR_001: process evaluates the plan; applied returns the cache."""

    spec = _two_joint_spec()
    manager = ActionManager(
        ActionCfg(
            terms={
                "hip": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names="hip"),
                    scale=0.2,
                    use_default_offset=True,
                )
            }
        ),
        spec,
    )
    actions = torch.tensor([[0.5]])
    commands = manager.process(actions)
    assert torch.allclose(commands["robot.actuator.hip"], torch.tensor([[0.15 + 0.2 * 0.5]]))
    assert manager.applied() is commands


def test_ac_py_unit_actmgr_002_applied_does_not_reevaluate() -> None:
    """AC_PY_UNIT_ACTMGR_002: three applied() calls leave plan execute count at 1."""

    spec = _two_joint_spec()
    manager = ActionManager(
        ActionCfg(terms={"hip": ActionTermCfg(entity=RobotEntityCfg(joint_names="hip"), scale=1.0)}),
        spec,
    )
    execute_count = 0
    original = manager._executor.execute

    def counting_execute(policy_actions: torch.Tensor) -> PhysicalCommandBatch:
        nonlocal execute_count
        execute_count += 1
        return original(policy_actions)

    manager._executor.execute = counting_execute  # type: ignore[method-assign]
    manager.process(torch.tensor([[0.25]]))
    assert execute_count == 1
    manager.applied()
    manager.applied()
    manager.applied()
    assert execute_count == 1


def test_ac_py_unit_actmgr_003_previous_action_is_raw() -> None:
    """AC_PY_UNIT_ACTMGR_003: previous_action keeps out-of-range raw; command is clipped."""

    spec = _two_joint_spec()
    manager = ActionManager(
        ActionCfg(
            terms={
                "hip": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names="hip"),
                    clip=(-1.0, 1.0),
                    scale=1.0,
                    use_default_offset=False,
                )
            }
        ),
        spec,
    )
    raw = torch.tensor([[2.5]])
    commands = manager.process(raw)
    assert torch.equal(manager.previous_action, raw)
    assert torch.allclose(commands["robot.actuator.hip"], torch.tensor([[1.0]]))


def test_ac_py_unit_actmgr_004_exported_plan_matches_manager_commands() -> None:
    """AC_PY_UNIT_ACTMGR_004: the exposed plan reproduces the live command."""

    spec = _two_joint_spec()
    manager = ActionManager(
        ActionCfg(
            terms={
                "hip": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names="hip"),
                    scale=2.0,
                    use_default_offset=False,
                )
            }
        ),
        spec,
    )
    action = torch.tensor([[0.4]])
    live = manager.process(action)
    exported = ActionPlanExecutor(manager.plan).execute(action)
    assert manager.plan.policy_width == 1
    assert manager.plan.command_fields == tuple(live.values)
    for name in live.values:
        torch.testing.assert_close(exported[name], live[name])
    torch.testing.assert_close(live["robot.actuator.hip"], torch.tensor([[0.8]]))


def test_ac_py_unit_actmgr_005_position_and_effort_coexist() -> None:
    """AC_PY_UNIT_ACTMGR_005: position + effort terms produce distinct command fields."""

    spec = _two_joint_spec()
    manager = ActionManager(
        ActionCfg(
            terms={
                "hip": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names="hip"),
                    target_mode="position",
                    scale=0.2,
                    use_default_offset=True,
                ),
                "knee": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names="knee"),
                    target_mode="effort",
                    scale=0.5,
                    use_default_offset=False,
                ),
            }
        ),
        spec,
    )
    assert manager.policy_width == 2
    commands = manager.process(torch.tensor([[0.5, -0.4]]))
    assert set(commands.values) == {"robot.actuator.hip", "robot.actuator.effort.knee"}
    assert torch.allclose(commands["robot.actuator.hip"], torch.tensor([[0.15 + 0.2 * 0.5]]))
    assert torch.allclose(commands["robot.actuator.effort.knee"], torch.tensor([[0.5 * -0.4]]))


def test_action_plan_has_no_offset_default_op() -> None:
    """Compiled plan expands default pose into offset.bias; no offset_default op."""

    spec = _two_joint_spec()
    manager = ActionManager(
        ActionCfg(
            terms={
                "hip": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names="hip"),
                    use_default_offset=True,
                )
            }
        ),
        spec,
    )
    op_names = {op.op for op in manager.plan.ops}
    assert "offset_default" not in op_names
    assert "offset" in op_names
    offset_ops = [op for op in manager.plan.ops if op.op == "offset"]
    assert offset_ops[0].params["bias"] == (0.15,)


def test_action_manager_distinct_slots_clip_before_joint_scale_and_offset() -> None:
    """Opposite saturated actions distinguish rows and hip/knee actuator order."""

    manager = ActionManager(
        ActionCfg(
            terms={
                "joints": ActionTermCfg(
                    entity=RobotEntityCfg(joint_names=("knee", "hip"), preserve_order=True),
                    clip=(-1.0, 1.0),
                    scale=(0.5, 0.2),
                    use_default_offset=True,
                ),
            }
        ),
        _two_joint_spec(),
    )
    raw = torch.tensor([[2.0, -3.0], [-0.5, 0.25]])
    commands = manager.process(raw)
    # knee: -0.3 + 0.5 * [1, -0.5]; hip: 0.15 + 0.2 * [-1, 0.25].
    assert tuple(commands.values) == ("robot.actuator.joints",)
    torch.testing.assert_close(commands["robot.actuator.joints"], torch.tensor([[0.2, -0.05], [-0.55, 0.2]]))
    assert torch.equal(manager.previous_action, raw)
