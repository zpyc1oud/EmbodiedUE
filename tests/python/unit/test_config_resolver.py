"""Verify Config Resolver behavior at the user-input boundary."""

import json
from dataclasses import dataclass
from typing import cast
from unittest.mock import Mock

import pytest

from uerl import (
    DirectTaskConfig,
    LaunchMode,
    LoggingConfig,
    ProtocolRequirements,
    RslRlRunnerConfig,
    RunConfigResolver,
    SessionConfig,
    WorkerConfig,
)
from uerl.errors import ConfigError
from uerl.tasks.registry import RunnerConfigFactory, TaskConfigFactory, TaskFactory, WorkerConfigFactory


@dataclass(frozen=True)
class _Registration:
    """Provide typed defaults without depending on the future Registry implementation."""

    task_version: str = "1.0"
    environment_id: str = "environment"
    robot_id: str = "robot"
    session_config: SessionConfig = SessionConfig(protocol=ProtocolRequirements(max_minor=1))
    worker_config_factory: WorkerConfigFactory = lambda: WorkerConfig(
        environment_id="environment", robot_id="robot", environment_config={"gravity": 9.81}
    )
    task_config_factory: TaskConfigFactory = lambda: DirectTaskConfig(state_requirements=("state.value",))
    runner_config_factory: RunnerConfigFactory = lambda: RslRlRunnerConfig(max_iterations=1)
    logging_config: LoggingConfig = LoggingConfig()
    task_factory: TaskFactory = Mock()


class _Registry:
    """Resolve one known registration for Resolver tests."""

    def resolve(self, task_id: str) -> _Registration:
        """Return the deterministic registration used by boundary tests."""

        assert task_id == "example"
        return _Registration()


def test_resolve_rejects_worker_robot_asset_path_override() -> None:
    """Keep the internal Worker asset projection out of user override paths."""

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(_Registry()).resolve(
            "example",
            {"worker.robot_asset_path": "/Game/Robots/CartPole/SKM_CartPole"},
        )

    assert raised.value.code == "DERIVED_FIELD_OVERRIDE"
    assert raised.value.path == "worker.robot_asset_path"


def test_resolve_applies_typed_overrides_and_computes_hash() -> None:
    """Resolve defaults and point-path overrides into one immutable config."""

    resolver = RunConfigResolver(_Registry())

    result = resolver.resolve(
        "example",
        {
            "session.mode": "attach",
            "worker.slot_count": "4",
            "worker.environment_config.gravity": "9.8",
        },
    )

    assert result.session.mode is LaunchMode.ATTACH
    assert result.worker.slot_count == 4
    assert result.worker.environment_config["gravity"] == 9.8
    assert len(result.normalized_hash) == 64
    print(f"[VERIFY] VC-001: config_hash={result.normalized_hash} unknown_path=REJECTED")


@pytest.mark.parametrize(
    "value",
    ["4", "[0, 7]", "[-1, 7]", "[7, 1]", "[1, 1.0]", "[1, 2147483648]", "[true, 7]", '["1", 7]'],
)
def test_resolve_requires_an_ordered_int32_decimation_range(value: str) -> None:
    """Reject the old scalar and invalid interval endpoints at the boundary."""

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(_Registry()).resolve("example", {"worker.decimation": value})
    assert raised.value.path is not None
    assert raised.value.path.startswith("worker.decimation")


def test_resolve_accepts_a_fixed_or_variable_decimation_range() -> None:
    """Encode both fixed and variable ranges as the same tuple contract."""

    resolver = RunConfigResolver(_Registry())
    fixed = resolver.resolve("example", {"worker.decimation": "[4,4]"})
    variable = resolver.resolve("example", {"worker.decimation": "[1,7]"})
    assert fixed.worker.decimation == (4, 4)
    assert variable.worker.decimation == (1, 7)


def test_resolve_accepts_valid_terrain_configuration() -> None:
    """Accept the three terrain primitives and preserve the resolved object."""
    terrain = {
        "num_levels": 3,
        "cell_size": [4.0, 4.0],
        "border_width": 1.0,
        "tiers": [
            {"level": 0, "primitive": "plane", "seed": "1", "platform_width": 1.0, "params": {}},
            {
                "level": 1,
                "primitive": "heightfield",
                "seed": "2",
                "platform_width": 1.0,
                "params": {
                    "noise_range": [0.01, 0.06],
                    "noise_step": 0.01,
                    "horizontal_scale": 0.5,
                    "vertical_scale": 0.005,
                    "downsampled_scale": None,
                },
            },
            {
                "level": 2,
                "primitive": "boxes",
                "seed": "3",
                "platform_width": 1.0,
                "params": {
                    "grid_width": 0.5,
                    "grid_height_range": [0.025, 0.1],
                    "holes": False,
                    "perlin_scale": 2.0,
                    "perlin_octaves": 2,
                    "perlin_persistence": 0.5,
                    "perlin_lacunarity": 2.0,
                },
            },
        ],
    }
    result = RunConfigResolver(_Registry()).resolve(
        "example", {"worker.terrain_config": json.dumps(terrain)}
    )
    resolved_terrain = cast(dict[str, object], result.worker.terrain_config)
    assert resolved_terrain["num_levels"] == terrain["num_levels"]
    resolved_tiers = cast(list[dict[str, object]], resolved_terrain["tiers"])
    assert resolved_tiers[0]["seed"] == "1"


