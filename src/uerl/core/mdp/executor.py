"""Python observation/action plan executor over the closed operator registry.

Source vs transform (see also ``operators`` module docstring):
* Source ops are specialized here against ``PlanInputs``.
* Transform ops call ``OperatorSpec.evaluate`` uniformly.

``ActionPlanExecutor`` reuses the same registry, Compile checks, and op
evaluation loop as ``PlanExecutor`` — only the input channel
(``policy_action``) and output mapping (``command_fields`` →
``PhysicalCommandBatch``) differ.

Stable ``ConfigError.code`` values (in addition to operator codes):

+----------------------------+-----------------------------------------------+
| code                       | meaning                                       |
+============================+===============================================+
| ``OP_MISSING_FIELD``       | ``select`` field absent from ``raw_state``    |
| ``OP_MISSING_CHANNEL``     | ``command`` channel absent from inputs        |
| ``OP_MISSING_PREVIOUS``    | ``previous_action`` required but not supplied |
| ``OP_MISSING_POLICY``      | ``policy_action`` required but not supplied   |
| ``OP_MISSING_DT``          | ``control_frame_dt`` required but not supplied|
| ``OP_INVALID_DT``          | ``control_frame_dt`` is non-finite/non-positive|
| ``OP_BATCH``               | batch dimension disagrees across op outputs   |
| ``OP_WIDTH``               | declared width disagrees with actual tensor   |
+----------------------------+-----------------------------------------------+

Error timing for ``command``:
* **Compile** (``PlanExecutor`` construction): plan width vs ``command_channels``
  (``channels()``) declaration.
* **Execute**: channel key absent from ``PlanInputs.commands`` (no silent zero-fill).

Error timing for action ``command_fields``:
* **Compile** (``ActionPlanExecutor`` construction): produced slot width vs
  ``command_channels`` physical-command declaration.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import TYPE_CHECKING, cast

import torch

from uerl.core.mdp.operators import (
    EvaluateFn,
    OperatorSpec,
    _check_operator_arity,
    _check_operator_params,
    check_clip,
    check_control_frame_dt,
    check_joint_pos_rel,
    check_offset,
    check_scale,
    check_slice,
    require_operator,
)
from uerl.core.mdp.plan import ActionPlan, ObservationPlan, PlanOp
from uerl.errors import ConfigError

if TYPE_CHECKING:
    from uerl.core.direct.types import PhysicalCommandBatch


_EMPTY_MAPPING: MappingProxyType[str, torch.Tensor] = MappingProxyType({})


def _empty_mapping() -> MappingProxyType[str, torch.Tensor]:
    return _EMPTY_MAPPING


@dataclass(frozen=True, slots=True)
class _CompiledOp:
    """Pre-resolved execution metadata for one PlanOp."""

    op: PlanOp
    spec: OperatorSpec
    input_indices: tuple[int, ...]
    path: str
    source_kind: str
    source_key: str | None
    control_dt_scale: float


@dataclass(frozen=True, slots=True)
class PlanInputs:
    """Runtime inputs consumed by source operators."""

    raw_state: Mapping[str, torch.Tensor]
    commands: Mapping[str, torch.Tensor] = field(default_factory=_empty_mapping)
    previous_action: torch.Tensor | None = None
    policy_action: torch.Tensor | None = None
    control_frame_dt: torch.Tensor | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.raw_state, MappingProxyType):
            object.__setattr__(self, "raw_state", MappingProxyType(dict(self.raw_state)))
        if not isinstance(self.commands, MappingProxyType):
            object.__setattr__(self, "commands", MappingProxyType(dict(self.commands)))


class PlanExecutor:
    """Compile an ``ObservationPlan`` once; evaluate it against ``PlanInputs``."""

    def __init__(
        self,
        plan: ObservationPlan,
        *,
        command_channels: Mapping[str, int] | None = None,
    ) -> None:
        """Construct and compile.

        ``command_channels`` is the ``channels()`` width map. When provided,
        each ``command`` op is checked at construction (Compile) against it.
        """

        plan.validate()
        self._plan = plan
        # Resolve every op at construction — missing ops fail here, not on execute.
        self._specs = tuple(require_operator(op.op) for op in plan.ops)
        channels = None if command_channels is None else dict(command_channels)
        _check_widths(plan.ops, self._specs, command_channels=channels)
        self._compiled = _compile_ops(plan.ops, self._specs)
        name_to_idx = {op.output: idx for idx, op in enumerate(plan.ops)}
        self._compiled_groups = tuple(
            (
                name,
                tuple(name_to_idx[member] for member in members),
                self._plan.group_widths[name],
            )
            for name, members in self._plan.groups.items()
        )

    def execute(self, inputs: PlanInputs) -> Mapping[str, torch.Tensor]:
        """Evaluate the plan and return group tensors in declared member order."""

        slots = _execute_compiled_ops(
            self._compiled,
            inputs.raw_state,
            inputs.commands,
            inputs.previous_action,
            inputs.policy_action,
            inputs.control_frame_dt,
        )
        batch_size = slots[0].shape[0] if slots else None

        groups: dict[str, torch.Tensor] = {}
        for name, member_indices, declared in self._compiled_groups:
            if len(member_indices) == 1:
                grouped = slots[member_indices[0]]
            else:
                grouped = torch.cat([slots[idx] for idx in member_indices], dim=-1)
            if grouped.shape[-1] != declared:
                raise ConfigError(
                    f"group {name!r} width {grouped.shape[-1]} != declared {declared}",
                    code="OP_WIDTH",
                    path=f"group_widths.{name}",
                )
            if batch_size is not None and grouped.shape[0] != batch_size:
                raise ConfigError(
                    f"group {name!r} batch {grouped.shape[0]} != {batch_size}",
                    code="OP_BATCH",
                    path=f"groups.{name}",
                )
            groups[name] = grouped
        return groups


class ActionPlanExecutor:
    """Compile an ``ActionPlan`` once; map policy actions to physical commands.

    Reuses the same operator registry and evaluation loop as ``PlanExecutor``.
    """

    def __init__(
        self,
        plan: ActionPlan,
        *,
        command_channels: Mapping[str, int] | None = None,
    ) -> None:
        """Construct and compile.

        ``command_channels`` is the physical-command width map keyed by
        ``command_fields`` names. When provided, each command field's produced
        width is checked at construction (Compile) against it.
        """

        plan.validate()
        self._plan = plan
        self._specs = tuple(require_operator(op.op) for op in plan.ops)
        channels = None if command_channels is None else dict(command_channels)
        produced = _check_widths(plan.ops, self._specs, command_channels=None)
        for index, op in enumerate(plan.ops):
            if op.op == "policy_action" and op.width != plan.policy_width:
                raise ConfigError(
                    f"policy_action width {op.width} != plan policy_width {plan.policy_width}",
                    code="OP_WIDTH",
                    path=f"ops[{index}].width",
                )
        if channels is not None:
            for index, name in enumerate(plan.command_fields):
                if name not in channels:
                    raise ConfigError(
                        f"command field {name!r} is not declared in command_channels",
                        code="OP_MISSING_CHANNEL",
                        path=f"command_fields[{index}]",
                    )
                declared = channels[name]
                actual = produced[name]
                if declared != actual:
                    raise ConfigError(
                        f"command field {name!r} width {actual} != declared {declared}",
                        code="OP_WIDTH",
                        path=f"command_fields[{index}]",
                    )
        self._compiled = _compile_ops(plan.ops, self._specs)
        name_to_idx = {op.output: idx for idx, op in enumerate(plan.ops)}
        self._command_slots = tuple(
            (name, name_to_idx[name]) for name in self._plan.command_fields
        )
        from uerl.core.direct.types import PhysicalCommandBatch

        self._command_batch_cls = PhysicalCommandBatch

    def execute(self, policy_actions: torch.Tensor) -> PhysicalCommandBatch:
        """Evaluate the plan and return named physical command tensors."""

        if policy_actions.ndim != 2:
            raise ConfigError(
                f"policy_actions must be rank-2 (batch, width), got shape {tuple(policy_actions.shape)}",
                code="OP_WIDTH",
                path="policy_action",
            )
        if policy_actions.shape[-1] != self._plan.policy_width:
            raise ConfigError(
                f"policy_actions width {policy_actions.shape[-1]} != plan policy_width "
                f"{self._plan.policy_width}",
                code="OP_WIDTH",
                path="policy_action",
            )
        slots = _execute_compiled_ops(
            self._compiled,
            _EMPTY_MAPPING,
            _EMPTY_MAPPING,
            None,
            policy_actions,
            None,
        )
        return self._command_batch_cls({name: slots[idx] for name, idx in self._command_slots})


def _check_widths(
    ops: Sequence[PlanOp],
    specs: Sequence[OperatorSpec],
    *,
    command_channels: Mapping[str, int] | None,
) -> dict[str, int]:
    """Validate arity, params, and derived widths at construction time."""

    produced: dict[str, int] = {}
    for index, (op, spec) in enumerate(zip(ops, specs, strict=True)):
        path = f"ops[{index}]"
        _check_operator_arity(spec, len(op.inputs), path=f"{path}.inputs")
        _check_operator_params(spec, op.params, path=path)
        if spec.is_source:
            if op.op == "select":
                # select width is compile-time on PlanOp; not derived from params.
                produced[op.output] = op.width
                continue
            if op.op == "control_frame_dt":
                check_control_frame_dt(op, path=path)
            derived = spec.output_width([], op.params)
            if derived != op.width:
                raise ConfigError(
                    f"operator {op.op!r} derived width {derived} != declared {op.width}",
                    code="OP_WIDTH",
                    path=f"{path}.width",
                )
            if op.op == "command":
                channel = op.params["channel"]
                assert isinstance(channel, str)
                if command_channels is None or channel not in command_channels:
                    raise ConfigError(
                        f"command channel {channel!r} is not declared in command_channels",
                        code="OP_MISSING_CHANNEL",
                        path=f"{path}.params.channel",
                    )
                declared = command_channels[channel]
                if declared != op.width:
                    raise ConfigError(
                        f"command channel {channel!r} width {declared} != declared {op.width}",
                        code="OP_WIDTH",
                        path=f"{path}.width",
                    )
            produced[op.output] = op.width
            continue
        input_widths = [produced[name] for name in op.inputs]
        if op.op == "joint_pos_rel":
            check_joint_pos_rel(op, input_widths, path=path)
        elif op.op == "scale":
            check_scale(op, input_widths, path=path)
        elif op.op == "offset":
            check_offset(op, input_widths, path=path)
        elif op.op == "clip":
            check_clip(op, input_widths, path=path)
        elif op.op == "slice":
            check_slice(op, input_widths, path=path)
        derived = spec.output_width(input_widths, op.params)
        if derived != op.width:
            raise ConfigError(
                f"operator {op.op!r} derived width {derived} != declared {op.width}",
                code="OP_WIDTH",
                path=f"{path}.width",
            )
        produced[op.output] = op.width
    return produced


def _compile_ops(
    ops: Sequence[PlanOp],
    specs: Sequence[OperatorSpec],
) -> tuple[_CompiledOp, ...]:
    """Pre-resolve slot indices and source metadata once after plan validation."""

    name_to_idx: dict[str, int] = {}
    compiled: list[_CompiledOp] = []
    for index, (op, spec) in enumerate(zip(ops, specs, strict=True)):
        input_indices = tuple(name_to_idx[name] for name in op.inputs)
        source_kind = op.op if spec.is_source else ""
        source_key: str | None = None
        control_dt_scale = 1.0
        if source_kind == "select":
            field_name = op.params["field"]
            assert isinstance(field_name, str)
            source_key = field_name
        elif source_kind == "command":
            channel = op.params["channel"]
            assert isinstance(channel, str)
            source_key = channel
        elif source_kind == "control_frame_dt":
            scale = op.params["scale"]
            assert isinstance(scale, (int, float)) and not isinstance(scale, bool)
            control_dt_scale = float(scale)
        compiled.append(
            _CompiledOp(
                op=op,
                spec=spec,
                input_indices=input_indices,
                path=f"ops[{index}]",
                source_kind=source_kind,
                source_key=source_key,
                control_dt_scale=control_dt_scale,
            )
        )
        name_to_idx[op.output] = index
    return tuple(compiled)


def _execute_compiled_ops(
    compiled: tuple[_CompiledOp, ...],
    raw_state: Mapping[str, torch.Tensor],
    commands: Mapping[str, torch.Tensor],
    previous_action: torch.Tensor | None,
    policy_action: torch.Tensor | None,
    control_frame_dt: torch.Tensor | None,
) -> list[torch.Tensor]:
    """Evaluate pre-compiled ops with flat slot indexing."""

    slots: list[torch.Tensor] = [None] * len(compiled)  # type: ignore[list-item]
    batch_size: int | None = None
    for index, item in enumerate(compiled):
        op = item.op
        source_kind = item.source_kind
        if source_kind:
            path = item.path
            if source_kind == "select":
                field_name = cast(str, item.source_key)
                value = raw_state.get(field_name)
                if value is None:
                    raise ConfigError(
                        f"select field {field_name!r} missing from raw_state",
                        code="OP_MISSING_FIELD",
                        path=f"{path}.params.field",
                    )
                if value.ndim == 1:
                    if op.width != 1:
                        raise ConfigError(
                            f"select field {field_name!r} is rank-1 but plan width is {op.width}",
                            code="OP_WIDTH",
                            path=f"{path}.output",
                        )
                    value = value.unsqueeze(-1)
            elif source_kind == "command":
                channel = cast(str, item.source_key)
                value = commands.get(channel)
                if value is None:
                    raise ConfigError(
                        f"command channel {channel!r} was not supplied",
                        code="OP_MISSING_CHANNEL",
                        path=f"{path}.params.channel",
                    )
            elif source_kind == "control_frame_dt":
                if control_frame_dt is None:
                    raise ConfigError(
                        "plan requires control_frame_dt but none was supplied",
                        code="OP_MISSING_DT",
                        path=f"{path}.op",
                    )
                if control_frame_dt.ndim != 2 or control_frame_dt.shape[-1] != 1:
                    raise ConfigError(
                        f"control_frame_dt must have shape (batch, 1), got {tuple(control_frame_dt.shape)}",
                        code="OP_WIDTH",
                        path=f"{path}.input",
                    )
                if not bool(torch.isfinite(control_frame_dt).all()) or not bool(
                    (control_frame_dt > 0).all()
                ):
                    raise ConfigError(
                        "control_frame_dt must be finite and positive",
                        code="OP_INVALID_DT",
                        path=f"{path}.input",
                    )
                value = control_frame_dt * item.control_dt_scale
            elif source_kind == "previous_action":
                if previous_action is None:
                    raise ConfigError(
                        "plan requires previous_action but none was supplied",
                        code="OP_MISSING_PREVIOUS",
                        path=f"{path}.op",
                    )
                value = previous_action
            elif source_kind == "policy_action":
                if policy_action is None:
                    raise ConfigError(
                        "plan requires policy_action but none was supplied",
                        code="OP_MISSING_POLICY",
                        path=f"{path}.op",
                    )
                value = policy_action
            else:
                raise ConfigError(
                    f"unhandled source operator {op.op!r}",
                    code="OP_UNKNOWN",
                    path=f"{path}.op",
                )
        else:
            in_idx = item.input_indices
            n_in = len(in_idx)
            if n_in == 1:
                args: Sequence[torch.Tensor] = (slots[in_idx[0]],)
            elif n_in == 2:
                args = (slots[in_idx[0]], slots[in_idx[1]])
            elif n_in == 0:
                args = ()
            else:
                args = [slots[idx] for idx in in_idx]
            value = cast(EvaluateFn, item.spec.evaluate)(args, op.params)
        if value.ndim != 2:
            raise ConfigError(
                f"operator {op.op!r} output must be rank-2 (batch, width), got shape {tuple(value.shape)}",
                code="OP_WIDTH",
                path=f"{item.path}.output",
            )
        if value.shape[-1] != op.width:
            raise ConfigError(
                f"operator {op.op!r} output width {value.shape[-1]} != declared {op.width}",
                code="OP_WIDTH",
                path=f"{item.path}.width",
            )
        rows = int(value.shape[0])
        if batch_size is None:
            batch_size = rows
        elif rows != batch_size:
            raise ConfigError(
                f"operator {op.op!r} batch {rows} != expected {batch_size}",
                code="OP_BATCH",
                path=f"{item.path}.output",
            )
        slots[index] = value
    return slots


def _execute_ops(
    ops: Sequence[PlanOp],
    specs: Sequence[OperatorSpec],
    inputs: PlanInputs,
) -> dict[str, torch.Tensor]:
    """Shared op evaluation loop for observation and action plans."""

    compiled = _compile_ops(ops, specs)
    slot_values = _execute_compiled_ops(
        compiled,
        inputs.raw_state,
        inputs.commands,
        inputs.previous_action,
        inputs.policy_action,
        inputs.control_frame_dt,
    )
    return {op.output: slot_values[idx] for idx, op in enumerate(ops)}
