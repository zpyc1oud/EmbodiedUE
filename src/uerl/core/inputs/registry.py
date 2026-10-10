"""Resolve external input declarations without simulator or Task-name dispatch."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from typing import Protocol

from ...errors import ConfigError
from .contracts import InputField, InputSpec, ResolvedInput


class InputFactory(Protocol):
    """Validate provider-specific parameters and return effective declarations."""

    @property
    def provider_id(self) -> str: ...

    @property
    def version(self) -> int: ...

    def validate(self, spec: InputSpec) -> ResolvedInput: ...


@dataclass(frozen=True, slots=True)
class InputFieldBinding:
    instance: str
    descriptor: InputField
    offset: int


@dataclass(frozen=True, slots=True)
class CompiledInputRequirements:
    """Hold stable declaration-order fields and offsets for one input set."""

    inputs: tuple[ResolvedInput, ...]
    fields: tuple[InputFieldBinding, ...]
    width: int


class InputRegistry:
    """Own explicit registrations; importing a package does not mutate a singleton."""

    def __init__(self) -> None:
        self._factories: dict[tuple[str, int], InputFactory] = {}

    def register(self, factory: InputFactory) -> None:
        InputSpec("registration", factory.provider_id, factory.version)
        key = (factory.provider_id, factory.version)
        if key in self._factories:
            raise ConfigError(f"input provider already registered: {key}", code="INPUT_PROVIDER_DUPLICATE")
        self._factories[key] = factory

    def compile(self, specs: Sequence[InputSpec]) -> CompiledInputRequirements:
        inputs: list[ResolvedInput] = []
        fields: list[InputFieldBinding] = []
        names: set[str] = set()
        output_names: set[str] = set()
        width = 0
        for spec in specs:
            if spec.name in names:
                raise ConfigError(f"duplicate input instance: {spec.name}", code="INPUT_INSTANCE_DUPLICATE")
            names.add(spec.name)
            key = (spec.provider_id, spec.provider_version)
            factory = self._factories.get(key)
            if factory is None:
                raise ConfigError(
                    f"input provider is not installed or version is unsupported: {key}",
                    code="INPUT_PROVIDER_UNAVAILABLE",
                    path=f"inputs.{spec.name}.provider_id",
                )
            resolved = factory.validate(spec)
            if not isinstance(resolved, ResolvedInput):
                raise ConfigError("factory must return ResolvedInput", code="INPUT_FACTORY_RESULT")
            identity = (resolved.spec.name, resolved.spec.provider_id, resolved.spec.provider_version)
            if identity != (spec.name, *key):
                raise ConfigError("factory changed input identity during validation", code="INPUT_FACTORY_IDENTITY")
            for descriptor in resolved.fields:
                if descriptor.name.casefold() in output_names:
                    raise ConfigError(f"multiple producers for field: {descriptor.name}", code="INPUT_FIELD_DUPLICATE")
                output_names.add(descriptor.name.casefold())
                fields.append(InputFieldBinding(spec.name, descriptor, width))
                width += descriptor.width
                if width > 2**31 - 1:
                    raise ConfigError("compiled input width must fit int32", code="INPUT_WIDTH_OVERFLOW")
            inputs.append(resolved)
        return CompiledInputRequirements(tuple(inputs), tuple(fields), width)
