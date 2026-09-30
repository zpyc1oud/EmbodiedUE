"""Export and play rebuild Robot, timing and Task settings from the Run that trained them."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from uerl.cli.run_config import resolve_run_config
from uerl.core.config.canonical import to_jsonable
from uerl.errors import ConfigError
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config


def _write_run(tmp_path: Path, payload: dict[str, object]) -> Path:
    run = tmp_path / "run"
    run.mkdir()
    (run / "resolved_config.json").write_text(json.dumps(payload), encoding="utf-8")
    return run


def test_resolve_run_config_keeps_recorded_robot_and_training_hash(tmp_path: Path) -> None:
    config = build_run_config(CARTPOLE_TASK_ID, overrides={"worker.slot_count": "8"})
    payload = to_jsonable(config)
    payload["worker"]["robot_semantics"]["actuators"][0]["action_scale"] = 0.4
    payload["worker"]["robot_semantics"]["actuators"][0]["stiffness"] = 0.0
    payload["session"]["host"] = "10.0.0.8"
    payload["session"]["port"] = 44000
    run = _write_run(tmp_path, payload)

    resolved = resolve_run_config(
        CARTPOLE_TASK_ID,
        run,
        {"worker.slot_count": "1", "session.port": "0"},
    )

    semantics = resolved.config.worker.robot_semantics
    assert semantics is not None
    actuator = semantics.actuators[0]
    assert resolved.from_run
    assert resolved.run_hash == config.normalized_hash
    assert actuator.action_scale == 0.4
    assert actuator.stiffness == 0.0
    assert resolved.config.worker.slot_count == 1
    assert resolved.config.worker.physics_dt == config.worker.physics_dt
    assert resolved.config.worker.decimation == config.worker.decimation
    assert resolved.config.session.port == 0
    assert resolved.config.session.host == config.session.host


def test_resolve_run_config_uses_task_defaults_without_a_recorded_config(tmp_path: Path) -> None:
    resolved = resolve_run_config(CARTPOLE_TASK_ID, tmp_path, {"worker.slot_count": "2"})

    assert not resolved.from_run
    assert resolved.config.worker.slot_count == 2
    assert resolved.run_hash == resolved.config.normalized_hash


def test_resolve_run_config_rejects_a_different_task(tmp_path: Path) -> None:
    config = build_run_config(CARTPOLE_TASK_ID)
    run = _write_run(tmp_path, to_jsonable(config))

    with pytest.raises(ConfigError, match="not 'UERL-PhantomX-Walk-v0'"):
        resolve_run_config("UERL-PhantomX-Walk-v0", run, {})
