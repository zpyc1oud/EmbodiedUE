"""Verify the public Config model boundary."""

from dataclasses import FrozenInstanceError

import pytest

from uerl import (
    DirectTaskConfig,
    LaunchMode,
    LoggingConfig,
    ResolvedRunConfig,
    RslRlRunnerConfig,
    SessionConfig,
    WorkerConfig,
)


def _resolved_config() -> ResolvedRunConfig:
    """Build a representative resolved model without invoking the Resolver."""

    return ResolvedRunConfig(
        task_id="example",
        task_version="1.0",
        session=SessionConfig(mode=LaunchMode.ATTACH),
        worker=WorkerConfig(environment_id="environment", robot_id="robot"),
        task=DirectTaskConfig(state_requirements=("state.value",)),
        runner=RslRlRunnerConfig(),
        logging=LoggingConfig(),
        normalized_hash="a" * 64,
    )


def test_config_partitions_are_frozen() -> None:
    """Keep resolved partitions immutable after construction."""

    config = _resolved_config()

    with pytest.raises(FrozenInstanceError):
        config.task_id = "changed"  # type: ignore[misc]

    with pytest.raises(FrozenInstanceError):
        config.session = SessionConfig()  # type: ignore[misc]


def test_nested_config_mappings_are_frozen() -> None:
    """Keep nested Config values stable after resolution."""

    source = {"limits": {"max_force": 1.0}}
    config = WorkerConfig(environment_config=source)
    source["limits"]["max_force"] = 2.0

    assert config.environment_config["limits"]["max_force"] == 1.0  # type: ignore[index]
    with pytest.raises(TypeError):
        config.environment_config["limits"]["max_force"] = 3.0  # type: ignore[index]


def test_session_is_one_partition_of_the_complete_config() -> None:
    """Keep session settings separate from Worker and runner settings."""

    config = _resolved_config()

    assert config.session.mode is LaunchMode.ATTACH
    # Worker spawn and Slot initialization, not a single socket read, set this budget.
    assert config.session.connect_timeout_s == 120.0
    assert config.session.request_timeout_s == 120.0
    assert config.worker.environment_id == "environment"
    assert config.worker.robot_asset_path == ""
    assert config.runner.device == "cpu"
    assert config.logging.run_directory.name == "runs"
