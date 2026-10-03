"""Continuation uses recorded semantics and permits only explicit operational changes."""

from __future__ import annotations

from pathlib import Path

import pytest

from uerl.application.continuation import Continuation
from uerl.core.config.canonical import canonical_json
from uerl.errors import ConfigError
from uerl.tasks.cartpole import CARTPOLE_TASK_ID
from uerl.training import build_run_config


def test_continuation_reuses_captured_source_and_reports_budget(tmp_path: Path) -> None:
    run = tmp_path / "source"
    run.mkdir()
    snapshot = run / "resolved_config.json"
    snapshot.write_text(
        canonical_json(
            build_run_config(
                CARTPOLE_TASK_ID,
                overrides={
                    "task.rew_scale_alive": "0.75",
                    "worker.decimation": "[3,3]",
                    "runner.max_iterations": "10",
                },
            )
        )
    )
    continuation = Continuation.open(CARTPOLE_TASK_ID, run / "model_final.pt")
    snapshot.unlink()  # Neither the second build nor defaults may replace the captured semantics.
    overrides = {"runner.max_iterations": "7", "runner.device": "cpu"}
    first = continuation.resolve(overrides)
    final = continuation.resolve(overrides, launch_overrides={"session.port": "45001"})

    assert final.task == first.task
    assert final.worker.decimation == (3, 3)
    assert final.runner.max_iterations == 7
    (budget,) = [item for item in continuation.changes(final, overrides) if item.path == "runner.max_iterations"]
    assert (budget.saved, budget.effective, budget.source) == (10, 7, "explicit override")
    with pytest.raises(ConfigError, match="cannot change"):
        continuation.resolve({"task.rew_scale_alive": "9"})


@pytest.mark.parametrize(
    "path,value",
    [
        ("worker.slot_count", "1"),
        ("worker.run_seed", "9"),
        ("worker.physics_dt", "0.02"),
        ("worker.decimation", "[3,3]"),
        ("task.rew_scale_alive", "3"),
        ("session.map_path", "/Game/Maps/Changed"),
        ("runner.parameters.learning_rate", "0.5"),
        ("runner.rollout_length", "123"),
        ("session.worker_args", '["-unsafe-physics-change"]'),
    ],
)
def test_continuation_rejects_semantic_overrides(tmp_path: Path, path: str, value: str) -> None:
    config = build_run_config(CARTPOLE_TASK_ID)
    (tmp_path / "resolved_config.json").write_text(canonical_json(config))
    source = Continuation.open(CARTPOLE_TASK_ID, tmp_path / "model_final.pt")

    with pytest.raises(ConfigError) as error:
        source.resolve({path: value})

    assert error.value.code == "RESUME_SEMANTIC_CHANGE"
    assert error.value.path == path


@pytest.mark.parametrize("missing", ["snapshot", "task.rew_scale_alive", "runner.parameters.gamma", "session.map_path"])
def test_continuation_rejects_incomplete_sources(tmp_path: Path, missing: str) -> None:
    import json

    from uerl.core.config.canonical import to_jsonable

    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    if missing != "snapshot":
        owner = payload
        *parts, leaf = missing.split(".")
        for part in parts:
            owner = owner[part]
        del owner[leaf]
        (tmp_path / "resolved_config.json").write_text(json.dumps(payload))

    with pytest.raises(ConfigError):
        Continuation.open(CARTPOLE_TASK_ID, tmp_path / "model_final.pt")


@pytest.mark.parametrize("layout", ["model_final.pt", "rsl_rl/model_8.pt", "checkpoints/model_8.pt"])
def test_continuation_recognizes_existing_checkpoint_layouts(tmp_path: Path, layout: str) -> None:
    (tmp_path / "resolved_config.json").write_text(canonical_json(build_run_config(CARTPOLE_TASK_ID)))
    source = Continuation.open(CARTPOLE_TASK_ID, tmp_path / layout)
    assert source.directory == tmp_path
    with pytest.raises(ConfigError, match="outside the source Run"):
        source.validate_output(tmp_path)
    with pytest.raises(ConfigError, match="outside the source Run"):
        source.validate_output(tmp_path / "nested")


def test_continuation_rejects_version_change_and_existing_output(tmp_path: Path) -> None:
    import json

    from uerl.core.config.canonical import to_jsonable

    payload = to_jsonable(build_run_config(CARTPOLE_TASK_ID))
    (tmp_path / "resolved_config.json").write_text(json.dumps(payload))
    source = Continuation.open(CARTPOLE_TASK_ID, tmp_path / "model_final.pt")
    with pytest.raises(ConfigError, match="new or empty"):
        source.validate_output(tmp_path.parent)
    payload["task_version"] = "different"
    (tmp_path / "resolved_config.json").write_text(json.dumps(payload))
    with pytest.raises(ConfigError, match="version differs"):
        Continuation.open(CARTPOLE_TASK_ID, tmp_path / "model_final.pt")
