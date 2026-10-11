"""Existing-state selection preserves the host's declared physical meaning."""

from dataclasses import replace

import pytest
import yaml

from uerl.core.inputs import InputField, InputRegistry, InputSpec, StateInputFactory
from uerl.errors import ConfigError


def catalog() -> list[InputField]:
    return [
        InputField("body_velocity", (2, 3), "m/s", "ground", "linear_velocity", "robot"),
        InputField("support", (2,), "1", "world", "geometric_support", "robot"),
    ]


def test_selection_preserves_metadata_and_requested_order_through_yaml() -> None:
    registry = InputRegistry()
    fields = catalog()
    registry.register(StateInputFactory(fields))
    fields.clear()  # The factory owns its catalog, not the caller's list.
    spec = InputSpec("proprioception", "uerl.state", 1, parameters={"fields": ["SUPPORT", "body_velocity"]})
    result = registry.compile([spec])
    assert [item.offset for item in result.fields] == [0, 2]
    assert result.width == 8
    assert [item.descriptor.to_mapping() for item in result.fields] == [
        {
            "name": "support",
            "shape": [2],
            "dtype": "float32",
            "unit": "1",
            "frame": "world",
            "semantic": "geometric_support",
            "source": "robot",
        },
        {
            "name": "body_velocity",
            "shape": [2, 3],
            "dtype": "float32",
            "unit": "m/s",
            "frame": "ground",
            "semantic": "linear_velocity",
            "source": "robot",
        },
    ]
    effective = result.inputs[0].spec
    assert effective.parameters["fields"] == ("support", "body_velocity")
    restored = InputSpec.from_mapping(yaml.safe_load(yaml.safe_dump(effective.to_mapping())))
    assert registry.compile([restored]) == result


@pytest.mark.parametrize(
    "parameters",
    [
        {},
        {"fields": []},
        {"fields": "support"},
        {"fields": [False]},
        {"fields": [" support"]},
        {"fields": ["missing"]},
        {"fields": ["support", "SUPPORT"]},
        {"fields": ["support"], "scale": 100},
    ],
)
def test_invalid_selection_is_rejected_with_a_field_path(parameters: dict[str, object]) -> None:
    factory = StateInputFactory(catalog())
    with pytest.raises(ConfigError) as error:
        factory.validate(InputSpec("state", "uerl.state", 1, parameters=parameters))
    assert error.value.code == "STATE_INPUT_INVALID"
    assert error.value.path is not None
    assert error.value.path.startswith("inputs.state.parameters")


@pytest.mark.parametrize(
    "change",
    [
        {"attachment": "root"},
        {"scene_bindings": ["floor"]},
        {"provider_id": "other"},
        {"provider_version": 2},
    ],
)
def test_state_selection_cannot_reinterpret_frame_or_provider(change: dict[str, object]) -> None:
    spec = InputSpec.from_mapping(
        {
            "name": "state",
            "provider_id": "uerl.state",
            "provider_version": 1,
            "parameters": {"fields": ["support"]},
            **change,
        }
    )
    with pytest.raises(ConfigError) as error:
        StateInputFactory(catalog()).validate(spec)
    assert error.value.code == "STATE_INPUT_INVALID"


def test_catalog_and_compiled_set_reject_duplicate_producers() -> None:
    fields = catalog()
    with pytest.raises(ConfigError, match="duplicate state field"):
        StateInputFactory([fields[0], replace(fields[0], name="BODY_VELOCITY")])
    registry = InputRegistry()
    registry.register(StateInputFactory(fields))
    with pytest.raises(ConfigError, match="multiple producers"):
        registry.compile(
            [
                InputSpec("a", "uerl.state", 1, parameters={"fields": ["support"]}),
                InputSpec("b", "uerl.state", 1, parameters={"fields": ["support"]}),
            ]
        )
