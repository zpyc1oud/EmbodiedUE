"""Check provider contracts with independent, externally defined factories."""

from dataclasses import replace

import pytest
import yaml

from uerl.core.inputs import InputField, InputRegistry, InputSpec, ResolvedInput
from uerl.errors import ConfigError


class ConstantFactory:
    provider_id = "example.constant"
    version = 1

    def validate(self, spec: InputSpec) -> ResolvedInput:
        unknown = set(spec.parameters) - {"value"}
        if unknown:
            raise ConfigError(f"unknown constant parameters: {unknown}", code="TEST_PARAMETER")
        effective = replace(spec, parameters={"value": spec.parameters.get("value", 3.0)})
        return ResolvedInput(
            effective, (InputField(f"input.{spec.name}.value", (2, 3), "m", "body", "position", self.provider_id),)
        )


def test_nested_configuration_is_frozen_and_yaml_round_trip_preserves_types() -> None:
    raw = {"pattern": [[1.0, 2.0], [3.0, 4.0]], "flags": {"enabled": True}}
    spec = InputSpec("scan", "example.ray", 1, parameters=raw, scene_bindings=("ground",))
    raw["pattern"][0][0] = 90.0  # type: ignore[index]
    restored = InputSpec.from_mapping(yaml.safe_load(yaml.safe_dump(spec.to_mapping())))
    assert restored.to_mapping()["parameters"] == {
        "pattern": [[1.0, 2.0], [3.0, 4.0]],
        "flags": {"enabled": True},
    }
    with pytest.raises(TypeError):
        spec.parameters["other"] = 1  # type: ignore[index]


def test_external_factory_compiles_literal_offsets_and_effective_defaults() -> None:
    registry = InputRegistry()
    registry.register(ConstantFactory())
    result = registry.compile(
        [InputSpec("left", "example.constant", 1), InputSpec("right", "example.constant", 1, parameters={"value": 9.0})]
    )
    assert [binding.offset for binding in result.fields] == [0, 6]
    assert [binding.descriptor.name for binding in result.fields] == ["input.left.value", "input.right.value"]
    assert result.width == 12
    assert result.inputs[0].spec.parameters["value"] == 3.0
    assert result.inputs[1].spec.parameters["value"] == 9.0


def test_unavailable_version_and_duplicate_registration_fail_before_binding() -> None:
    registry = InputRegistry()
    registry.register(ConstantFactory())
    with pytest.raises(ConfigError, match="already registered"):
        registry.register(ConstantFactory())
    with pytest.raises(ConfigError, match="not installed or version"):
        registry.compile([InputSpec("scan", "example.constant", 2)])
    with pytest.raises(ConfigError, match="duplicate input instance"):
        registry.compile([InputSpec("same", "example.constant", 1)] * 2)


@pytest.mark.parametrize(
    "change",
    [
        {"sampling": "async"},
        {"missing_data": "zero"},
        {"provider_version": True},
        {"scene_bindings": ("ground", "ground")},
        {"parameters": {"height": float("nan")}},
    ],
)
def test_unsupported_contracts_fail_with_field_path(change: dict[str, object]) -> None:
    values = {"name": "scan", "provider_id": "example.constant", "provider_version": 1, **change}
    with pytest.raises(ConfigError) as error:
        InputSpec.from_mapping(values)
    assert error.value.path


def test_factory_cannot_change_identity_or_publish_duplicate_outputs() -> None:
    class IdentityFactory(ConstantFactory):
        def validate(self, spec: InputSpec) -> ResolvedInput:
            return super().validate(replace(spec, name="changed"))

    registry = InputRegistry()
    registry.register(IdentityFactory())
    with pytest.raises(ConfigError, match="changed input identity"):
        registry.compile([InputSpec("scan", "example.constant", 1)])

    class DuplicateFactory(ConstantFactory):
        def validate(self, spec: InputSpec) -> ResolvedInput:
            return ResolvedInput(spec, (InputField("same", (1,), "1", "body", "value", self.provider_id),))

    registry = InputRegistry()
    registry.register(DuplicateFactory())
    with pytest.raises(ConfigError, match="multiple producers"):
        registry.compile([InputSpec("a", "example.constant", 1), InputSpec("b", "example.constant", 1)])


def test_field_shape_and_dtype_have_explicit_numeric_bounds() -> None:
    with pytest.raises(ConfigError, match="positive integers"):
        InputField("x", (True,), "m", "body", "position", "test")
    with pytest.raises(ConfigError, match="int32"):
        InputField("x", (2**30, 4), "m", "body", "position", "test")
    with pytest.raises(ConfigError, match="float32"):
        InputField("x", (2,), "m", "body", "position", "test", dtype="uint8")


def test_case_only_output_duplicates_match_native_field_name_rules() -> None:
    spec = InputSpec("scan", "example.constant", 1)
    with pytest.raises(ConfigError, match="unique"):
        ResolvedInput(
            spec,
            (
                InputField("Height", (1,), "m", "body", "height", "test"),
                InputField("height", (1,), "m", "body", "height", "test"),
            ),
        )


def test_provider_parameter_typo_is_not_silently_ignored() -> None:
    registry = InputRegistry()
    registry.register(ConstantFactory())
    with pytest.raises(ConfigError, match="unknown constant parameters"):
        registry.compile([InputSpec("scan", "example.constant", 1, parameters={"vaule": 9.0})])


def test_ray_factory_records_effective_parameters_and_distinct_output_meaning() -> None:
    from uerl.core.inputs import RayGroundFactory

    registry = InputRegistry()
    registry.register(RayGroundFactory())
    result = registry.compile(
        [
            InputSpec(
                "scan", "uerl.ray_ground", 1, attachment="root", parameters={"offsets_m": [[0.0, 0.0], [0.75, 0.0]]}
            ),
            InputSpec("clearance", "uerl.ray_ground", 1, attachment="root", parameters={"output": "clearance"}),
        ]
    )
    assert result.inputs[0].spec.to_mapping()["parameters"] == {
        "offsets_m": [[0.0, 0.0], [0.75, 0.0]],
        "start_height_m": 1.0,
        "end_depth_m": 2.0,
        "alignment": "yaw",
        "output": "height",
    }
    assert result.inputs[0].spec.scene_bindings == ("ground",)
    assert result.fields[0].descriptor.shape == (2,)
    assert result.fields[0].descriptor.semantic == "terrain_height"
    assert result.fields[1].descriptor.semantic == "ground_clearance"
    assert result.fields[1].offset == 2


@pytest.mark.parametrize(
    "parameters",
    [
        {"offsets_m": []},
        {"offsets_m": [[1.0, 2.0, 3.0]]},
        {"alignment": "full"},
        {"output": "camera"},
        {"start_height_m": -1.0},
        {"start_height_m": True},
        {"start_heigth_m": 1.0},
    ],
)
def test_ray_factory_rejects_unsupported_parameter_values(parameters: dict[str, object]) -> None:
    from uerl.core.inputs import RayGroundFactory

    with pytest.raises(ConfigError):
        RayGroundFactory().validate(InputSpec("scan", "uerl.ray_ground", 1, attachment="root", parameters=parameters))
