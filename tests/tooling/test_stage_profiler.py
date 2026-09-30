"""Verify per-stage latency accumulation and per-iteration JSONL flushing."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from uerl.core.direct.profiling import DETAIL_STAGE_NAMES, STAGE_NAMES, StageProfiler


def test_commit_step_returns_prefixed_stage_seconds(tmp_path: Path) -> None:
    profiler = StageProfiler(rollout_length=4, jsonl_path=tmp_path / "stage.jsonl")
    with profiler.stage("preprocess"):
        pass
    metrics = profiler.commit_step()
    assert set(metrics) == {f"Perf/stage_{name}" for name in STAGE_NAMES + DETAIL_STAGE_NAMES}
    assert all(value >= 0.0 for value in metrics.values())


def test_flushes_one_jsonl_record_per_rollout_window(tmp_path: Path) -> None:
    path = tmp_path / "stage.jsonl"
    profiler = StageProfiler(rollout_length=3, jsonl_path=path)
    for _ in range(6):
        profiler.commit_step()
    lines = path.read_text(encoding="utf-8").splitlines()
    assert len(lines) == 2
    first = json.loads(lines[0])
    second = json.loads(lines[1])
    assert first["iteration"] == 0
    assert second["iteration"] == 1
    assert first["steps"] == 3
    assert set(first["stage_mean_s"]) == set(STAGE_NAMES) | set(DETAIL_STAGE_NAMES)
    assert set(first["stage_total_s"]) == set(STAGE_NAMES) | set(DETAIL_STAGE_NAMES)


def test_records_exclusive_client_step_details_for_roundtrip_accounting(tmp_path: Path) -> None:
    """Persist the exclusive client phases needed to reconcile Step roundtrip."""

    path = tmp_path / "stage.jsonl"
    profiler = StageProfiler(rollout_length=1, jsonl_path=path)
    details = {name: (index + 1) / 1000.0 for index, name in enumerate(DETAIL_STAGE_NAMES)}

    profiler.record_step_details(details)
    metrics = profiler.commit_step()

    record = json.loads(path.read_text(encoding="utf-8"))
    assert {name: record["stage_total_s"][name] for name in DETAIL_STAGE_NAMES} == details
    assert all(f"Perf/stage_{name}" in metrics for name in DETAIL_STAGE_NAMES)


def test_partial_window_is_not_flushed(tmp_path: Path) -> None:
    path = tmp_path / "stage.jsonl"
    profiler = StageProfiler(rollout_length=5, jsonl_path=path)
    for _ in range(4):
        profiler.commit_step()
    assert not path.exists()


def test_rejects_non_positive_rollout_length(tmp_path: Path) -> None:
    with pytest.raises(ValueError):
        StageProfiler(rollout_length=0, jsonl_path=tmp_path / "stage.jsonl")
