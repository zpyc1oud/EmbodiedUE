"""Compile declarative action terms into an ``ActionPlan``."""

from __future__ import annotations

from typing import Literal

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec
from uerl.core.mdp.plan import ActionPlan, PlanOp
from uerl.core.mdp.terms import ActionCfg, ActionTermCfg
from uerl.errors import ConfigError


def compile_action_plan(cfg: ActionCfg, spec: RobotSpec) -> ActionPlan:
    """Expand ``ActionCfg`` into a plan using only the closed operator set.

    ``use_default_offset`` is expanded here into a numeric ``offset`` bias from
    each actuator's ``default_pos``. The plan never contains ``offset_default``.
    """

    if not cfg.terms:
        raise ConfigError(
            "ActionCfg.terms must not be empty",
            code="CONFIG_OUT_OF_RANGE",
            path="terms",
        )

    prepared: list[tuple[str, ActionTermCfg, RobotEntityCfg, tuple[int, ...]]] = []
    for name, term in cfg.terms.items():
        if not name:
            raise ConfigError(
                "action term names must be non-empty",
                code="CONFIG_OUT_OF_RANGE",
                path="terms",
            )
        entity = term.entity
        if not entity.resolved:
            entity.resolve(spec)
        joint_ids = entity.joint_ids
        if not joint_ids:
            raise ConfigError(
                f"action term {name!r} selected no joints",
                code="CONFIG_OUT_OF_RANGE",
                path=f"terms.{name}.entity",
            )
        prepared.append((name, term, entity, joint_ids))

    policy_width = sum(len(joint_ids) for _n, _t, _e, joint_ids in prepared)
    ops: list[PlanOp] = [
        PlanOp("policy_action", (), "raw", policy_width, {"width": policy_width}),
    ]
    command_fields: list[str] = []
    cursor = 0
    for name, term, _entity, joint_ids in prepared:
        width = len(joint_ids)
        sliced = f"{name}.raw"
        ops.append(
            PlanOp(
                "slice",
                ("raw",),
                sliced,
                width,
                {"start": cursor, "width": width},
            )
        )
        cursor += width
        current = sliced
        if term.clip is not None:
            low, high = term.clip
            clipped = f"{name}.clipped"
            ops.append(PlanOp("clip", (current,), clipped, width, {"low": low, "high": high}))
            current = clipped
        scaled = f"{name}.scaled"
        ops.append(
            PlanOp(
                "scale",
                (current,),
                scaled,
                width,
                {"factor": _scale_param(term.scale, width, path=f"terms.{name}.scale")},
            )
        )
        current = scaled
        if term.use_default_offset:
            bias = tuple(spec.actuators[_actuator_index(spec, joint_id)].default_pos for joint_id in joint_ids)
            offset_out = f"{name}.offset"
            ops.append(PlanOp("offset", (current,), offset_out, width, {"bias": bias}))
            current = offset_out
        command_field = _command_field_name(name, term.target_mode)
        if command_field in command_fields:
            raise ConfigError(
                f"action term {name!r} produces duplicate command field {command_field!r}",
                code="CONFIG_OUT_OF_RANGE",
                path=f"terms.{name}",
            )
        # Rename final slot to the command field via identity scale 1.0 when names differ.
        if current != command_field:
            ops.append(PlanOp("scale", (current,), command_field, width, {"factor": 1.0}))
        command_fields.append(command_field)

    return ActionPlan(ops=tuple(ops), command_fields=tuple(command_fields), policy_width=policy_width)


def _command_field_name(term_name: str, target_mode: Literal["position", "effort"]) -> str:
    if target_mode == "position":
        return f"robot.actuator.{term_name}"
    return f"robot.actuator.effort.{term_name}"


def _scale_param(
    scale: float | tuple[float, ...],
    width: int,
    *,
    path: str,
) -> float | tuple[float, ...]:
    if isinstance(scale, tuple):
        if len(scale) != width:
            raise ConfigError(
                f"scale length {len(scale)} != joint count {width}",
                code="CONFIG_OUT_OF_RANGE",
                path=path,
            )
        return scale
    return float(scale)


def _actuator_index(spec: RobotSpec, joint_index: int) -> int:
    for actuator in spec.actuators:
        if actuator.joint_index == joint_index:
            return actuator.index
    raise ConfigError(
        f"no actuator bound to joint_index {joint_index}",
        code="CONFIG_OUT_OF_RANGE",
        path="actuators",
    )


__all__ = ["compile_action_plan"]
