"""Verify the new training-to-Robot configuration ownership boundary."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Protocol, cast

import pytest

from uerl import ConfigError
from uerl.core.config.canonical import canonical_json
from uerl.core.config.robot import RobotConfig
from uerl.runtime.session.worker import _build_initialize_request
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.tasks.cartpole.config import (
    DEFAULT_CARTPOLE_TRAINING_CONFIG,
    load_cartpole_training_config,
    parse_cartpole_training_config,
)
from uerl.training import build_run_config


class _ActuatorView(Protocol):
    action_scale: float


class _RobotView(Protocol):
    actuators: tuple[_ActuatorView, ...]


class _WorkerView(Protocol):
    robot_asset_path: str
    robot_config_path: Path | None
    robot_config: Mapping[str, object]
    robot_semantics: RobotConfig | None


class _TrainingView(Protocol):
    robot_asset_path: str
    robot_config_path: Path
    robot_config: _RobotView
    worker: _WorkerView


def test_cartpole_config_loads_explicit_robot_reference_and_semantics() -> None:
    """Load the UE asset reference and the logical Robot semantics exactly once."""
    config = cast(_TrainingView, cast(object, load_cartpole_training_config()))
    worker = config.worker

    assert config.robot_asset_path == "/Game/Robots/CartPole/SKM_CartPole"
    assert config.robot_config_path == Path("robots/cartpole/robot.yaml")
    assert config.robot_config.actuators[0].action_scale == 100.0
    assert worker.robot_asset_path == config.robot_asset_path
    assert worker.robot_config_path == config.robot_config_path
    assert worker.robot_config == {}
    semantics = worker.robot_semantics
    assert semantics is not None
    assert semantics.actuators[0].action_scale == 100.0


def test_robot_projection_is_included_in_canonical_config() -> None:
    """Keep the explicit Robot path and semantics in the reproducibility hash input."""
    resolved = build_run_config(CARTPOLE_TASK_ID)
    canonical = canonical_json(resolved)

    assert '"robot_asset_path":"/Game/Robots/CartPole/SKM_CartPole"' in canonical
    assert '"robot_config_path":"robots/cartpole/robot.yaml"' in canonical
    assert '"robot_semantics"' in canonical


def test_robot_reference_is_projected_to_ue_without_training_semantics() -> None:
    """Keep the asset path on the UE projection and keep Robot semantics local."""
    resolved = build_run_config(CARTPOLE_TASK_ID)
    request = _build_initialize_request(resolved, {"state_requirements": [], "action_schema": []})
    worker_projection = cast(dict[str, object], request["worker_config"])
    robot = cast(dict[str, object], worker_projection["robot"])

    assert robot["asset_path"] == "/Game/Robots/CartPole/SKM_CartPole"
    assert "semantics" not in robot
    scalars = cast(dict[str, object], robot["scalars"])
    assert scalars == {}
    assert "robot.cart.mass_kg" not in scalars
    assert "robot.pole.length_m" not in scalars


def test_missing_robot_asset_declaration_fails_at_training_config_boundary(tmp_path: Path) -> None:
    """Fail loudly when the Robot asset declaration key is not registered."""
    text = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "config_path: robots/cartpole/robot.yaml", "config_path: robots/cartpole/missing-robot.yaml"
    )
    training_path = tmp_path / "training.yaml"
    training_path.write_text(text, encoding="utf-8")

    with pytest.raises(ConfigError) as raised:
        load_cartpole_training_config(training_path)

    assert raised.value.code == "ROBOT_ASSET_NOT_FOUND"
    assert raised.value.path == "robot.config_path"


def test_old_task_action_scale_is_not_a_training_yaml_field() -> None:
    """Require actuator-local scaling instead of the removed task duplicate."""
    text = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "  action_clip: 1.0", "  action_scale: 100.0\n  action_clip: 1.0"
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(text, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == "task.action_scale"


def test_old_worker_robot_block_is_not_a_training_yaml_field() -> None:
    """Reject the former user-maintained Worker Robot block."""
    text = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "  run_seed: 0\n", "  robot: {}\n  run_seed: 0\n"
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(text, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == "worker.robot"


@pytest.mark.parametrize("config_path", ["../robot.yaml", "C:/outside/robot.yaml", "/outside/robot.yaml"])
def test_robot_asset_reference_must_be_registered(config_path: str) -> None:
    """Path-like values remain inert identities and cannot select an asset."""
    text = DEFAULT_CARTPOLE_TRAINING_CONFIG.read_text(encoding="utf-8").replace(
        "config_path: robots/cartpole/robot.yaml", f"config_path: '{config_path}'"
    )

    with pytest.raises(ConfigError) as raised:
        parse_cartpole_training_config(text, source=str(DEFAULT_CARTPOLE_TRAINING_CONFIG))

    assert raised.value.code == "ROBOT_ASSET_NOT_FOUND"
    assert raised.value.path == "robot.config_path"


def test_ac_py_unit_generic_003_resolver_rejects_removed_task_action_scale_override() -> None:
    """Keep action scaling exclusively in Robot semantics."""
    from uerl import RunConfigResolver
    from uerl.tasks.registry import create_default_registry

    with pytest.raises(ConfigError) as raised:
        RunConfigResolver(create_default_registry()).resolve(
            CARTPOLE_TASK_ID,
            {"task.action_scale": "50"},
        )

    assert raised.value.code == "UNKNOWN_OVERRIDE_PATH"
    assert raised.value.path == "task.action_scale"
