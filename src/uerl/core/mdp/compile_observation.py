"""Compile declarative observation terms into an ``ObservationPlan``.

Term convenience fields expand in fixed order **scale → clip**: first rescale
into the target unit, then clip in that unit.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from math import prod
from typing import Any, cast

from uerl.core.config.entity import RobotEntityCfg
from uerl.core.config.robot import RobotSpec
from uerl.core.direct.robot_observation import ObservationShapeTable, robot_observation_schema
from uerl.core.mdp.operators import require_operator
from uerl.core.mdp.plan import PLAN_VERSION, ObservationPlan, PlanOp
from uerl.core.mdp.terms import ObservationCfg, ObsGroupCfg, ObsTermCfg
from uerl.errors import ConfigError


def compile_observation_plan(
    cfg: ObservationCfg,
    spec: RobotSpec,
    *,
    policy_width: int,
    observation_shapes: ObservationShapeTable,
) -> ObservationPlan:
    """Expand ``ObservationCfg`` into a plan using only the closed operator set."""

    if not cfg.groups:
        raise ConfigError(
            "ObservationCfg.groups must not be empty",
            code="CONFIG_OUT_OF_RANGE",
            path="groups",
        )

    field_widths = _field_widths(spec, observation_shapes)
    ops: list[PlanOp] = []
    produced: dict[str, int] = {}
    groups: dict[str, tuple[str, ...]] = {}
    group_widths: dict[str, int] = {}

    for group_name, group in cfg.groups.items():
        if not group_name:
            raise ConfigError(
                "observation group names must be non-empty",
                code="CONFIG_OUT_OF_RANGE",
                path="groups",
            )
        if not group.concatenate:
            raise ConfigError(
                "ObservationCfg currently requires concatenate=True",
                code="CONFIG_OUT_OF_RANGE",
                path=f"groups.{group_name}.concatenate",
            )
        if not group.terms:
            raise ConfigError(
                f"observation group {group_name!r} has no terms",
                code="CONFIG_OUT_OF_RANGE",
                path=f"groups.{group_name}.terms",
            )

        for term_name, term in group.terms.items():
            if not term_name:
                raise ConfigError(
                    "observation term names must be non-empty",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"groups.{group_name}.terms",
                )
            if term_name in produced:
                raise ConfigError(
                    f"observation term {term_name!r} is produced more than once",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"groups.{group_name}.terms.{term_name}",
                )
            term_ops = _compile_term(
                term_name,
                term,
                spec,
                field_widths=field_widths,
                produced=produced,
                policy_width=policy_width,
                path=f"groups.{group_name}.terms.{term_name}",
            )
            ops.extend(term_ops)
            for op in term_ops:
                produced[op.output] = op.width

        members = _group_members(group, path=f"groups.{group_name}")
        width = 0
        for member in members:
            if member not in produced:
                raise ConfigError(
                    f"group {group_name!r} references unknown term {member!r}",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"groups.{group_name}.members",
                )
            width += produced[member]
        if width <= 0:
            raise ConfigError(
                f"group {group_name!r} width derivation failed",
                code="CONFIG_OUT_OF_RANGE",
                path=f"groups.{group_name}",
            )
        groups[group_name] = members
        group_widths[group_name] = width

    state_requirements = tuple(
        dict.fromkeys(
            cast(str, op.params["field"])
            for op in ops
            if op.op == "select" and isinstance(op.params.get("field"), str)
        )
    )
    return ObservationPlan(
        state_requirements=state_requirements,
        ops=tuple(ops),
        groups=groups,
        group_widths=group_widths,
        plan_version=PLAN_VERSION,
    )


def _group_members(group: ObsGroupCfg, *, path: str) -> tuple[str, ...]:
    declared = tuple(group.terms.keys())
    if group.members is None:
        return declared
    if not group.members:
        raise ConfigError(
            "observation group members must not be empty when set",
            code="CONFIG_OUT_OF_RANGE",
            path=f"{path}.members",
        )
    unknown = [name for name in group.members if name not in group.terms]
    if unknown:
        raise ConfigError(
            f"group references unknown term {unknown[0]!r}",
            code="CONFIG_OUT_OF_RANGE",
            path=f"{path}.members",
        )
    return tuple(group.members)


def _compile_term(
    name: str,
    cfg: ObsTermCfg,
    spec: RobotSpec,
    *,
    field_widths: Mapping[str, int],
    produced: Mapping[str, int],
    policy_width: int,
    path: str,
) -> tuple[PlanOp, ...]:
    try:
        require_operator(cfg.op)
    except ConfigError as exc:
        raise ConfigError(
            f"unknown observation operator {cfg.op!r}",
            code=exc.code,
            path=f"{path}.op",
        ) from exc

    params = dict(cfg.params)
    entity = params.pop("entity", None)
    if entity is not None and not isinstance(entity, RobotEntityCfg):
        raise ConfigError(
            "params.entity must be a RobotEntityCfg",
            code="CONFIG_OUT_OF_RANGE",
            path=f"{path}.params.entity",
        )

    ops: list[PlanOp] = []
    if entity is not None:
        core, width = _expand_entity_core(
            name,
            cfg,
            entity,
            spec,
            params=params,
            field_widths=field_widths,
            ops=ops,
            path=path,
        )
        if cfg.inputs:
            raise ConfigError(
                "entity-expanded terms cannot declare inputs",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.inputs",
            )
        if params:
            raise ConfigError(
                f"unused observation term params: {sorted(params)!r}",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params",
            )
    else:
        core, width = _expand_plain_core(
            name,
            cfg,
            params=params,
            field_widths=field_widths,
            produced=produced,
            policy_width=policy_width,
            ops=ops,
            path=path,
        )

    return _apply_scale_clip(name, core, width, cfg, ops, path=path)


def _expand_entity_core(
    name: str,
    cfg: ObsTermCfg,
    entity: RobotEntityCfg,
    spec: RobotSpec,
    *,
    params: dict[str, Any],
    field_widths: Mapping[str, int],
    ops: list[PlanOp],
    path: str,
) -> tuple[str, int]:
    if not entity.resolved:
        entity.resolve(spec)
    field_type = params.pop("field", None)
    if not isinstance(field_type, str) or not field_type:
        if cfg.op == "joint_pos_rel":
            field_type = "joint_position"
        elif cfg.op == "select":
            raise ConfigError(
                "select with entity requires params.field observation type",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params.field",
            )
        else:
            raise ConfigError(
                f"operator {cfg.op!r} does not support params.entity",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params.entity",
            )

    source_fields = _entity_observation_fields(entity, field_type, path=f"{path}.params.entity")
    source_slots: list[str] = []
    for index, field_name in enumerate(source_fields):
        slot = f"{name}.src{index}"
        width = _lookup_width(field_name, field_widths, path=f"{path}.params.entity")
        ops.append(PlanOp("select", (), slot, width, {"field": field_name}))
        source_slots.append(slot)

    if len(source_slots) == 1:
        concat_slot = source_slots[0]
        concat_width = ops[-1].width
    else:
        concat_slot = f"{name}.concat"
        concat_width = sum(
            op.width for op in ops if op.output in set(source_slots)
        )
        ops.append(PlanOp("concat", tuple(source_slots), concat_slot, concat_width, {}))

    if cfg.op == "select":
        return concat_slot, concat_width
    if cfg.op == "joint_pos_rel":
        defaults = params.pop("default", None)
        if defaults is None:
            defaults = _actuator_defaults(spec, entity.joint_ids, path=f"{path}.params.entity")
        elif not isinstance(defaults, tuple):
            raise ConfigError(
                "joint_pos_rel default must be a float tuple",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params.default",
            )
        rel_slot = f"{name}.rel"
        ops.append(
            PlanOp(
                "joint_pos_rel",
                (concat_slot,),
                rel_slot,
                concat_width,
                {"default": cast(tuple[float, ...], defaults)},
            )
        )
        return rel_slot, concat_width
    raise ConfigError(
        f"operator {cfg.op!r} does not support params.entity",
        code="CONFIG_OUT_OF_RANGE",
        path=f"{path}.params.entity",
    )


def _expand_plain_core(
    name: str,
    cfg: ObsTermCfg,
    *,
    params: dict[str, Any],
    field_widths: Mapping[str, int],
    produced: Mapping[str, int],
    policy_width: int,
    ops: list[PlanOp],
    path: str,
) -> tuple[str, int]:
    inputs = tuple(cfg.inputs)
    for input_name in inputs:
        if input_name not in produced:
            raise ConfigError(
                f"term input {input_name!r} is not produced by any earlier term",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.inputs",
            )

    core = f"{name}.core"
    if cfg.op == "select":
        field_name = params.pop("field", None)
        if not isinstance(field_name, str) or not field_name:
            raise ConfigError(
                "select requires params.field",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params.field",
            )
        width = _lookup_width(field_name, field_widths, path=f"{path}.params.field")
        ops.append(PlanOp("select", (), core, width, {"field": field_name}))
        if params:
            raise ConfigError(
                f"unused observation term params: {sorted(params)!r}",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params",
            )
        return core, width

    if cfg.op == "previous_action":
        width = int(params.pop("width", policy_width))
        if width <= 0:
            raise ConfigError(
                "previous_action width must be positive",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params.width",
            )
        ops.append(PlanOp("previous_action", (), core, width, {"width": width}))
        if params:
            raise ConfigError(
                f"unused observation term params: {sorted(params)!r}",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params",
            )
        return core, width

    if cfg.op == "command":
        channel = params.pop("channel", None)
        width = params.pop("width", None)
        if not isinstance(channel, str) or not channel:
            raise ConfigError(
                "command requires params.channel",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params.channel",
            )
        if not isinstance(width, int) or isinstance(width, bool) or width <= 0:
            raise ConfigError(
                "command requires positive int params.width",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params.width",
            )
        ops.append(PlanOp("command", (), core, width, {"channel": channel, "width": width}))
        if params:
            raise ConfigError(
                f"unused observation term params: {sorted(params)!r}",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.params",
            )
        return core, width

    input_widths = [produced[input_name] for input_name in inputs]
    width = _derive_width(cfg.op, input_widths, params, path=path)
    ops.append(PlanOp(cfg.op, inputs, core, width, _plan_params(params)))
    return core, width


def _apply_scale_clip(
    name: str,
    core: str,
    width: int,
    cfg: ObsTermCfg,
    ops: list[PlanOp],
    *,
    path: str,
) -> tuple[PlanOp, ...]:
    """Apply scale → clip, ending with output slot ``name``."""

    needs_scale = _needs_scale(cfg.scale)
    needs_clip = cfg.clip is not None
    current = core

    if needs_scale and needs_clip:
        scaled = f"{name}.scaled"
        ops.append(
            PlanOp(
                "scale",
                (current,),
                scaled,
                width,
                {"factor": _scale_param(cfg.scale, width, path=f"{path}.scale")},
            )
        )
        low, high = cfg.clip  # type: ignore[misc]
        ops.append(
            PlanOp(
                "clip",
                (scaled,),
                name,
                width,
                {"low": float(low), "high": float(high)},
            )
        )
        return tuple(ops)

    if needs_scale:
        ops.append(
            PlanOp(
                "scale",
                (current,),
                name,
                width,
                {"factor": _scale_param(cfg.scale, width, path=f"{path}.scale")},
            )
        )
        return tuple(ops)

    if needs_clip:
        low, high = cfg.clip  # type: ignore[misc]
        ops.append(
            PlanOp(
                "clip",
                (current,),
                name,
                width,
                {"low": float(low), "high": float(high)},
            )
        )
        return tuple(ops)

    if current != name:
        # No convenience ops: promote the core/concat slot to the term name.
        # Prefer rewriting the last op's output when it is uniquely ours.
        last = ops[-1]
        if last.output == current and all(op.output != name for op in ops):
            ops[-1] = PlanOp(last.op, last.inputs, name, last.width, dict(last.params))
        else:
            ops.append(PlanOp("scale", (current,), name, width, {"factor": 1.0}))
    return tuple(ops)


def _entity_observation_fields(
    entity: RobotEntityCfg,
    field_type: str,
    *,
    path: str,
) -> tuple[str, ...]:
    prefixes: list[str] = []
    if entity.joint_ids:
        prefixes.extend(entity.joint_field_names)
    if entity.body_ids:
        prefixes.extend(entity.body_field_names)
    if not prefixes:
        raise ConfigError(
            "entity selected no joints or bodies",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )
    return tuple(f"{prefix}.{field_type}" for prefix in prefixes)


def _field_widths(spec: RobotSpec, observation_shapes: ObservationShapeTable) -> dict[str, int]:
    widths: dict[str, int] = {}
    for descriptor in robot_observation_schema(spec, observation_shapes):
        name = cast(str, descriptor["name"])
        shape = cast(Sequence[int], descriptor["shape"])
        widths[name] = int(prod(shape)) if shape else 1
    return widths


def _lookup_width(field_name: str, field_widths: Mapping[str, int], *, path: str) -> int:
    if field_name not in field_widths:
        raise ConfigError(
            f"observation field {field_name!r} is not in the robot schema",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )
    return field_widths[field_name]


def _actuator_defaults(spec: RobotSpec, joint_ids: Sequence[int], *, path: str) -> tuple[float, ...]:
    defaults: list[float] = []
    for joint_id in joint_ids:
        matched = False
        for actuator in spec.actuators:
            if actuator.joint_index == joint_id:
                defaults.append(float(actuator.default_pos))
                matched = True
                break
        if not matched:
            raise ConfigError(
                f"no actuator bound to joint_index {joint_id}",
                code="CONFIG_OUT_OF_RANGE",
                path=path,
            )
    return tuple(defaults)


def _needs_scale(scale: float | tuple[float, ...]) -> bool:
    if isinstance(scale, tuple):
        return any(value != 1.0 for value in scale)
    return float(scale) != 1.0


def _scale_param(
    scale: float | tuple[float, ...],
    width: int,
    *,
    path: str,
) -> float | tuple[float, ...]:
    if isinstance(scale, tuple):
        if len(scale) != width:
            raise ConfigError(
                f"scale length {len(scale)} != term width {width}",
                code="CONFIG_OUT_OF_RANGE",
                path=path,
            )
        return scale
    return float(scale)


def _derive_width(
    op_name: str,
    input_widths: Sequence[int],
    params: Mapping[str, Any],
    *,
    path: str,
) -> int:
    operator = require_operator(op_name)
    try:
        width = int(operator.output_width(input_widths, _plan_params(params)))
    except ConfigError:
        raise
    except Exception as exc:  # noqa: BLE001
        raise ConfigError(
            f"width derivation failed for operator {op_name!r}: {exc}",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        ) from exc
    if width <= 0:
        explicit = params.get("width")
        if isinstance(explicit, int) and not isinstance(explicit, bool) and explicit > 0:
            return explicit
        raise ConfigError(
            f"width derivation failed for operator {op_name!r}",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )
    return width


def _plan_params(params: Mapping[str, Any]) -> dict[str, Any]:
    return {key: value for key, value in params.items() if key != "inputs"}


__all__ = ["compile_observation_plan"]
