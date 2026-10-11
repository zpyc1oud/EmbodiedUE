"""Versioned, paired Worker/artifact input contract serialization."""

from collections.abc import Mapping, Sequence

from ...errors import ConfigError
from .contracts import InputField, InputSpec, ResolvedInput


def input_contract(requirements: Sequence[ResolvedInput]) -> dict[str, object]:
    return {"version": 1, "requirements": [item.to_mapping() for item in requirements]}


def input_declarations(specs: Sequence[InputSpec]) -> dict[str, object]:
    return {"version": 1, "requirements": [{"spec": item.to_mapping(), "fields": []} for item in specs]}


def parse_input_contract(value: object) -> tuple[ResolvedInput, ...]:
    def fail(message: str) -> ConfigError:
        return ConfigError(message, code="INPUT_CONTRACT_INVALID", path="input_contract")

    if not isinstance(value, Mapping) or set(value) != {"version", "requirements"}:
        raise fail("input contract requires version and requirements")
    if type(value["version"]) is not int or value["version"] != 1:
        raise fail("unsupported input contract version")
    rows = value["requirements"]
    if not isinstance(rows, list):
        raise fail("input requirements must be an array")
    resolved: list[ResolvedInput] = []
    names: set[str] = set()
    fields: set[str] = set()
    for row in rows:
        if not isinstance(row, Mapping) or set(row) != {"spec", "fields"} or not isinstance(row["spec"], Mapping):
            raise fail("input requirement must contain spec and fields")
        if set(row["spec"]) != {"name", "provider_id", "provider_version", "parameters", "attachment",
                                "scene_bindings", "sampling", "missing_data"}:
            raise fail("persisted input specification must be complete")
        spec = InputSpec.from_mapping(row["spec"])
        if spec.name in names:
            raise fail(f"duplicate input instance: {spec.name}")
        names.add(spec.name)
        if not isinstance(row["fields"], list):
            raise fail("input fields must be an array")
        descriptors = []
        for field in row["fields"]:
            if not isinstance(field, Mapping) or set(field) != {
                "name", "shape", "dtype", "unit", "frame", "semantic", "source"}:
                raise fail("invalid input field descriptor")
            descriptor = InputField(**dict(field))
            if descriptor.name.casefold() in fields:
                raise fail(f"duplicate input field: {descriptor.name}")
            fields.add(descriptor.name.casefold())
            descriptors.append(descriptor)
        resolved.append(ResolvedInput(spec, tuple(descriptors)))
    return tuple(resolved)
