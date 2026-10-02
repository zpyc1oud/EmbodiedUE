"""Custom engine installations work with the full and E2E test runners."""
from __future__ import annotations

import runpy
from pathlib import Path

import pytest


@pytest.mark.parametrize("variable", ["UE_ROOT", "UE_58_ROOT"])
def test_worker_runner_uses_custom_engine_root(variable: str, tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.delenv("UE_ROOT", raising=False)
    monkeypatch.delenv("UE_58_ROOT", raising=False)
    monkeypatch.setenv(variable, str(tmp_path / "Custom Engine"))
    script = Path(__file__).resolve().parents[1] / "e2e/support/worker_runner.py"
    settings = runpy.run_path(str(script))
    assert Path(settings["UE_CMD"]) == tmp_path / "Custom Engine/Engine/Binaries/Win64/UnrealEditor-Cmd.exe"
    assert Path(settings["UPROJECT"]).is_file()
