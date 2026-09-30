"""Verify deterministic Manifest identity fields."""

from pathlib import Path

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


def _manifest(directory: Path) -> RunManifest:
    """Build the same contract in a different output directory."""

    config = ResolvedRunConfig(
        task_id="task",
        task_version="1.0",
        session=SessionConfig(),
        worker=WorkerConfig(environment_id="environment", robot_id="robot", run_seed=7),
        task=DirectTaskConfig(),
        runner=RslRlRunnerConfig(),
        logging=LoggingConfig(),
        normalized_hash="b" * 64,
    )
    return RunManifest(
        resolved_config=config,
        run_seed=7,
        run_directory=directory,
        effective_worker_config={"physics_dt": 1.0 / 60.0, "slot_count": 1},
        build_identity={"build_id": "ue-build"},
        selected_protocol={"major": 2, "minor": 0},
        schema_hashes={"state": "c" * 64, "action": "e" * 64},
        layout_hashes={"initial_state": "d" * 64},
        seed_derivation_version="seed-v1",
        git_identity={"commit": "a" * 40, "ref": "master", "dirty": False},
    )


def test_manifest_identity_is_independent_of_output_directory(tmp_path: Path) -> None:
    """Keep the contract hash stable when only the output directory changes."""

    first_dir = tmp_path / "first"
    second_dir = tmp_path / "second"
    first_hash = RunRecorder(first_dir).write_manifest_atomic(_manifest(first_dir))
    second_hash = RunRecorder(second_dir).write_manifest_atomic(_manifest(second_dir))

    assert first_hash == second_hash
    print(
        "[VERIFY] VC-009: config_hash=EQUAL manifest_contract=EQUAL "
        "seed_derivation=EQUAL output_directory=EXCLUDED"
    )
