"""Export and play rebuild Robot, timing and Task settings from the Run that trained them."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from uerl.application.run_config import resolve_run_config
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


@pytest.mark.parametrize("explicit_map", [None, "/Game/Maps/Override"])
def test_saved_map_is_restored_without_reusing_training_endpoint(
    tmp_path: Path, explicit_map: str | None,
) -> None:
    config = build_run_config(CARTPOLE_TASK_ID)
    payload = to_jsonable(config)
    payload["session"]["map_path"] = "/Game/Maps/Recorded"
    payload["session"]["port"] = 44000
    payload["logging"]["run_directory"] = "training-evidence"
    run = _write_run(tmp_path, payload)
    before = (run / "resolved_config.json").read_bytes()
    overrides = {} if explicit_map is None else {"session.map_path": explicit_map}

    resolved = resolve_run_config(CARTPOLE_TASK_ID, run, overrides)

    assert resolved.config.session.map_path == (explicit_map or "/Game/Maps/Recorded")
    assert resolved.config.session.port == config.session.port
    assert resolved.config.logging.run_directory == config.logging.run_directory
    assert (run / "resolved_config.json").read_bytes() == before


@pytest.mark.parametrize("missing_reward", [False, True])
def test_saved_fields_survive_changed_defaults_but_missing_fields_use_current_template(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, missing_reward: bool,
) -> None:
    from dataclasses import replace

    from uerl.tasks.cartpole import registration
    from uerl.tasks.cartpole.config import load_cartpole_training_config

    original = load_cartpole_training_config()
    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    payload["task"]["rew_scale_alive"] = 0.75
    payload["worker"]["decimation"] = [3, 3]
    payload["runner"]["parameters"]["hidden_dims"] = [64, 64]
    if missing_reward:
        del payload["task"]["rew_scale_alive"]
    run = _write_run(tmp_path, payload)
    changed = replace(original, task=replace(original.task, rew_scale_alive=9.0))
    monkeypatch.setattr(registration, "load_cartpole_training_config", lambda: changed)

    resolved = resolve_run_config(CARTPOLE_TASK_ID, run, {})

    assert to_jsonable(resolved.config.task)["rew_scale_alive"] == (9.0 if missing_reward else 0.75)
    assert resolved.config.worker.decimation == (3, 3)
    assert resolved.config.runner.parameters["hidden_dims"] == (64, 64)


@pytest.mark.parametrize("case", ["identity", "unknown_field", "wrong_type", "missing_partition"])
def test_invalid_saved_configuration_is_rejected(tmp_path: Path, case: str) -> None:
    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    if case == "identity":
        del payload["normalized_hash"]
    elif case == "unknown_field":
        payload["task"]["unknown_reward"] = 1.0
    elif case == "wrong_type":
        payload["worker"]["slot_count"] = "eight"
    else:
        del payload["worker"]
    run = _write_run(tmp_path, payload)

    with pytest.raises(ConfigError) as error:
        resolve_run_config(CARTPOLE_TASK_ID, run, {})

    assert error.value.code == "INVALID_RUN_CONFIG"
