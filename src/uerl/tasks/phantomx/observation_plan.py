"""PhantomX policy observation plan (ticket 11).

Assembles the same segments as ``PhantomXTask.build_observations`` using only
the closed operator set. The 3-D velocity command is an external ``command``
channel (training fills it via heading/turn logic; deploy hosts supply it
directly) — that math is intentionally outside the observation operator set.
"""

from __future__ import annotations

from math import prod

from uerl.core.config.robot import ObsType
from uerl.core.direct.robot_observation import ObservationShapeTable
from uerl.core.mdp.plan import PLAN_VERSION, ObservationPlan, PlanOp
from uerl.tasks.phantomx.config import (
    PHANTOMX_CONTROL_FRAME_DT_SCALE,
    PHANTOMX_FEET,
    PHANTOMX_JOINTS,
)

# Actuator default_pos order matches the built-in PhantomX asset declaration /
# ``PHANTOMX_JOINTS`` (c1=0, thigh=0.15, tibia=-0.30).
PHANTOMX_JOINT_DEFAULTS: tuple[float, ...] = tuple(
    0.0 if index % 3 == 0 else (0.15 if index % 3 == 1 else -0.30) for index in range(len(PHANTOMX_JOINTS))
)

_POLICY_GROUP = (
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


def build_phantomx_observation_plan(observation_shapes: ObservationShapeTable) -> ObservationPlan:
    """Return the declarative plan using the negotiated UE field widths."""

    body_pose_width = prod(observation_shapes.shape_for(ObsType.BODY_POSE))
    linear_velocity_width = prod(observation_shapes.shape_for(ObsType.BODY_LINEAR_VELOCITY))
    angular_velocity_width = prod(observation_shapes.shape_for(ObsType.BODY_ANGULAR_VELOCITY))
    terrain_width = prod(observation_shapes.shape_for(ObsType.TERRAIN_HEIGHT))
    clearance_width = prod(observation_shapes.shape_for(ObsType.GROUND_CLEARANCE))
    contact_width = prod(observation_shapes.shape_for(ObsType.CONTACT))
    joint_position_width = prod(observation_shapes.shape_for(ObsType.JOINT_POSITION))
    joint_velocity_width = prod(observation_shapes.shape_for(ObsType.JOINT_VELOCITY))
    action_width = len(PHANTOMX_JOINTS)

    ops: list[PlanOp] = [
        PlanOp("select", (), "pose", body_pose_width, {"field": "robot.body.base_link.body_pose"}),
        PlanOp(
            "select", (), "lin_vel_w", linear_velocity_width, {"field": "robot.body.base_link.body_linear_velocity"}
        ),
        PlanOp(
            "select", (), "ang_vel_w", angular_velocity_width, {"field": "robot.body.base_link.body_angular_velocity"}
        ),
        PlanOp("slice", ("pose",), "quat", 4, {"start": 3, "width": 4}),
        PlanOp("rotate_inverse", ("quat", "lin_vel_w"), "lin_vel_b", linear_velocity_width, {}),
        PlanOp("rotate_inverse", ("quat", "ang_vel_w"), "ang_vel_b", angular_velocity_width, {}),
        PlanOp("projected_gravity", ("quat",), "gravity_b", linear_velocity_width, {"gravity": (0.0, 0.0, -1.0)}),
        PlanOp("command", (), "velocity_cmd", 3, {"channel": "velocity", "width": 3}),
        PlanOp("select", (), "terrain", terrain_width, {"field": "robot.body.base_link.terrain_height"}),
        PlanOp("select", (), "clearance", clearance_width, {"field": "robot.body.base_link.ground_clearance"}),
    ]

    contact_slots: list[str] = []
    for foot in PHANTOMX_FEET:
        name = f"contact_{foot}"
        ops.append(PlanOp("select", (), name, contact_width, {"field": f"robot.body.{foot}.contact"}))
        contact_slots.append(name)
    ops.append(PlanOp("concat", tuple(contact_slots), "contacts", contact_width * len(contact_slots), {}))

    joint_pos_slots: list[str] = []
    joint_vel_slots: list[str] = []
    for joint in PHANTOMX_JOINTS:
        pos = f"jp_{joint}"
        vel = f"jv_{joint}"
        ops.append(
            PlanOp("select", (), pos, joint_position_width, {"field": f"robot.joint.{joint}.joint_position"})
        )
        ops.append(
            PlanOp("select", (), vel, joint_velocity_width, {"field": f"robot.joint.{joint}.joint_velocity"})
        )
        joint_pos_slots.append(pos)
        joint_vel_slots.append(vel)
    joint_pos_total_width = joint_position_width * len(joint_pos_slots)
    joint_vel_total_width = joint_velocity_width * len(joint_vel_slots)
    ops.append(PlanOp("concat", tuple(joint_pos_slots), "joint_pos", joint_pos_total_width, {}))
    ops.append(
        PlanOp(
            "joint_pos_rel",
            ("joint_pos",),
            "joint_pos_rel",
            joint_pos_total_width,
            {"default": PHANTOMX_JOINT_DEFAULTS},
        )
    )
    ops.append(PlanOp("concat", tuple(joint_vel_slots), "joint_vel", joint_vel_total_width, {}))
    ops.append(PlanOp("previous_action", (), "prev_action", action_width, {"width": action_width}))
    ops.append(
        PlanOp(
            "control_frame_dt",
            (),
            "control_frame_dt",
            1,
            {"scale": PHANTOMX_CONTROL_FRAME_DT_SCALE},
        )
    )

    state_requirements = (
        "robot.body.base_link.body_pose",
        "robot.body.base_link.body_linear_velocity",
        "robot.body.base_link.body_angular_velocity",
        "robot.body.base_link.terrain_height",
        "robot.body.base_link.ground_clearance",
        *(f"robot.body.{foot}.contact" for foot in PHANTOMX_FEET),
        *(f"robot.joint.{joint}.joint_position" for joint in PHANTOMX_JOINTS),
        *(f"robot.joint.{joint}.joint_velocity" for joint in PHANTOMX_JOINTS),
    )
    return ObservationPlan(
        state_requirements=state_requirements,
        ops=tuple(ops),
        groups={"policy": _POLICY_GROUP},
        group_widths={
            "policy": sum(
                next(op.width for op in ops if op.output == member) for member in _POLICY_GROUP
            )
        },
        plan_version=PLAN_VERSION,
    )


__all__ = [
    "PHANTOMX_JOINT_DEFAULTS",
    "PHANTOMX_CONTROL_FRAME_DT_SCALE",
    "build_phantomx_observation_plan",
]
