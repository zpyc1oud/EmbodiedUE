"""Export and play rebuild Robot, timing and Task settings from the Run that trained them."""

from __future__ import annotations

import json
from pathlib import Path

import pytest
import torch
import yaml

from tests.python.run_config_files import write_resolved_config
from uerl.application import run_config
from uerl.application.run_config import resolve_run_config
from uerl.core.config import RunConfigResolver
from uerl.core.config.canonical import to_jsonable
from uerl.core.config.snapshot import CHECKPOINT_CONFIG_KEY, resolved_config_to_yaml
from uerl.errors import ConfigError
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config


def _write_run(tmp_path: Path, payload: dict[str, object]) -> Path:
    run = tmp_path / "run"
    run.mkdir()
    write_resolved_config(run, payload)
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


def test_strict_restore_identifies_task_from_run_and_keeps_its_semantics(tmp_path: Path) -> None:
    config = build_run_config(CARTPOLE_TASK_ID)
    payload = to_jsonable(config)
    payload["task"]["rew_scale_alive"] = 0.75
    payload["worker"]["decimation"] = [3, 3]
    run = _write_run(tmp_path, payload)

    resolved = resolve_run_config(None, run, {}, strict=True)

    assert resolved.from_run
    assert resolved.config.task_id == CARTPOLE_TASK_ID
    assert to_jsonable(resolved.config.task)["rew_scale_alive"] == 0.75
    assert resolved.config.worker.decimation == (3, 3)
    assert resolved.config.runner.parameters["hidden_dims"] == config.runner.parameters["hidden_dims"]
    assert resolved.config.worker.robot_semantics == config.worker.robot_semantics


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
    before = (run / "resolved_config.yaml").read_bytes()
    overrides = {} if explicit_map is None else {"session.map_path": explicit_map}

    resolved = resolve_run_config(CARTPOLE_TASK_ID, run, overrides)

    assert resolved.config.session.map_path == (explicit_map or "/Game/Maps/Recorded")
    assert resolved.config.session.port == config.session.port
    assert resolved.config.logging.run_directory == config.logging.run_directory
    assert (run / "resolved_config.yaml").read_bytes() == before


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


