"""Declare frozen configuration specs with strict mapping construction."""

from __future__ import annotations

import math
import types
from collections.abc import Mapping
from dataclasses import MISSING as DC_MISSING
from dataclasses import (
    dataclass,
    field,
    fields,
    is_dataclass,
)
from dataclasses import replace as dc_replace
from typing import Any, Literal, TypeVar, Union, cast, dataclass_transform, get_args, get_origin, get_type_hints

from uerl.errors import ConfigError

T = TypeVar("T")

_CONSTRAINTS_KEY = "configspec_constraints"
_TYPE_HINTS_ATTR = "__configspec_type_hints__"


class _Missing:
    """Sentinel for required fields that must be supplied by the payload."""

    def __repr__(self) -> str:
        return "MISSING"

    def __bool__(self) -> bool:
        return False


MISSING: Any = _Missing()


@dataclass(frozen=True, slots=True)
class _FieldConstraints:
    """Numeric and integral constraints attached via ``spec_field`` metadata."""

    gt: float | None = None
    ge: float | None = None
    lt: float | None = None
    le: float | None = None
    finite: bool = False
    integral: bool = False
    min_length: int | None = None
    item_gt: float | None = None
    ordered: bool = False


def spec_field(
    default: Any = MISSING,
    *,
    default_factory: Any = MISSING,
    gt: float | None = None,
    ge: float | None = None,
    lt: float | None = None,
    le: float | None = None,
    finite: bool = False,
    integral: bool = False,
    min_length: int | None = None,
    item_gt: float | None = None,
    ordered: bool = False,
) -> Any:
    """Declare a config field with optional numeric constraint metadata.

    Args:
        default: Field default, or ``MISSING`` when the field is required.
        default_factory: Optional zero-arg factory used instead of ``default``.
        gt: Exclusive lower bound.
        ge: Inclusive lower bound.
        lt: Exclusive upper bound.
        le: Inclusive upper bound.
        finite: When true, reject NaN and infinities.
        integral: When true, require a non-bool ``int``.
        min_length: Minimum length for ``str`` or sequence values.
        item_gt: Exclusive lower bound applied to each sequence element.
        ordered: When true, require a two-number sequence with lower <= upper.

    Returns:
        A ``dataclasses.field`` carrying constraint metadata.
    """

    constraints = _FieldConstraints(
        gt=gt,
        ge=ge,
        lt=lt,
        le=le,
        finite=finite,
        integral=integral,
        min_length=min_length,
        item_gt=item_gt,
        ordered=ordered,
    )
    metadata = {_CONSTRAINTS_KEY: constraints}
    if default_factory is not MISSING:
        return field(default_factory=default_factory, metadata=metadata)
    return field(default=default, metadata=metadata)


@dataclass_transform(field_specifiers=(spec_field, field))
def configspec(cls: type[T]) -> type[T]:
    """Turn a class into a frozen slots dataclass with mapping helpers.

    Differences from a plain dataclass:
      - ``MISSING`` fields omitted from a payload raise ``ConfigError``
      - ``from_mapping`` rejects unknown keys and records dotted ``path``
      - nested ``@configspec`` fields recurse automatically
      - ``to_dict`` yields a JSON-like tree consumable by ``canonical``
    """

    decorated = dataclass(frozen=True, slots=True)(cls)
    decorated.__configspec__ = True  # type: ignore[attr-defined]
    setattr(decorated, _TYPE_HINTS_ATTR, get_type_hints(decorated))

    def _from_mapping(cls_: type[T], payload: Mapping[str, Any], *, path: str = "") -> T:
        return from_mapping(cls_, payload, path=path)

    def _to_dict(self: T) -> dict[str, Any]:
        return to_dict(self)

    def _replace(self: T, **overrides: Any) -> T:
        return replace(self, **overrides)

    decorated.from_mapping = classmethod(_from_mapping)  # type: ignore[attr-defined]
    decorated.to_dict = _to_dict  # type: ignore[attr-defined]
    decorated.replace = _replace  # type: ignore[attr-defined]
    return decorated


