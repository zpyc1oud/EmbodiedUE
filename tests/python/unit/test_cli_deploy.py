"""Verify deploy prepares a project and prints a resolved import command."""

from __future__ import annotations

from pathlib import Path
from types import SimpleNamespace

import pytest

from uerl.cli import deploy
from uerl.tasks.phantomx import PHANTOMX_TASK_ID


def _artifact_tree(root: Path) -> tuple[Path, Path]:
    run_directory = root / "runs" / PHANTOMX_TASK_ID / "20260923-150000-walk"
    artifact = run_directory / "exported" / f"{PHANTOMX_TASK_ID}.uerlpol2"
    artifact.parent.mkdir(parents=True)
    artifact.write_bytes(b"artifact")
    (run_directory / "command.txt").write_text("train\n", encoding="utf-8")
    project = root / "Game" / "Game.uproject"
    project.parent.mkdir()
    project.write_text("{}\n", encoding="utf-8")
    return project, artifact


def test_deploy_check_prints_the_import_command_without_installing(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("UERL_HOST_PROFILE", raising=False)
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    project, artifact = _artifact_tree(tmp_path)
    seen: dict[str, object] = {}

    def check_project(*, project_dir: Path, artifact: Path | None) -> tuple[SimpleNamespace, ...]:
        seen["project"] = project_dir
        seen["artifact"] = artifact
        return (SimpleNamespace(name="plugins", passed=True, detail="ok"),)

    monkeypatch.setattr(deploy, "check_project", check_project)
    monkeypatch.setattr(
        deploy,
        "install",
        lambda **_kwargs: (_ for _ in ()).throw(AssertionError("install should not run")),
    )
    monkeypatch.setattr(deploy, "resolve_project", lambda project_dir: (project.parent, project))

    assert deploy.main(
        ["--project", str(project), "--check", "--task", PHANTOMX_TASK_ID, "--artifact", "latest"]
    ) == 0

    output = capsys.readouterr().out
    assert Path(str(seen["artifact"])).resolve() == artifact.resolve()
    assert "[PASS] plugins: ok" in output
    assert "UERLPolicyImport" in output
    assert "-robotmesh=/Game/Robots/PhantomX/SK_PhantomX" in output
    assert "-replaceexisting" not in output


def test_deploy_import_stops_when_the_editor_is_missing(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    monkeypatch.chdir(tmp_path)
    project, _artifact = _artifact_tree(tmp_path)
    monkeypatch.setattr(deploy, "check_project", lambda **_kwargs: ())
    monkeypatch.setattr(deploy, "install", lambda **_kwargs: ["copied plugin"])
    monkeypatch.setattr(deploy, "resolve_project", lambda project_dir: (project.parent, project))

    assert deploy.main(
        [
            "--project",
            str(project),
            "--task",
            PHANTOMX_TASK_ID,
            "--artifact",
            "latest",
            "--import",
            "--ue-executable",
            str(tmp_path / "missing-editor.exe"),
        ]
    ) == 1

    assert "Unreal Editor not found" in capsys.readouterr().out


def test_deploy_uses_host_profile_and_allows_explicit_override(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    monkeypatch.chdir(tmp_path)
    monkeypatch.delenv("UERL_HOST_PROFILE", raising=False)
    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    project, _artifact = _artifact_tree(tmp_path)
    profile = tmp_path / "host.toml"
    profile.write_text('ue_executable = "Engine/Binaries/Win64/CustomEditor-Cmd.exe"\n', encoding="utf-8")
    override_editor = tmp_path / "OverrideEditor.exe"

    monkeypatch.setattr(
        deploy,
        "check_project",
        lambda **_kwargs: (SimpleNamespace(name="ok", passed=True, detail=""),),
    )
    monkeypatch.setattr(deploy, "resolve_project", lambda project_dir: (project.parent, project))

    assert deploy.main(
        [
            "--project",
            str(project),
            "--check",
            "--task",
            PHANTOMX_TASK_ID,
            "--artifact",
            "latest",
            "--host-profile",
            str(profile),
        ]
    ) == 0
    out_profile = capsys.readouterr().out
    assert str((tmp_path / "Engine/Binaries/Win64/CustomEditor-Cmd.exe").resolve()) in out_profile

    assert deploy.main(
        [
            "--project",
            str(project),
            "--check",
            "--task",
            PHANTOMX_TASK_ID,
            "--artifact",
            "latest",
            "--host-profile",
            str(profile),
            "--ue-executable",
            str(override_editor),
        ]
    ) == 0
    out_override = capsys.readouterr().out
    assert str(override_editor.resolve()) in out_override
    assert "CustomEditor-Cmd.exe" not in out_override


def test_deploy_check_rejects_import() -> None:
    with pytest.raises(SystemExit):
        deploy.main(["--project", "Game.uproject", "--check", "--artifact", "policy.uerlpol2", "--import"])
