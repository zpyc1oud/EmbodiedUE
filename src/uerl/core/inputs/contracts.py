"""Immutable, host-independent requirements for synchronous numeric inputs."""

from __future__ import annotations

import math
from collections.abc import Mapping
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import cast

from ...errors import ConfigError


def _error(path: str, message: str) -> ConfigError:
    return ConfigError(message, path=path, code="INPUT_CONTRACT_INVALID")


def _name(value: object, path: str) -> str:
    if not isinstance(value, str) or not value.strip() or value != value.strip():
        raise _error(path, "expected a nonempty name without outer whitespace")
    return value


def _freeze(value: object, path: str) -> object:
    if value is None or isinstance(value, (str, bool)):
        return value
    if type(value) in (int, float):
        try:
            finite = math.isfinite(cast(float, value))
        except OverflowError:
            finite = False
        if not finite:
            raise _error(path, "input parameters must be finite")
        return value
    if isinstance(value, Mapping):
        return MappingProxyType({_name(key, path): _freeze(item, f"{path}.{key}") for key, item in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(_freeze(item, f"{path}[{index}]") for index, item in enumerate(value))
    raise _error(path, "expected a YAML scalar, list or string-keyed mapping")


def _thaw(value: object) -> object:
    if isinstance(value, Mapping):
        return {key: _thaw(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [_thaw(item) for item in value]
    return value


@dataclass(frozen=True, slots=True)
class InputSpec:
    """Declare policy semantics without references to a concrete UE map."""

    name: str
    provider_id: str
    provider_version: int
    parameters: Mapping[str, object] = field(default_factory=dict)
    attachment: str | None = None
    scene_bindings: tuple[str, ...] = ()
    sampling: str = "completed_window"
    missing_data: str = "fault"

    def __post_init__(self) -> None:
        _name(self.name, "input.name")
        _name(self.provider_id, f"inputs.{self.name}.provider_id")
        if type(self.provider_version) is not int or not 1 <= self.provider_version <= 2**31 - 1:
            raise _error(f"inputs.{self.name}.provider_version", "expected a positive int32")
        if not isinstance(self.parameters, Mapping):
            raise _error(f"inputs.{self.name}.parameters", "expected a mapping")
        object.__setattr__(self, "parameters", _freeze(self.parameters, f"inputs.{self.name}.parameters"))
        if self.attachment is not None:
            _name(self.attachment, f"inputs.{self.name}.attachment")
        if not isinstance(self.scene_bindings, (tuple, list)):
            raise _error(f"inputs.{self.name}.scene_bindings", "expected a sequence of logical binding names")
        bindings = tuple(_name(item, f"inputs.{self.name}.scene_bindings") for item in self.scene_bindings)
        if len(set(bindings)) != len(bindings):
            raise _error(f"inputs.{self.name}.scene_bindings", "logical bindings must be unique")
        object.__setattr__(self, "scene_bindings", bindings)
        if self.sampling != "completed_window":
            raise _error(f"inputs.{self.name}.sampling", "only completed_window sampling is supported")
        if self.missing_data != "fault":
            raise _error(f"inputs.{self.name}.missing_data", "only required-data fault behavior is supported")

    def to_mapping(self) -> dict[str, object]:
        """Produce mutable YAML-ready data without exposing the frozen configuration."""
        return {
            "name": self.name,
            "provider_id": self.provider_id,
            "provider_version": self.provider_version,
            "parameters": _thaw(self.parameters),
            "attachment": self.attachment,
            "scene_bindings": list(self.scene_bindings),
            "sampling": self.sampling,
            "missing_data": self.missing_data,
        }

    @classmethod
    def from_mapping(cls, value: Mapping[str, object]) -> InputSpec:
        """Parse one declaration and reject misspelled settings."""
        if not isinstance(value, Mapping):
            raise _error("input", "expected a mapping")
        allowed = {
            "name",
            "provider_id",
            "provider_version",
            "parameters",
            "attachment",
            "scene_bindings",
            "sampling",
            "missing_data",
        }
        unknown = set(value) - allowed
        if unknown:
            raise _error("input", f"unknown input fields: {sorted(str(key) for key in unknown)}")
        missing = {"name", "provider_id", "provider_version"} - set(value)
        if missing:
            raise _error("input", f"missing input fields: {sorted(missing)}")
        return cls(**dict(value))  # type: ignore[arg-type]


@dataclass(frozen=True, slots=True)
class InputField:
    """Describe an output using the existing numeric field metadata vocabulary."""

    name: str
    shape: tuple[int, ...]
    unit: str
    frame: str
    semantic: str
    source: str
    dtype: str = "float32"

    def __post_init__(self) -> None:
        for attr in ("name", "unit", "frame", "semantic", "source"):
            _name(getattr(self, attr), f"field.{attr}")
        if self.name.casefold() == "none":
            raise _error("field.name", "None is reserved by native field names")
        if not isinstance(self.shape, (tuple, list)) or not self.shape:
            raise _error(f"fields.{self.name}.shape", "expected a nonempty shape")
        shape = tuple(self.shape)
        if any(type(size) is not int or size <= 0 for size in shape):
            raise _error(f"fields.{self.name}.shape", "shape dimensions must be positive integers")
        if math.prod(shape) > 2**31 - 1:
            raise _error(f"fields.{self.name}.shape", "field width must fit int32")
        object.__setattr__(self, "shape", shape)
        if self.dtype != "float32":
            raise _error(f"fields.{self.name}.dtype", "only float32 inputs are supported")

    @property
    def width(self) -> int:
        return math.prod(self.shape)

    def to_mapping(self) -> dict[str, object]:
        return {
            "name": self.name,
            "shape": list(self.shape),
            "dtype": self.dtype,
            "unit": self.unit,
            "frame": self.frame,
            "semantic": self.semantic,
            "source": self.source,
        }


@dataclass(frozen=True, slots=True)
class ResolvedInput:
    """Pair the effective specification with factory-validated output descriptors."""

    spec: InputSpec
    fields: tuple[InputField, ...]

    def __post_init__(self) -> None:
        if not isinstance(self.spec, InputSpec):
            raise _error("input.spec", "expected InputSpec")
        if not self.fields or any(not isinstance(item, InputField) for item in self.fields):
            raise _error(f"inputs.{self.spec.name}.fields", "expected nonempty InputField descriptors")
        fields = tuple(self.fields)
        if len({item.name.casefold() for item in fields}) != len(fields):
            raise _error(f"inputs.{self.spec.name}.fields", "output field names must be unique")
        object.__setattr__(self, "fields", fields)

    def to_mapping(self) -> dict[str, object]:
        return {"spec": self.spec.to_mapping(), "fields": [item.to_mapping() for item in self.fields]}
