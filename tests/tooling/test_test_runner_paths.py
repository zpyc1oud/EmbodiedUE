"""The test runners use product host profiles and explicit path precedence."""
from __future__ import annotations

import argparse
import runpy
from pathlib import Path

import pytest

from scripts.test_runner_support import TestRun, add_runner_flags
from tests.e2e.support.host import host_environment, resolve_test_host, worker_log_path
from uerl.errors import ConfigError


@pytest.fixture(autouse=True)
def clean_host_environment(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    for variable in ("UE_ROOT", "UE_58_ROOT", "UERL_HOST_PROFILE", "UERL_TEST_UE_EXECUTABLE", "UERL_TEST_PROJECT"):
        monkeypatch.delenv(variable, raising=False)
    monkeypatch.setattr(Path, "home", lambda: tmp_path)


def profile_file(tmp_path: Path) -> Path:
    profile = tmp_path / "host.toml"
    profile.write_text('ue_executable = "Custom Engine/editor.exe"\nproject = "Custom Project/host.uproject"\n')
    return profile


@pytest.mark.parametrize("variable", ["UE_ROOT", "UE_58_ROOT"])
def test_worker_runner_uses_custom_engine_root(variable: str, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv(variable, str(tmp_path / "Custom Engine"))
    script = Path(__file__).resolve().parents[1] / "e2e/support/worker_runner.py"
    settings = runpy.run_path(str(script))
    assert Path(settings["UE_CMD"]) == tmp_path / "Custom Engine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
    assert Path(settings["UPROJECT"]).is_file()


def test_profile_beats_root_fallback_and_resolves_relative_paths(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setenv("UERL_HOST_PROFILE", str(profile_file(tmp_path)))
    monkeypatch.setenv("UE_ROOT", str(tmp_path / "ignored"))
    host = resolve_test_host()
    assert host.ue_executable == tmp_path / "Custom Engine/editor.exe"
    assert host.project == tmp_path / "Custom Project/host.uproject"
    assert host.sources == {"ue_executable": "profile", "project": "profile"}


def test_explicit_paths_beat_profile_and_pass_to_child(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    host = resolve_test_host(
        profile_path=profile_file(tmp_path),
        ue_executable=tmp_path / "selected.exe", project=tmp_path / "selected.uproject",
    )
    for key, value in host_environment(host, tmp_path / "reports").items():
        monkeypatch.setenv(key, value)
    child = resolve_test_host()
    assert child.ue_executable == tmp_path / "selected.exe"
    assert child.project == tmp_path / "selected.uproject"
    assert child.profile_path == tmp_path / "host.toml"


def test_absent_default_profile_stays_optional_in_child(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    host = resolve_test_host()
    environment = host_environment(host, tmp_path)
    assert "UERL_HOST_PROFILE" not in environment
    for key, value in environment.items():
        monkeypatch.setenv(key, value)
    assert resolve_test_host().project == host.project.resolve()


def test_explicit_missing_profile_is_not_ignored(tmp_path: Path) -> None:
    with pytest.raises(ConfigError, match="Cannot read host profile"):
        resolve_test_host(profile_path=tmp_path / "missing.toml")


def test_root_precedence(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("UE_ROOT", str(tmp_path / "first"))
    monkeypatch.setenv("UE_58_ROOT", str(tmp_path / "second"))
    assert resolve_test_host().ue_executable == tmp_path / "first/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"


def test_missing_host_is_blocked_without_running_stage(tmp_path: Path) -> None:
    parser = argparse.ArgumentParser()
    add_runner_flags(parser)
    args = parser.parse_args(["--ue-executable", str(tmp_path / "missing.exe")])
    run = TestRun(tmp_path / "reports", ["UE", "later"], 2)
    assert not run.configure_host(args, 0)
    assert [stage.status for stage in run.stages] == ["blocked", "not_run"]
    assert "missing.exe" in str(run.stages[0].detail)
    assert run.stages[0].command is None


def test_worker_logs_are_unique_in_selected_output_directory(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("UERL_TEST_OUTPUT_DIR", str(tmp_path))
    first = Path(worker_log_path("worker"))
    first.write_text("previous evidence")
    second = Path(worker_log_path("worker"))
    assert first != second
    assert first.parent == second.parent == tmp_path
    assert first.read_text() == "previous evidence"
