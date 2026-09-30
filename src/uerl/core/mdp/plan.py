"""Observation and action plan data models with a versioned JSON codec.

Plans are immutable data. This module does not execute operators; it only
parses, serializes, and checks structural self-consistency.

Stable ``ConfigError.code`` values:

+---------------------------+----------------------------------------------+
| code                      | meaning                                      |
+===========================+==============================================+
| ``PLAN_VERSION_MISMATCH`` | ``plan_version`` missing or not supported    |
| ``PLAN_NOT_MAPPING``      | root or nested object is not a mapping       |
| ``PLAN_MISSING_FIELD``    | required JSON field absent                   |
| ``PLAN_UNKNOWN_KEY``      | unexpected JSON key                          |
| ``PLAN_TYPE_MISMATCH``    | JSON value has the wrong type                |
| ``PLAN_INVALID_WIDTH``    | op or group width is not a positive int      |
| ``PLAN_DUPLICATE_SLOT``   | output slot name repeats or collides         |
| ``PLAN_UNKNOWN_SLOT``     | input/group member names an unknown slot     |
| ``PLAN_CYCLE``            | op dependency graph contains a cycle         |
| ``PLAN_NOT_TOPOLOGICAL``  | ops are not listed in topological order      |
| ``PLAN_GROUP_MISMATCH``   | groups / group_widths keys or widths disagree|
+---------------------------+----------------------------------------------+
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Any, Final

from uerl.errors import ConfigError

PLAN_VERSION: Final[int] = 1

ParamValue = float | int | bool | str | tuple[float, ...] | tuple[int, ...]

_OBS_KEYS: Final[frozenset[str]] = frozenset(
    {"plan_version", "state_requirements", "ops", "groups", "group_widths"}
)
_ACTION_KEYS: Final[frozenset[str]] = frozenset(
    {"plan_version", "ops", "command_fields", "policy_width"}
)
_OP_KEYS: Final[frozenset[str]] = frozenset({"op", "inputs", "output", "width", "params"})


@dataclass(frozen=True, slots=True)
class PlanOp:
    """One operator node in a plan DAG."""

    op: str
    inputs: tuple[str, ...]
    output: str
    width: int
    params: Mapping[str, ParamValue] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "inputs", tuple(self.inputs))
        object.__setattr__(self, "params", MappingProxyType(dict(self.params)))


@dataclass(frozen=True, slots=True)
class ObservationPlan:
    """Declarative observation assembly plan (data only)."""

    state_requirements: tuple[str, ...]
    ops: tuple[PlanOp, ...]
    groups: Mapping[str, tuple[str, ...]]
    group_widths: Mapping[str, int]
    plan_version: int = PLAN_VERSION

    def __post_init__(self) -> None:
        object.__setattr__(self, "state_requirements", tuple(self.state_requirements))
        object.__setattr__(self, "ops", tuple(self.ops))
        frozen_groups = {name: tuple(members) for name, members in self.groups.items()}
        object.__setattr__(self, "groups", MappingProxyType(frozen_groups))
        object.__setattr__(self, "group_widths", MappingProxyType(dict(self.group_widths)))
        self.validate()

    def validate(self) -> None:
        """Reject cyclic, non-topological, or otherwise inconsistent structure."""

        _validate_ops(
            self.ops,
            available_sources=set(self.state_requirements),
            path_prefix="ops",
        )
        produced = {op.output: op.width for op in self.ops}
        if set(self.groups) != set(self.group_widths):
            raise ConfigError(
                "groups and group_widths must declare the same keys",
                code="PLAN_GROUP_MISMATCH",
                path="groups",
            )
        for name, members in self.groups.items():
            width_path = f"group_widths.{name}"
            declared = self.group_widths[name]
            if type(declared) is not int or isinstance(declared, bool) or declared <= 0:
                raise ConfigError(
                    f"group width must be a positive int, got {declared!r}",
                    code="PLAN_INVALID_WIDTH",
                    path=width_path,
                )
            total = 0
            for index, member in enumerate(members):
                member_path = f"groups.{name}[{index}]"
                if member not in produced:
                    raise ConfigError(
                        f"group member {member!r} is not produced by any op",
                        code="PLAN_UNKNOWN_SLOT",
                        path=member_path,
                    )
                total += produced[member]
            if total != declared:
                raise ConfigError(
                    f"group {name!r} width {declared} != sum of member widths {total}",
                    code="PLAN_GROUP_MISMATCH",
                    path=width_path,
                )

    def to_json(self) -> dict[str, Any]:
        """Serialize to the versioned plan JSON object."""

        return {
            "plan_version": self.plan_version,
            "state_requirements": list(self.state_requirements),
            "ops": [_op_to_json(op) for op in self.ops],
            "groups": {name: list(members) for name, members in self.groups.items()},
            "group_widths": dict(self.group_widths),
        }

    @classmethod
    def from_json(cls, payload: Mapping[str, Any]) -> ObservationPlan:
        """Parse and validate an observation plan from JSON-like data."""

        mapping = _require_mapping(payload, path="")
        _reject_unknown_keys(mapping, _OBS_KEYS, path="")
        version = _require_plan_version(mapping)
        state_requirements = _parse_str_tuple(
            mapping.get("state_requirements"),
            path="state_requirements",
            required=True,
        )
        ops = _parse_ops(mapping.get("ops"), path="ops")
        groups_raw = mapping.get("groups")
        if groups_raw is None:
            raise ConfigError("missing required field 'groups'", code="PLAN_MISSING_FIELD", path="groups")
        groups_map = _require_mapping(groups_raw, path="groups")
        groups: dict[str, tuple[str, ...]] = {}
        for name, members in groups_map.items():
            groups[str(name)] = _parse_str_tuple(members, path=f"groups.{name}", required=True)
        widths_raw = mapping.get("group_widths")
        if widths_raw is None:
            raise ConfigError(
                "missing required field 'group_widths'",
                code="PLAN_MISSING_FIELD",
                path="group_widths",
            )
        widths_map = _require_mapping(widths_raw, path="group_widths")
        group_widths: dict[str, int] = {}
        for name, width in widths_map.items():
            group_widths[str(name)] = _require_positive_int(width, path=f"group_widths.{name}")
        return cls(
            plan_version=version,
            state_requirements=state_requirements,
            ops=ops,
            groups=groups,
            group_widths=group_widths,
        )


@dataclass(frozen=True, slots=True)
class ActionPlan:
    """Declarative action assembly plan (data only)."""

    ops: tuple[PlanOp, ...]
    command_fields: tuple[str, ...]
    policy_width: int
    plan_version: int = PLAN_VERSION

    def __post_init__(self) -> None:
        object.__setattr__(self, "ops", tuple(self.ops))
        object.__setattr__(self, "command_fields", tuple(self.command_fields))
        self.validate()

    def validate(self) -> None:
        """Reject cyclic, non-topological, or otherwise inconsistent structure."""

        if type(self.policy_width) is not int or isinstance(self.policy_width, bool) or self.policy_width <= 0:
            raise ConfigError(
                f"policy_width must be a positive int, got {self.policy_width!r}",
                code="PLAN_INVALID_WIDTH",
                path="policy_width",
            )
        _validate_ops(self.ops, available_sources=set(), path_prefix="ops")
        produced = {op.output for op in self.ops}
        for index, name in enumerate(self.command_fields):
            if name not in produced:
                raise ConfigError(
                    f"command field {name!r} is not produced by any op",
                    code="PLAN_UNKNOWN_SLOT",
                    path=f"command_fields[{index}]",
                )

    def to_json(self) -> dict[str, Any]:
        """Serialize to the versioned action-plan JSON object."""

        return {
            "plan_version": self.plan_version,
            "ops": [_op_to_json(op) for op in self.ops],
            "command_fields": list(self.command_fields),
            "policy_width": self.policy_width,
        }

    @classmethod
    def from_json(cls, payload: Mapping[str, Any]) -> ActionPlan:
        """Parse and validate an action plan from JSON-like data."""

        mapping = _require_mapping(payload, path="")
        _reject_unknown_keys(mapping, _ACTION_KEYS, path="")
        version = _require_plan_version(mapping)
        ops = _parse_ops(mapping.get("ops"), path="ops")
        command_fields = _parse_str_tuple(
            mapping.get("command_fields"),
            path="command_fields",
            required=True,
        )
        if "policy_width" not in mapping:
            raise ConfigError(
                "missing required field 'policy_width'",
                code="PLAN_MISSING_FIELD",
                path="policy_width",
            )
        policy_width = _require_positive_int(mapping["policy_width"], path="policy_width")
        return cls(
            plan_version=version,
            ops=ops,
            command_fields=command_fields,
            policy_width=policy_width,
        )


def _op_to_json(op: PlanOp) -> dict[str, Any]:
    return {
        "op": op.op,
        "inputs": list(op.inputs),
        "output": op.output,
        "width": op.width,
        "params": {key: _param_to_json(value) for key, value in op.params.items()},
    }


def _param_to_json(value: ParamValue) -> Any:
    if isinstance(value, tuple):
        return list(value)
    return value


def _param_from_json(value: Any, *, path: str) -> ParamValue:
    if isinstance(value, bool):
        return value
    if isinstance(value, int):
        return value
    if isinstance(value, float):
        return value
    if isinstance(value, str):
        return value
    if isinstance(value, list):
        if len(value) == 0:
            return ()
        if all(type(item) is int for item in value):
            return tuple(value)
        if all(isinstance(item, (int, float)) and not isinstance(item, bool) for item in value):
            return tuple(float(item) for item in value)
        raise ConfigError(
            "param vector must contain only numbers",
            code="PLAN_TYPE_MISMATCH",
            path=path,
        )
    raise ConfigError(
        f"unsupported param value type {type(value).__name__}",
        code="PLAN_TYPE_MISMATCH",
        path=path,
    )


def _parse_ops(raw: Any, *, path: str) -> tuple[PlanOp, ...]:
    if raw is None:
        raise ConfigError(f"missing required field '{path}'", code="PLAN_MISSING_FIELD", path=path)
    if not isinstance(raw, list):
        raise ConfigError("ops must be a list", code="PLAN_TYPE_MISMATCH", path=path)
    ops: list[PlanOp] = []
    for index, item in enumerate(raw):
        op_path = f"{path}[{index}]"
        mapping = _require_mapping(item, path=op_path)
        _reject_unknown_keys(mapping, _OP_KEYS, path=op_path)
        if "op" not in mapping:
            raise ConfigError("missing required field 'op'", code="PLAN_MISSING_FIELD", path=f"{op_path}.op")
        if not isinstance(mapping["op"], str) or not mapping["op"]:
            raise ConfigError("op name must be a non-empty string", code="PLAN_TYPE_MISMATCH", path=f"{op_path}.op")
        if "output" not in mapping:
            raise ConfigError(
                "missing required field 'output'",
                code="PLAN_MISSING_FIELD",
                path=f"{op_path}.output",
            )
        if not isinstance(mapping["output"], str) or not mapping["output"]:
            raise ConfigError(
                "output must be a non-empty string",
                code="PLAN_TYPE_MISMATCH",
                path=f"{op_path}.output",
            )
        if "width" not in mapping:
            raise ConfigError(
                "missing required field 'width'",
                code="PLAN_MISSING_FIELD",
                path=f"{op_path}.width",
            )
        width = _require_positive_int(mapping["width"], path=f"{op_path}.width")
        inputs = _parse_str_tuple(mapping.get("inputs", []), path=f"{op_path}.inputs", required=True)
        params_raw = mapping.get("params", {})
        params_map = _require_mapping(params_raw, path=f"{op_path}.params")
        params = {
            str(key): _param_from_json(value, path=f"{op_path}.params.{key}")
            for key, value in params_map.items()
        }
        ops.append(
            PlanOp(
                op=mapping["op"],
                inputs=inputs,
                output=mapping["output"],
                width=width,
                params=params,
            )
        )
    return tuple(ops)


def _validate_ops(
    ops: Sequence[PlanOp],
    *,
    available_sources: set[str],
    path_prefix: str,
) -> None:
    produced: dict[str, int] = {}
    for index, op in enumerate(ops):
        op_path = f"{path_prefix}[{index}]"
        if not op.op:
            raise ConfigError("op name must be non-empty", code="PLAN_TYPE_MISMATCH", path=f"{op_path}.op")
        if type(op.width) is not int or isinstance(op.width, bool) or op.width <= 0:
            raise ConfigError(
                f"width must be a positive int, got {op.width!r}",
                code="PLAN_INVALID_WIDTH",
                path=f"{op_path}.width",
            )
        if not op.output:
            raise ConfigError("output must be non-empty", code="PLAN_TYPE_MISMATCH", path=f"{op_path}.output")
        if op.output in produced or op.output in available_sources:
            raise ConfigError(
                f"duplicate slot name {op.output!r}",
                code="PLAN_DUPLICATE_SLOT",
                path=f"{op_path}.output",
            )
        produced[op.output] = op.width

    all_slots = set(available_sources) | set(produced)
    output_index = {op.output: index for index, op in enumerate(ops)}
    dependents: list[list[int]] = [[] for _ in ops]
    for consumer_index, op in enumerate(ops):
        for input_index, name in enumerate(op.inputs):
            if name not in all_slots:
                raise ConfigError(
                    f"unknown slot {name!r}",
                    code="PLAN_UNKNOWN_SLOT",
                    path=f"{path_prefix}[{consumer_index}].inputs[{input_index}]",
                )
            producer = output_index.get(name)
            if producer is not None:
                dependents[producer].append(consumer_index)

    if _has_cycle(dependents):
        raise ConfigError("plan ops contain a cyclic dependency", code="PLAN_CYCLE", path=path_prefix)

    available = set(available_sources)
    for index, op in enumerate(ops):
        for input_index, name in enumerate(op.inputs):
            if name not in available:
                raise ConfigError(
                    f"op input {name!r} is not available in topological order",
                    code="PLAN_NOT_TOPOLOGICAL",
                    path=f"{path_prefix}[{index}].inputs[{input_index}]",
                )
        available.add(op.output)


def _has_cycle(dependents: Sequence[Sequence[int]]) -> bool:
    """Return True when the directed adjacency list contains a cycle."""

    white, gray, black = 0, 1, 2
    color = [white] * len(dependents)

    def visit(node: int) -> bool:
        color[node] = gray
        for nxt in dependents[node]:
            if color[nxt] == gray:
                return True
            if color[nxt] == white and visit(nxt):
                return True
        color[node] = black
        return False

    return any(color[node] == white and visit(node) for node in range(len(dependents)))


def _require_plan_version(mapping: Mapping[str, Any]) -> int:
    if "plan_version" not in mapping:
        raise ConfigError(
            "missing required field 'plan_version'",
            code="PLAN_VERSION_MISMATCH",
            path="plan_version",
        )
    version = mapping["plan_version"]
    if type(version) is not int or isinstance(version, bool):
        raise ConfigError(
            f"plan_version must be an int, got {type(version).__name__}",
            code="PLAN_VERSION_MISMATCH",
            path="plan_version",
        )
    if version != PLAN_VERSION:
        raise ConfigError(
            f"unsupported plan_version {version}; expected {PLAN_VERSION}",
            code="PLAN_VERSION_MISMATCH",
            path="plan_version",
        )
    return version


def _require_mapping(value: Any, *, path: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        label = path or "<root>"
        raise ConfigError(
            f"{label} must be a mapping",
            code="PLAN_NOT_MAPPING",
            path=path or None,
        )
    return value


def _reject_unknown_keys(mapping: Mapping[str, Any], allowed: frozenset[str], *, path: str) -> None:
    for key in mapping:
        if key not in allowed:
            key_path = f"{path}.{key}" if path else str(key)
            raise ConfigError(
                f"unknown plan key {key!r}",
                code="PLAN_UNKNOWN_KEY",
                path=key_path,
            )


def _parse_str_tuple(raw: Any, *, path: str, required: bool) -> tuple[str, ...]:
    if raw is None:
        if required:
            raise ConfigError(f"missing required field '{path}'", code="PLAN_MISSING_FIELD", path=path)
        return ()
    if not isinstance(raw, list):
        raise ConfigError("expected a list of strings", code="PLAN_TYPE_MISMATCH", path=path)
    result: list[str] = []
    for index, item in enumerate(raw):
        if not isinstance(item, str) or not item:
            raise ConfigError(
                "list entries must be non-empty strings",
                code="PLAN_TYPE_MISMATCH",
                path=f"{path}[{index}]",
            )
        result.append(item)
    return tuple(result)


def _require_positive_int(value: Any, *, path: str) -> int:
    if type(value) is not int or isinstance(value, bool) or value <= 0:
        raise ConfigError(
            f"expected a positive int, got {value!r}",
            code="PLAN_INVALID_WIDTH",
            path=path,
        )
    return value
