"""The CLI reports invalid profiles as JSON without launching UE."""

import json
from pathlib import Path

import pytest

from uerl.cli.main import main


def test_invalid_profile_is_machine_readable(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    missing = tmp_path / "missing.toml"
    assert main(["check", "host", "--host-profile", str(missing), "--json"]) == 1
    result = json.loads(capsys.readouterr().out)
    assert result["ok"] is False
    assert result["error"]["code"] == "INVALID_HOST_PROFILE"


def test_cli_uses_profile_and_explicit_override_without_starting_processes(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    def forbid_process(*args: object, **kwargs: object) -> None:
        pytest.fail("static host checks must not start a process")

    monkeypatch.setattr("subprocess.Popen", forbid_process)
    profile = tmp_path / "host.toml"
    profile.write_text("project = 'profile.uproject'\nue_executable = 'missing.exe'", encoding="utf-8")
    monkeypatch.setenv("UERL_HOST_PROFILE", str(profile))
    override = tmp_path / "explicit.uproject"
    assert main(["check", "host", "--project", str(override), "--json"]) == 1
    report = json.loads(capsys.readouterr().out)
    assert report["selected"]["project"] == str(override)
    assert report["selected"]["ue_executable"] == str(tmp_path / "missing.exe")
    assert report["selected"]["sources"] == {"project": "explicit", "ue_executable": "profile"}
    assert "runtime readiness are unverified" in report["scope"]
    assert all(check["action"] for check in report["checks"] if not check["ok"])


def test_text_output_includes_sources_and_actions(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    profile = tmp_path / "host.toml"
    profile.write_text("project = 'missing.uproject'\nue_executable = 'missing.exe'", encoding="utf-8")
    assert main(["check", "host", "--host-profile", str(profile)]) == 1
    output = capsys.readouterr().out
    assert "source=profile" in output
    assert "action: " in output
    assert "[FAIL] UE_EXECUTABLE_MISSING" in output