def from_mapping(spec: type[T], payload: Mapping[str, Any], *, path: str = "") -> T:
    """Strictly construct a ``@configspec`` instance from a mapping.

    Args:
        spec: Decorated configuration class.
        payload: User-provided mapping (typically YAML-loaded).
        path: Dotted path prefix for nested error reporting.

    Returns:
        A fully validated frozen configuration instance.

    Raises:
        ConfigError: When the payload is not a mapping, contains unknown keys,
            omits required fields, or fails type/constraint checks.
    """

    if not _is_configspec_type(spec):
        raise TypeError(f"{spec!r} is not a @configspec class")
    mapping = _require_mapping(payload, path)
    spec_fields = fields(cast(Any, spec))
    field_by_name = {item.name: item for item in spec_fields}
    for key in mapping:
        if key not in field_by_name or key.startswith("_"):
            raise ConfigError(
                f"unknown configuration key {key!r}",
                code="CONFIG_UNKNOWN_KEY",
                path=_join_path(path, key),
            )

    hints = _type_hints_of(spec)
    values: dict[str, Any] = {}
    for item in spec_fields:
        field_path = _join_path(path, item.name)
        annotation = hints.get(item.name, item.type)
        if item.name.startswith("_"):
            if item.default is not DC_MISSING:
                values[item.name] = item.default
            elif item.default_factory is not DC_MISSING:
                values[item.name] = item.default_factory()
            else:
                values[item.name] = ()
            continue
        if item.name in mapping:
            values[item.name] = _parse_field_value(item, annotation, mapping[item.name], field_path)
            continue
        if item.default is not DC_MISSING:
            if item.default is MISSING:
                raise ConfigError(
                    f"missing required configuration field {item.name!r}",
                    code="CONFIG_MISSING_FIELD",
                    path=field_path,
                )
            values[item.name] = item.default
            continue
        if item.default_factory is not DC_MISSING:
            values[item.name] = item.default_factory()
            continue
        raise ConfigError(
            f"missing required configuration field {item.name!r}",
            code="CONFIG_MISSING_FIELD",
            path=field_path,
        )

    return spec(**values)


def to_dict(instance: Any) -> dict[str, Any]:
    """Export a ``@configspec`` instance as a JSON-like mapping.

    Nested configspecs become dicts and tuples become lists so the result is
    consumable by ``uerl.core.config.canonical``. Derived fields whose names
    start with ``_`` are omitted so ``from_mapping`` can round-trip the payload.
    """

    if not _is_configspec_instance(instance):
        raise TypeError(f"{type(instance)!r} is not a @configspec instance")
    return {
        item.name: _to_jsonable_value(getattr(instance, item.name))
        for item in fields(cast(Any, instance))
        if not item.name.startswith("_")
    }


def replace(instance: T, **overrides: Any) -> T:
    """Return a new instance with selected fields replaced.

    The original instance is left unchanged. Unknown override names raise
    ``ConfigError(code=CONFIG_UNKNOWN_KEY)``. Override values are validated
    with the same rules as ``from_mapping``.
    """

    if not _is_configspec_instance(instance):
        raise TypeError(f"{type(instance)!r} is not a @configspec instance")
    field_by_name = {item.name: item for item in fields(cast(Any, instance))}
    hints = _type_hints_of(type(instance))
    coerced: dict[str, Any] = {}
    for key, value in overrides.items():
        if key not in field_by_name or key.startswith("_"):
            raise ConfigError(
                f"unknown configuration key {key!r}",
                code="CONFIG_UNKNOWN_KEY",
                path=key,
            )
        annotation = hints.get(key, field_by_name[key].type)
        coerced[key] = _parse_field_value(field_by_name[key], annotation, value, key)
    return cast(T, dc_replace(cast(Any, instance), **coerced))


def _type_hints_of(spec: type[Any]) -> dict[str, Any]:
    cached = getattr(spec, _TYPE_HINTS_ATTR, None)
    if isinstance(cached, dict):
        return cached
    hints = get_type_hints(spec)
    setattr(spec, _TYPE_HINTS_ATTR, hints)
    return hints


def _is_configspec_type(spec: type[Any]) -> bool:
    return bool(getattr(spec, "__configspec__", False)) and is_dataclass(spec)


def _is_configspec_instance(value: Any) -> bool:
    return _is_configspec_type(type(value))


def _join_path(path: str, key: str) -> str:
    return f"{path}.{key}" if path else key


