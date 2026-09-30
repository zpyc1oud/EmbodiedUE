"""Dry-run the UERLEngine BuildPlugin command."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path
from types import ModuleType

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "package_plugin.py"


def _load() -> ModuleType:
    spec = importlib.util.spec_from_file_location("package_plugin", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_dry_run_prints_uat_command_without_running_it(tmp_path: Path) -> None:
    repo = tmp_path / "repo"
    plugin = repo / "engine" / "Plugins" / "UERLEngine"
    plugin.mkdir(parents=True)
    (plugin / "UERLEngine.uplugin").write_text("{}\n", encoding="utf-8")
    engine = tmp_path / "UE_5.8"
    uat = engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.bat"
    uat.parent.mkdir(parents=True)
    uat.write_text("@echo should-not-run\n", encoding="utf-8")
    output = tmp_path / "dist" / "UERLEngine"

    completed = subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--engine",
            str(engine),
            "--repo-root",
            str(repo),
            "--output",
            str(output),
            "--dry-run",
        ],
        check=False,
        capture_output=True,
        text=True,
    )

    assert completed.returncode == 0
    assert "should-not-run" not in completed.stdout
    assert str(uat) in completed.stdout
    assert "BuildPlugin" in completed.stdout
    assert str(plugin / "UERLEngine.uplugin") in completed.stdout
    assert str(output.resolve()) in completed.stdout
    assert "Win64" in completed.stdout

    module = _load()
    command = module.build_uat_command(engine_root=engine, repo_root=repo, output=output)
    assert command[0] == str(uat)
    assert command[1] == "BuildPlugin"
    assert command[-1] == "-TargetPlatforms=Win64"
