"""Select existing host state without changing its physical interpretation."""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import replace

from ...errors import ConfigError
from .contracts import InputField, InputSpec, ResolvedInput


class StateInputFactory:
    """Resolve fields against a host's catalog; register per catalog, not globally."""

    provider_id = "uerl.state"
    version = 1

    def __init__(self, fields: Sequence[InputField]) -> None:
        self._fields: dict[str, InputField] = {}
        for field in fields:
            key = field.name.casefold()
            if key in self._fields:
                raise ConfigError(f"duplicate state field: {field.name}", code="STATE_INPUT_INVALID")
            self._fields[key] = field

    def validate(self, spec: InputSpec) -> ResolvedInput:
        def fail(field: str, message: str) -> ConfigError:
            return ConfigError(message, path=f"inputs.{spec.name}.{field}", code="STATE_INPUT_INVALID")

        if spec.provider_id != self.provider_id or spec.provider_version != self.version:
            raise fail("provider_id", "expected uerl.state version 1")
        if spec.attachment is not None or spec.scene_bindings:
            raise fail("attachment", "existing state retains its host frame and needs no scene bindings")
        if set(spec.parameters) != {"fields"}:
            raise fail("parameters", "expected only an explicit fields list")
        names = spec.parameters["fields"]
        if not isinstance(names, tuple) or not names:
            raise fail("parameters.fields", "expected a nonempty list of state field names")
        selected: list[InputField] = []
        seen: set[str] = set()
        for name in names:
            if not isinstance(name, str) or not name or name.strip() != name:
                raise fail("parameters.fields", "expected state field names without outer whitespace")
            key = name.casefold()
            if key in seen:
                raise fail("parameters.fields", f"duplicate state field: {name}")
            seen.add(key)
            descriptor = self._fields.get(key)
            if descriptor is None:
                raise fail("parameters.fields", f"state field is unavailable: {name}")
            selected.append(descriptor)
        return ResolvedInput(replace(spec, parameters={"fields": [field.name for field in selected]}), tuple(selected))