def _require_mapping(value: Any, path: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping) or any(not isinstance(key, str) for key in value):
        raise ConfigError(
            "expected a mapping with string keys",
            code="CONFIG_NOT_MAPPING",
            path=path or None,
        )
    return value


def _constraints_of(item: Any) -> _FieldConstraints:
    raw = item.metadata.get(_CONSTRAINTS_KEY)
    if isinstance(raw, _FieldConstraints):
        return raw
    return _FieldConstraints()


def _parse_field_value(item: Any, annotation: Any, value: Any, path: str) -> Any:
    if value is MISSING:
        raise ConfigError(
            f"missing required configuration field {item.name!r}",
            code="CONFIG_MISSING_FIELD",
            path=path,
        )
    parsed = _coerce_value(annotation, value, path)
    _apply_constraints(parsed, _constraints_of(item), path)
    return parsed


def _optional_args(annotation: Any) -> tuple[Any, bool]:
    """Return ``(inner_type, allows_none)`` for ``T | None`` annotations."""

    origin = get_origin(annotation)
    if origin is Union or origin is types.UnionType:
        args = list(get_args(annotation))
        none_type = type(None)
        allows_none = any(arg is none_type for arg in args)
        non_none = [arg for arg in args if arg is not none_type]
        if allows_none and len(non_none) == 1:
            return non_none[0], True
        if allows_none and not non_none:
            return none_type, True
        if len(non_none) > 1:
            # Preserve multi-alternative unions such as ``str | tuple[str, ...]``.
            combined: Any = non_none[0]
            for alternative in non_none[1:]:
                combined = combined | alternative
            return combined, allows_none
    return annotation, False


def _coerce_value(annotation: Any, value: Any, path: str) -> Any:
    if annotation is Any:
        return value

    inner, allows_none = _optional_args(annotation)
    origin = get_origin(inner)
    if origin is Literal:
        allowed = get_args(inner)
        if value not in allowed:
            raise ConfigError(
                f"expected one of {allowed!r}, got {value!r}",
                code="CONFIG_OUT_OF_RANGE",
                path=path,
            )
        return value

    if value is None:
        if allows_none:
            return None
        raise ConfigError(
            f"expected {annotation!r}, got None",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )

    if origin is Union or origin is types.UnionType:
        alternatives = [arg for arg in get_args(inner) if arg is not type(None)]
        errors: list[ConfigError] = []
        for alternative in alternatives:
            try:
                return _coerce_value(alternative, value, path)
            except ConfigError as exc:
                errors.append(exc)
        if errors:
            raise errors[0]
        raise ConfigError(
            f"expected one of {alternatives!r}, got {type(value).__name__}",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )

    if _is_configspec_type(inner):
        if isinstance(value, inner):
            return value
        mapping = _require_mapping(value, path)
        return from_mapping(inner, mapping, path=path)

    if origin is tuple:
        return _coerce_tuple(inner, value, path)

    if origin is dict or inner is dict:
        return _coerce_string_key_mapping(value, path)

    if inner is float:
        return _coerce_float(value, path)
    if inner is int:
        return _coerce_int(value, path)
    if inner is bool:
        if type(value) is not bool:
            raise ConfigError(
                f"expected bool, got {type(value).__name__}",
                code="CONFIG_TYPE_MISMATCH",
                path=path,
            )
        return value
    if inner is str:
        if not isinstance(value, str):
            raise ConfigError(
                f"expected str, got {type(value).__name__}",
                code="CONFIG_TYPE_MISMATCH",
                path=path,
            )
        return value

    if isinstance(inner, type) and not isinstance(value, inner):
        raise ConfigError(
            f"expected {inner.__name__}, got {type(value).__name__}",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )
    return value


def _coerce_tuple(annotation: Any, value: Any, path: str) -> tuple[Any, ...]:
    args = get_args(annotation)
    if not isinstance(value, (list, tuple)):
        raise ConfigError(
            f"expected tuple, got {type(value).__name__}",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )
    if len(args) == 2 and args[1] is Ellipsis:
        return tuple(_coerce_value(args[0], item, f"{path}[{index}]") for index, item in enumerate(value))
    if len(value) != len(args):
        raise ConfigError(
            f"expected tuple of length {len(args)}, got length {len(value)}",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )
    return tuple(_coerce_value(args[index], item, f"{path}[{index}]") for index, item in enumerate(value))


