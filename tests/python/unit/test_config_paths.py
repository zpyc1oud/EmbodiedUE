"""Verify canonical config-root-relative path identity and safety."""

from __future__ import annotations

from pathlib import Path

import pytest

from uerl import ConfigError
from uerl.core.config.paths import ConfigPathResolver
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.tasks.phantomx import PHANTOMX_TERRAIN_TASK_ID
from uerl.training import build_run_config


def test_resolver_returns_logical_identity_and_absolute_read_path(tmp_path: Path) -> None:
    root = tmp_path / "configs"
    target = root / "robots" / "cartpole" / "robot.yaml"
    target.parent.mkdir(parents=True)
    _ = target.write_text("robot: true\n", encoding="utf-8")

    resolved = ConfigPathResolver(root).resolve("robots/cartpole/robot.yaml", path="robot.config_path")

    assert resolved.logical_path == "robots/cartpole/robot.yaml"
    assert resolved.absolute_path == target.resolve()


@pytest.mark.parametrize(
    "value",
    ["../robot.yaml", "/outside/robot.yaml", "C:/outside/robot.yaml", ""],
)
def test_resolver_rejects_non_root_relative_paths(tmp_path: Path, value: str) -> None:
    with pytest.raises(ConfigError, match="config path") as raised:
        ConfigPathResolver(tmp_path / "configs").resolve(value, path="robot.config_path")

    assert raised.value.code == "INVALID_CONFIG_PATH"
    assert raised.value.path == "robot.config_path"


def test_config_identity_is_independent_of_cwd(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    first = build_run_config(CARTPOLE_TASK_ID)
    monkeypatch.chdir(tmp_path)
    second = build_run_config(CARTPOLE_TASK_ID)

    assert first.normalized_hash == second.normalized_hash
    robot_config_path = first.worker.robot_config_path
    assert robot_config_path is not None
    assert robot_config_path == Path("robots/cartpole/robot.yaml")
    assert not robot_config_path.is_absolute()


def test_terrain_source_identity_is_logical() -> None:
    config = build_run_config(PHANTOMX_TERRAIN_TASK_ID)

    terrain_config_path = config.worker.terrain_config_path
    assert terrain_config_path is not None
    assert terrain_config_path == Path("environments/terrains/phantomx/continuous.yaml")
    assert not terrain_config_path.is_absolute()


def test_resolver_rejects_symlink_escape(tmp_path: Path) -> None:
    root = tmp_path / "configs"
    root.mkdir()
    outside = tmp_path / "outside.yaml"
    _ = outside.write_text("robot: true\n", encoding="utf-8")
    link = root / "robot.yaml"
    try:
        link.symlink_to(outside)
    except (OSError, NotImplementedError) as exc:
        pytest.skip(f"symlink creation unavailable: {exc}")

    with pytest.raises(ConfigError, match="escapes") as raised:
        ConfigPathResolver(root).resolve("robot.yaml", path="robot.config_path")

    assert raised.value.code == "INVALID_CONFIG_PATH"