def test_strict_restore_uses_saved_reward_and_timing_when_current_defaults_change(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from dataclasses import replace

    from uerl.tasks.cartpole import registration
    from uerl.tasks.cartpole.config import load_cartpole_training_config

    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    payload["task"]["rew_scale_alive"] = 0.75
    payload["worker"]["decimation"] = [3, 3]
    run = _write_run(tmp_path, payload)
    original = load_cartpole_training_config()
    changed = replace(
        original,
        task=replace(original.task, rew_scale_alive=9.0),
        worker=replace(original.worker, decimation=(5, 5)),
    )
    monkeypatch.setattr(registration, "load_cartpole_training_config", lambda: changed)
    before = (run / "resolved_config.yaml").read_bytes()

    resolved = resolve_run_config(None, run, {}, strict=True)

    assert to_jsonable(resolved.config.task)["rew_scale_alive"] == 0.75
    assert resolved.config.worker.decimation == (3, 3)
    assert (run / "resolved_config.yaml").read_bytes() == before


@pytest.mark.parametrize("missing", ["file", "task_id", "task_version", "normalized_hash", "worker", "task_field"])
def test_strict_restore_rejects_missing_run_metadata_without_default_fallback(
    tmp_path: Path,
    missing: str,
) -> None:
    payload: dict[str, object] | None = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    if missing == "file":
        payload = None
    else:
        assert payload is not None
        if missing == "task_field":
            assert isinstance(payload["task"], dict)
            del payload["task"]["rew_scale_alive"]
        else:
            del payload[missing]
    run = tmp_path / "run"
    run.mkdir()
    if payload is not None:
        write_resolved_config(run, payload)

    with pytest.raises(ConfigError) as error:
        resolve_run_config(CARTPOLE_TASK_ID, run, {}, strict=True)

    assert error.value.code == "MISSING_RUN_CONFIG"


def test_strict_restore_checks_explicit_task_against_run_identity(tmp_path: Path) -> None:
    run = _write_run(tmp_path, to_jsonable(build_run_config(CARTPOLE_TASK_ID)))

    with pytest.raises(ConfigError) as error:
        resolve_run_config("UERL-PhantomX-Walk-v0", run, {}, strict=True)

    assert error.value.code == "RUN_TASK_MISMATCH"


def test_strict_restore_rejects_a_changed_task_version(tmp_path: Path) -> None:
    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    payload["task_version"] = "0.0.0"
    run = _write_run(tmp_path, payload)

    with pytest.raises(ConfigError) as error:
        resolve_run_config(None, run, {}, strict=True)

    assert error.value.code == "RUN_TASK_MISMATCH"


def test_strict_restore_resolves_enabled_external_task_identity(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from dataclasses import replace

    from uerl.tasks.registry import TaskRegistry, create_default_registry

    external_task_id = "example.external-cartpole-v0"
    built_in = create_default_registry().resolve(CARTPOLE_TASK_ID)
    registry = TaskRegistry()
    registry.register(replace(built_in, task_id=external_task_id, task_version="0.1.0"))
    payload = to_jsonable(RunConfigResolver(registry).resolve(external_task_id, {}))
    monkeypatch.setattr(run_config, "create_default_registry", lambda: registry)
    run = _write_run(tmp_path, payload)

    resolved = resolve_run_config(None, run, {}, strict=True)

    assert resolved.config.task_id == external_task_id


def test_checkpoint_finds_ancestor_run_config(tmp_path: Path) -> None:
    run = tmp_path / "run"
    checkpoint = run / "rsl_rl" / "model_10.pt"
    checkpoint.parent.mkdir(parents=True)
    checkpoint.write_bytes(b"checkpoint")
    (run / "resolved_config.yaml").write_text("not parsed by path discovery\n", encoding="utf-8")

    assert run_config.find_run_directory_for_checkpoint(checkpoint) == run


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


def test_versioned_yaml_sidecar_restores_typed_reward_and_timing(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from dataclasses import replace

    from uerl.tasks.cartpole import registration
    from uerl.tasks.cartpole.config import load_cartpole_training_config

    config = build_run_config(
        CARTPOLE_TASK_ID,
        overrides={"task.rew_scale_alive": "0.75", "worker.decimation": "[3, 3]"},
    )
    run = tmp_path / "run"
    run.mkdir()
    sidecar = run / "resolved_config.yaml"
    sidecar.write_text(resolved_config_to_yaml(config), encoding="utf-8")
    before = sidecar.read_bytes()
    current = load_cartpole_training_config()
    changed = replace(
        current,
        task=replace(current.task, rew_scale_alive=9.0),
        worker=replace(current.worker, decimation=(5, 5)),
    )
    monkeypatch.setattr(registration, "load_cartpole_training_config", lambda: changed)

    restored = resolve_run_config(None, run, {}, strict=True)

    assert to_jsonable(restored.config.task)["rew_scale_alive"] == 0.75
    assert restored.config.worker.decimation == (3, 3)
    assert sidecar.read_bytes() == before


def test_detached_checkpoint_restores_config_and_rejects_disagreeing_sidecar(tmp_path: Path) -> None:
    saved = build_run_config(CARTPOLE_TASK_ID, overrides={"worker.decimation": "[3, 3]"})
    checkpoint = tmp_path / "detached.pt"
    torch.save({"infos": {CHECKPOINT_CONFIG_KEY: resolved_config_to_yaml(saved)}}, checkpoint)

    restored = run_config.load_run_config_source(None, None, strict=True, checkpoint=checkpoint)
    assert restored.recorded is not None
    assert restored.recorded.worker.decimation == (3, 3)

    run = tmp_path / "run"
    run.mkdir()
    conflicting = build_run_config(CARTPOLE_TASK_ID, overrides={"worker.decimation": "[5, 5]"})
    (run / "resolved_config.yaml").write_text(resolved_config_to_yaml(conflicting), encoding="utf-8")
    with pytest.raises(ConfigError) as error:
        run_config.load_run_config_source(None, run, strict=True, checkpoint=checkpoint)
    assert error.value.code == "RUN_CONFIG_CONFLICT"


def test_json_only_run_config_is_rejected_and_left_unchanged(tmp_path: Path) -> None:
    run = tmp_path / "run"
    run.mkdir()
    old_json = run / "resolved_config.json"
    old_json.write_text(json.dumps(to_jsonable(build_run_config(CARTPOLE_TASK_ID))), encoding="utf-8")
    before = old_json.read_bytes()

    with pytest.raises(ConfigError) as error:
        resolve_run_config(None, run, {}, strict=True)
    assert error.value.code == "UNSUPPORTED_RUN_CONFIG_FORMAT"
    assert old_json.read_bytes() == before


def test_unversioned_yaml_sidecar_requires_explicit_migration(tmp_path: Path) -> None:
    run = tmp_path / "run"
    run.mkdir()
    old_yaml = run / "resolved_config.yaml"
    old_yaml.write_text(yaml.safe_dump(to_jsonable(build_run_config(CARTPOLE_TASK_ID))), encoding="utf-8")
    before = old_yaml.read_bytes()

    with pytest.raises(ConfigError) as error:
        resolve_run_config(None, run, {}, strict=True)

    assert error.value.code == "INVALID_RUN_CONFIG"
    assert "run_migration" in str(error.value)
    assert old_yaml.read_bytes() == before


def test_manifest_yaml_is_a_recovery_source_for_run_identity(tmp_path: Path) -> None:
    config = build_run_config(CARTPOLE_TASK_ID, overrides={"worker.decimation": "[3, 3]"})
    run = tmp_path / "run"
    run.mkdir()
    (run / "manifest.yaml").write_text(
        yaml.safe_dump({"resolved_config": to_jsonable(config)}, sort_keys=False),
        encoding="utf-8",
    )

    restored = resolve_run_config(None, run, {}, strict=True)
    assert restored.config.worker.decimation == (3, 3)


def test_uniform_phantomx_command_overrides_survive_saved_run(tmp_path: Path) -> None:
    from uerl.tasks.phantomx.config import PHANTOMX_TASK_ID, PhantomXCommandSampling, PhantomXTaskConfig

    config = build_run_config(PHANTOMX_TASK_ID, overrides={
        "task.command.sampling": "uniform_velocity",
        "task.command.initial_speed_min": "-1.0",
        "task.command.initial_speed_max": "1.0",
        "task.command.lateral_speed_min": "-1.0",
        "task.command.lateral_speed_max": "1.0",
        "task.command.standing_probability": "0.02",
        "task.command.resampling_time_min_s": "10.0",
        "task.command.resampling_time_max_s": "10.0",
    })
    run = _write_run(tmp_path, to_jsonable(config))
    restored = resolve_run_config(PHANTOMX_TASK_ID, run, {}).config
    assert restored.task == config.task
    assert isinstance(restored.task, PhantomXTaskConfig)
    assert restored.task.command.sampling is PhantomXCommandSampling.UNIFORM_VELOCITY
    assert restored.task.command.initial_speed_min == -1.0
    assert restored.task.command.lateral_speed_max == 1.0
    assert restored.task.command.standing_probability == 0.02
