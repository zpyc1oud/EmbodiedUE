"""An external declaration factory; native sampling is supplied by ExampleInput."""

import struct
from dataclasses import replace
from importlib.resources import files

from uerl.core.config.yaml_loader import load_unique_yaml
from uerl.core.inputs import InputField, InputRegistry, InputSpec, ResolvedInput
from uerl.errors import ConfigError


class ConstantFactory:
    provider_id = "example.constant"
    version = 1

    def validate(self, spec: InputSpec) -> ResolvedInput:
        if spec.attachment is not None or spec.scene_bindings:
            raise ConfigError("constant input does not use scene bindings", code="EXAMPLE_INPUT_BINDING")
        if set(spec.parameters) - {"value"}:
            raise ConfigError("constant input accepts only value", code="EXAMPLE_INPUT_PARAMETER")
        value = spec.parameters.get("value", 1.0)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise ConfigError("constant value must be numeric", code="EXAMPLE_INPUT_PARAMETER")
        if abs(value) > 3.4028234663852886e38:
            raise ConfigError("constant value must fit float32", code="EXAMPLE_INPUT_PARAMETER")
        # InputSpec has already rejected non-finite configuration values.
        return ResolvedInput(
            replace(spec, parameters={"value": struct.unpack("<f", struct.pack("<f", value))[0]}),
            (InputField(f"input.{spec.name}.value", (1,), "1", "none", "constant", self.provider_id),),
        )


def register_inputs(registry: InputRegistry) -> None:
    registry.register(ConstantFactory())


def example_spec() -> InputSpec:
    payload = load_unique_yaml(files(__package__).joinpath("input.yaml").read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("input.yaml must contain a mapping")
    return InputSpec.from_mapping(payload)