def test_resolve_rejects_invalid_terrain_configuration() -> None:
    """Reject terrain tiers that do not cover every contiguous level."""
    terrain = {"num_levels": 1, "cell_size": [4.0, 4.0], "border_width": 0.0, "tiers": []}
    with pytest.raises(ConfigError, match="terrain tiers"):
        RunConfigResolver(_Registry()).resolve("example", {"worker.terrain_config": json.dumps(terrain)})


def test_resolve_rejects_non_canonical_terrain_seed() -> None:
    """Reject seeds that UE cannot parse as canonical unsigned decimals."""
    terrain = {
        "num_levels": 1,
        "cell_size": [4.0, 4.0],
        "border_width": 0.0,
        "tiers": [{"level": 0, "primitive": "plane", "seed": "01", "platform_width": 0.0, "params": {}}],
    }
    with pytest.raises(ConfigError, match="unsigned 64-bit"):
        RunConfigResolver(_Registry()).resolve("example", {"worker.terrain_config": json.dumps(terrain)})


def test_resolve_rejects_unknown_path_at_input_boundary() -> None:
    """Reject a point path that is not part of the typed configuration."""

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(_Registry()).resolve("example", {"worker.unknown": "1"})

    assert raised.value.code == "UNKNOWN_OVERRIDE_PATH"
    assert raised.value.path == "worker.unknown"


def test_resolve_rejects_invalid_cross_partition_value() -> None:
    """Reject a Worker value that violates a boundary constraint."""

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(_Registry()).resolve("example", {"worker.slot_count": "0"})

    assert raised.value.code == "INVALID_WORKER_CONFIG"
    assert raised.value.path == "worker.slot_count"


def test_resolve_rejects_map_object_path_instead_of_world_package() -> None:
    """Require a UE World long package name at the external config boundary."""

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(_Registry()).resolve(
            "example", {"session.map_path": "/Game/Maps/Training.Training"}
        )

    assert raised.value.code == "INVALID_MAP_PATH"
    assert raised.value.path == "session.map_path"


def test_resolve_rejects_non_positive_runner_iterations() -> None:
    """Reject an unset or non-positive generic Runner iteration budget."""

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(_Registry()).resolve("example", {"runner.max_iterations": "0"})

    assert raised.value.code == "INVALID_RUNNER_CONFIG"
    assert raised.value.path == "runner.max_iterations"


def test_resolve_rejects_invalid_task_timeout() -> None:
    """Reject a negative task timeout at the Config input boundary."""

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(_Registry()).resolve("example", {"task.max_episode_steps": "-1"})

    assert raised.value.code == "INVALID_TASK_CONFIG"
    assert raised.value.path == "task.max_episode_steps"


def test_resolve_rejects_non_finite_fault_reward() -> None:
    """Reject a non-finite slot fault reward at the Config input boundary."""

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(_Registry()).resolve("example", {"task.slot_fault_reward": "nan"})

    assert raised.value.code == "INVALID_TYPE"


def test_same_resolved_input_has_same_hash() -> None:
    """Keep normalized hashing deterministic for identical resolved inputs."""

    resolver = RunConfigResolver(_Registry())

    first = resolver.resolve("example", {"worker.slot_count": "2"})
    second = resolver.resolve("example", {"worker.slot_count": "2"})

    assert first.normalized_hash == second.normalized_hash
    assert first.worker is not second.worker
    assert first.task is not second.task
    assert first.runner is not second.runner


@pytest.mark.parametrize(
    ("path", "value"),
    [
        ("worker.environment_id", "other-environment"),
        ("worker.robot_id", "other-robot"),
    ],
)
def test_resolve_rejects_registered_physical_binding_override(path: str, value: str) -> None:
    """Keep user overrides from rebinding a registered UE Environment or Robot."""

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(_Registry()).resolve("example", {path: value})

    assert raised.value.code == "REGISTERED_BINDING_OVERRIDE"
    assert raised.value.path == path