def _coerce_string_key_mapping(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, Mapping) or any(not isinstance(key, str) for key in value):
        raise ConfigError(
            "expected a mapping with string keys",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )
    return dict(cast(Mapping[str, Any], value))


def _coerce_float(value: Any, path: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigError(
            f"expected float, got {type(value).__name__}",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )
    return float(value)


def _coerce_int(value: Any, path: str) -> int:
    if type(value) is not int:
        raise ConfigError(
            f"expected int, got {type(value).__name__}",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )
    return value


def _apply_constraints(value: Any, constraints: _FieldConstraints, path: str) -> None:
    if constraints.min_length is not None:
        if not isinstance(value, (str, tuple, list)):
            raise ConfigError(
                f"expected a sized value, got {type(value).__name__}",
                code="CONFIG_TYPE_MISMATCH",
                path=path,
            )
        if len(value) < constraints.min_length:
            raise ConfigError(
                f"expected length >= {constraints.min_length}",
                code="CONFIG_OUT_OF_RANGE",
                path=path,
            )

    if constraints.item_gt is not None:
        if not isinstance(value, (tuple, list)):
            raise ConfigError(
                f"expected a sequence, got {type(value).__name__}",
                code="CONFIG_TYPE_MISMATCH",
                path=path,
            )
        for index, item in enumerate(value):
            if isinstance(item, bool) or not isinstance(item, (int, float)):
                raise ConfigError(
                    f"expected a number, got {type(item).__name__}",
                    code="CONFIG_TYPE_MISMATCH",
                    path=f"{path}[{index}]",
                )
            if not float(item) > constraints.item_gt:
                raise ConfigError(
                    f"expected value > {constraints.item_gt}",
                    code="CONFIG_OUT_OF_RANGE",
                    path=f"{path}[{index}]",
                )

    if constraints.ordered:
        if not isinstance(value, (tuple, list)) or len(value) != 2:
            raise ConfigError(
                "expected an ordered pair",
                code="CONFIG_TYPE_MISMATCH",
                path=path,
            )
        lower, upper = value
        for index, item in enumerate((lower, upper)):
            if isinstance(item, bool) or not isinstance(item, (int, float)):
                raise ConfigError(
                    f"expected a number, got {type(item).__name__}",
                    code="CONFIG_TYPE_MISMATCH",
                    path=f"{path}[{index}]",
                )
            if not math.isfinite(float(item)):
                raise ConfigError(
                    "expected a finite number",
                    code="CONFIG_NOT_FINITE",
                    path=f"{path}[{index}]",
                )
        if float(lower) > float(upper):
            raise ConfigError(
                "expected lower bound <= upper bound",
                code="CONFIG_OUT_OF_RANGE",
                path=path,
            )

    if constraints.integral and type(value) is not int:
        raise ConfigError(
            f"expected an integral int, got {type(value).__name__}",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )

    needs_numeric = constraints.finite or any(
        bound is not None for bound in (constraints.gt, constraints.ge, constraints.lt, constraints.le)
    )
    if not needs_numeric:
        return

    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ConfigError(
            f"expected a number, got {type(value).__name__}",
            code="CONFIG_TYPE_MISMATCH",
            path=path,
        )
    numeric = float(value)

    if constraints.finite and not math.isfinite(numeric):
        raise ConfigError(
            "expected a finite number",
            code="CONFIG_NOT_FINITE",
            path=path,
        )

    if constraints.gt is not None and not numeric > constraints.gt:
        raise ConfigError(
            f"expected value > {constraints.gt}",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )
    if constraints.ge is not None and not numeric >= constraints.ge:
        raise ConfigError(
            f"expected value >= {constraints.ge}",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )
    if constraints.lt is not None and not numeric < constraints.lt:
        raise ConfigError(
            f"expected value < {constraints.lt}",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )
    if constraints.le is not None and not numeric <= constraints.le:
        raise ConfigError(
            f"expected value <= {constraints.le}",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )


def _to_jsonable_value(value: Any) -> Any:
    if _is_configspec_instance(value):
        return to_dict(value)
    if isinstance(value, tuple):
        return [_to_jsonable_value(item) for item in value]
    return value


__all__ = [
    "MISSING",
    "configspec",
    "from_mapping",
    "replace",
    "spec_field",
    "to_dict",
]
