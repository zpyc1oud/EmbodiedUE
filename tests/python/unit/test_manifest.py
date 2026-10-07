"""Verify atomic Run Manifest recording at the filesystem boundary."""

from pathlib import Path

import pytest

from uerl import (
    DirectTaskConfig,
    LoggingConfig,
    ResolvedRunConfig,
    RslRlRunnerConfig,
    RunManifest,
    RunRecorder,
    SessionConfig,
    WorkerConfig,
)
from uerl.core.config import manifest as manifest_module
from uerl.core.config.manifest import GitIdentityError, capture_git_identity
from uerl.core.config.snapshot import resolved_config_from_yaml
from uerl.core.config.yaml_loader import load_unique_yaml


def _config() -> ResolvedRunConfig:
    """Build a complete resolved Config for Manifest tests."""

    return ResolvedRunConfig(
        task_id="task",
        task_version="1.0",
        session=SessionConfig(),
        worker=WorkerConfig(
            environment_id="environment",
            robot_id="robot",
            robot_config_path=Path("robots/cartpole/robot.yaml"),
            terrain_config_path=Path("environments/terrains/phantomx/continuous.yaml"),
            run_seed=7,
        ),
        task=DirectTaskConfig(state_requirements=("state.value",)),
        runner=RslRlRunnerConfig(),
        logging=LoggingConfig(),
        normalized_hash="b" * 64,
    )


def _manifest(run_directory: Path) -> RunManifest:
    """Build a Manifest whose contract is independent of its output directory."""

    return RunManifest(
        resolved_config=_config(),
        run_seed=7,
        run_directory=run_directory,
        effective_worker_config={"physics_dt": 1.0 / 60.0, "slot_count": 1},
        build_identity={"build_id": "ue-build"},
        selected_protocol={"major": 2, "minor": 0},
        schema_hashes={"state": "c" * 64, "action": "e" * 64},
        layout_hashes={"initial_state": "d" * 64},
        seed_derivation_version="seed-v1",
        git_identity={"commit": "a" * 40, "ref": "feature/test", "dirty": False},
    )


def test_manifest_identity_excludes_only_local_run_directory(tmp_path: Path) -> None:
    first_directory = tmp_path / "first"
    second_directory = tmp_path / "second"
    first_hash = RunRecorder(first_directory).write_manifest_atomic(_manifest(first_directory))
    second_hash = RunRecorder(second_directory).write_manifest_atomic(_manifest(second_directory))

    assert first_hash == second_hash
    first_payload = load_unique_yaml((first_directory / "manifest.yaml").read_text(encoding="utf-8"))
    assert isinstance(first_payload, dict)
    assert first_payload["resolved_config"]["worker"]["robot_config_path"] == "robots/cartpole/robot.yaml"
    assert first_payload["resolved_config"]["worker"]["terrain_config_path"] == (
        "environments/terrains/phantomx/continuous.yaml"
    )
    assert first_payload["git_identity"] == {
        "commit": "a" * 40,
        "ref": "feature/test",
        "dirty": False,
    }


def test_capture_git_identity_reads_the_source_checkout_outside_current_directory(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    monkeypatch.chdir(tmp_path)
    identity = capture_git_identity()

    assert isinstance(identity["commit"], str)
    assert len(identity["commit"]) == 40
    assert isinstance(identity["ref"], str)
    assert isinstance(identity["dirty"], bool)


def test_manifest_is_written_before_ready_boundary(tmp_path: Path) -> None:
    """Write resolved Config and Manifest atomically with complete fields."""

    run_directory = tmp_path / "run"
    recorder = RunRecorder(run_directory)
    _ = recorder.write_resolved_config(_config())
    manifest_hash = recorder.write_manifest_atomic(_manifest(run_directory))

    manifest = load_unique_yaml((run_directory / "manifest.yaml").read_text(encoding="utf-8"))
    assert isinstance(manifest, dict)
    assert manifest["resolved_config"]["task_id"] == "task"
    assert manifest["run_seed"] == 7
    assert manifest["schema_hashes"]["state"] == "c" * 64
    assert len(manifest_hash) == 64
    config_payload = resolved_config_from_yaml((run_directory / "resolved_config.yaml").read_text(encoding="utf-8"))
    assert config_payload["task_id"] == "task"
    assert not (run_directory / "resolved_config.json").exists()
    assert not (run_directory / "manifest.json").exists()
    print(f"[VERIFY] VC-002: manifest_before_ready=true ready_ack_count=0 hash={manifest_hash}")


@pytest.mark.parametrize("enclosing_checkout", [False, True], ids=["outside-checkout", "inside-unrelated-checkout"])
def test_installed_framework_records_package_identity_without_using_enclosing_git(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, enclosing_checkout: bool,
) -> None:
    root = Path(__file__).resolve().parents[3] if enclosing_checkout else tmp_path
    monkeypatch.setattr(manifest_module, "_SOURCE_REPOSITORY_ROOT", root)
    monkeypatch.setattr(manifest_module, "__file__", str(root / "installed/uerl/core/config/manifest.py"))

    identity = capture_git_identity()

    assert identity == {"commit": "unavailable", "ref": "package:ue-rl-engine==1.0.0", "dirty": False}


def test_explicit_source_repository_git_error_is_preserved(tmp_path: Path) -> None:
    with pytest.raises(GitIdentityError, match="cannot capture Git identity"):
        capture_git_identity(tmp_path)
