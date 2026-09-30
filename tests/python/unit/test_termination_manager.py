"""Verify TerminationManager wraps combine_termination_terms with params."""

from __future__ import annotations

import pytest
import torch

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec, RobotTopology, merge_robot_spec, parse_robot_config
from uerl.core.direct.types import PhysicalCommandBatch, StepContext
from uerl.core.mdp.lib import terminations as term_lib
from uerl.core.mdp.managers import TerminationManager
from uerl.core.mdp.terms import DoneTermCfg, TerminationCfg


def _context(
    *,
    steps: torch.Tensor,
    transition_state: dict[str, torch.Tensor] | None = None,
) -> StepContext:
    rows = steps.shape[0]
    return StepContext(
        raw_state={},
        previous_policy_actions=torch.zeros(rows, 1),
        policy_actions=torch.zeros(rows, 1),
        physical_command=PhysicalCommandBatch({}),
        transition_state=transition_state or {},
        state_valid=torch.ones(rows, dtype=torch.bool),
        slot_fault_code=torch.zeros(rows, dtype=torch.uint16),
        episode_steps=steps,
    )


def _tiny_spec() -> RobotSpec:
    config = parse_robot_config(
        """
actuators:
  - joint: hip
    stiffness: 1.0
    damping: 0.1
    effort_limit: 1.0
    default_pos: 0.0
    action_scale: 1.0
observations:
  - type: joint_position
    joint: hip
  - type: body_pose
    body: base
  - type: contact_force
    body: base
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
            "body_names": ["base", "link"],
            "body_motion_types": ["simulated", "simulated"],
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
                }
            ],
        }
    )
    return merge_robot_spec(config, topology)


def test_ac_py_unit_donemgr_001_timeout_and_failure_masks_per_row() -> None:
    """AC_PY_UNIT_DONEMGR_001: terminated/truncated masks do not cross-contaminate."""

    spec = _tiny_spec()
    manager = TerminationManager(
        TerminationCfg(
            terms={
                "timeout": DoneTermCfg(func=term_lib.time_out, time_out=True, params={"max_steps": 2}),
                "force": DoneTermCfg(
                    func=lambda ctx, **_: ctx.episode_steps.eq(0),
                    time_out=False,
                ),
                "other_fail": DoneTermCfg(
                    func=lambda ctx, **_: ctx.episode_steps.eq(1),
                    time_out=False,
                ),
            }
        ),
        spec,
    )
    result = manager.compute(_context(steps=torch.tensor([0, 1, 2])))

    assert torch.equal(result.terminated, torch.tensor([True, True, False]))
    assert torch.equal(result.truncated, torch.tensor([False, False, True]))
    assert torch.equal(result.reason["force"], torch.tensor([True, False, False]))
    assert torch.equal(result.reason["other_fail"], torch.tensor([False, True, False]))
    assert torch.equal(result.reason["timeout"], torch.tensor([False, False, True]))


def test_ac_py_unit_donemgr_002_entity_resolved_at_assemble() -> None:
    """AC_PY_UNIT_DONEMGR_002: RobotEntityCfg in params resolves during manager init."""

    spec = _tiny_spec()
    entity = RobotEntityCfg(body_names="base")
    assert not entity.resolved
    manager = TerminationManager(
        TerminationCfg(
            terms={
                "height": DoneTermCfg(
                    func=term_lib.root_height_below,
                    params={"entity": entity, "minimum_m": 0.05},
                )
            }
        ),
        spec,
    )
    assert entity.resolved
    assert entity.body_field_names == ("robot.body.base",)
    pose = torch.tensor([[0.0, 0.0, 0.1, 0.0, 0.0, 0.0, 1.0], [0.0, 0.0, 0.01, 0.0, 0.0, 0.0, 1.0]])
    result = manager.compute(
        _context(
            steps=torch.zeros(2, dtype=torch.long),
            transition_state={"robot.body.base.body_pose": pose},
        )
    )
    assert torch.equal(result.terminated, torch.tensor([False, True]))


def test_ac_py_unit_donemgr_003_reason_keys_exact() -> None:
    """AC_PY_UNIT_DONEMGR_003: reason keys match declared term names exactly."""

    spec = _tiny_spec()
    expected = ("base_contact", "timeout")
    manager = TerminationManager(
        TerminationCfg(
            terms={
                "base_contact": DoneTermCfg(func=lambda ctx, **_: ctx.episode_steps.eq(0)),
                "timeout": DoneTermCfg(func=term_lib.time_out, time_out=True, params={"max_steps": 10}),
            }
        ),
        spec,
    )
    result = manager.compute(_context(steps=torch.zeros(1, dtype=torch.long)))
    assert tuple(result.reason) == expected
    assert manager.term_names == expected


def test_ac_py_unit_donemgr_004_shape_check_still_enforced() -> None:
    """AC_PY_UNIT_DONEMGR_004: combiner shape validation still fires through the manager."""

    spec = _tiny_spec()
    manager = TerminationManager(
        TerminationCfg(
            terms={
                "wrong_shape": DoneTermCfg(
                    func=lambda ctx, **_: torch.zeros(1, dtype=torch.bool),
                )
            }
        ),
        spec,
    )
    with pytest.raises(ValueError, match="returned shape"):
        manager.compute(_context(steps=torch.zeros(2, dtype=torch.long)))


def test_ac_py_unit_donemgr_004_duplicate_name_still_enforced() -> None:
    """AC_PY_UNIT_DONEMGR_004: combiner duplicate-name check still fires through the manager."""

    from uerl.core.direct.termination import TerminationTerm

    spec = _tiny_spec()
    manager = TerminationManager(
        TerminationCfg(
            terms={"timeout": DoneTermCfg(func=term_lib.time_out, time_out=True, params={"max_steps": 1})}
        ),
        spec,
    )
    # Dict configs cannot express duplicate keys; inject through the manager wrap.
    evaluate = manager._terms[0].evaluate
    manager._terms = (
        TerminationTerm(name="dup", time_out=False, evaluate=evaluate),
        TerminationTerm(name="dup", time_out=True, evaluate=evaluate),
    )
    with pytest.raises(ValueError, match="more than once"):
        manager.compute(_context(steps=torch.zeros(1, dtype=torch.long)))


def test_termination_lib_time_out_inclusive_boundary() -> None:
    ctx = _context(steps=torch.tensor([1, 2, 3]))
    assert torch.equal(term_lib.time_out(ctx, max_steps=2), torch.tensor([False, True, True]))


def test_termination_lib_bad_orientation_strict_limit() -> None:
    """Tilt exactly at limit_rad does not terminate; above does."""

    import math

    spec = _tiny_spec()
    entity = RobotEntityCfg(body_names="base")
    entity.resolve(spec)
    limit = 0.5

    def pose_for_upright(upright: float) -> torch.Tensor:
        qx = ((1.0 - upright) / 2.0) ** 0.5
        qw = (1.0 - qx * qx) ** 0.5
        return torch.tensor([[0.0, 0.0, 0.1, qx, 0.0, 0.0, qw]])

    upright_at = math.cos(limit)
    at = term_lib.bad_orientation(
        _context(
            steps=torch.zeros(1, dtype=torch.long),
            transition_state={"robot.body.base.body_pose": pose_for_upright(upright_at)},
        ),
        entity=entity,
        limit_rad=limit,
    )
    above = term_lib.bad_orientation(
        _context(
            steps=torch.zeros(1, dtype=torch.long),
            transition_state={"robot.body.base.body_pose": pose_for_upright(upright_at - 0.05)},
        ),
        entity=entity,
        limit_rad=limit,
    )
    assert not bool(at.item())
    assert bool(above.item())


def test_termination_lib_contact_force_and_height_and_joint_limits() -> None:
    spec = _tiny_spec()
    body = RobotEntityCfg(body_names="base")
    body.resolve(spec)
    joint = RobotEntityCfg(joint_names="hip")
    joint.resolve(spec)

    force_ok = term_lib.contact_force_exceeds(
        _context(
            steps=torch.zeros(1, dtype=torch.long),
            transition_state={"robot.body.base.contact_force": torch.tensor([[0.5, 0.0, 0.0]])},
        ),
        entity=body,
        threshold_n=1.0,
    )
    force_hit = term_lib.contact_force_exceeds(
        _context(
            steps=torch.zeros(1, dtype=torch.long),
            transition_state={"robot.body.base.contact_force": torch.tensor([[1.5, 0.0, 0.0]])},
        ),
        entity=body,
        threshold_n=1.0,
    )
    assert not bool(force_ok.item())
    assert bool(force_hit.item())

    height = term_lib.root_height_below(
        _context(
            steps=torch.zeros(2, dtype=torch.long),
            transition_state={
                "robot.body.base.body_pose": torch.tensor(
                    [
                        [0.0, 0.0, 0.1, 0.0, 0.0, 0.0, 1.0],
                        [0.0, 0.0, 0.05, 0.0, 0.0, 0.0, 1.0],
                    ]
                )
            },
        ),
        entity=body,
        minimum_m=0.05,
    )
    # z < 0.05 only; z==0.05 is allowed
    assert torch.equal(height, torch.tensor([False, False]))
    height_low = term_lib.root_height_below(
        _context(
            steps=torch.zeros(1, dtype=torch.long),
            transition_state={
                "robot.body.base.body_pose": torch.tensor([[0.0, 0.0, 0.049, 0.0, 0.0, 0.0, 1.0]])
            },
        ),
        entity=body,
        minimum_m=0.05,
    )
    assert bool(height_low.item())

    # joint limits: lower=-1 upper=1; exactly at bound OK; outside fails
    at_bound = term_lib.joint_limit_violated(
        _context(
            steps=torch.zeros(1, dtype=torch.long),
            transition_state={"robot.joint.hip.joint_position": torch.tensor([1.0])},
        ),
        entity=joint,
    )
    over = term_lib.joint_limit_violated(
        _context(
            steps=torch.zeros(1, dtype=torch.long),
            transition_state={"robot.joint.hip.joint_position": torch.tensor([1.01])},
        ),
        entity=joint,
    )
    assert not bool(at_bound.item())
    assert bool(over.item())
