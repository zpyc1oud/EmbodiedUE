"""Verify task archive enforcement behavior."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "check_task_archive.py"


def _write(root: Path, relative: str, content: str) -> None:
    path = root / ".scratch" / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    _ = path.write_text(content, encoding="utf-8")


def _run(root: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--root", str(root)],
        capture_output=True,
        text=True,
        check=False,
    )


def test_done_task_outside_archive_blocks(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "2026-08-20-feature/issues/2026-08-20-01-finished.md",
        "Status: done\nStarted: 2026-08-20\nCompleted: 2026-08-20\nClosed: -\n",
    )

    result = _run(tmp_path)

    assert result.returncode == 1
    assert "must be moved under .scratch/archive/" in result.stderr


def test_archived_task_with_dates_passes(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "archive/2026-08-19-feature/issues/2026-08-19-01-finished.md",
        "Status: done\nStarted: 2026-08-19\nCompleted: 2026-08-19\nClosed: 2026-08-20\n",
    )

    result = _run(tmp_path)

    assert result.returncode == 0
    assert result.stdout == "Task archive check passed.\n"


def test_open_task_requires_dated_filename_and_pending_dates(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "2026-08-20-feature/issues/01-open.md",
        "Status: in-progress\nStarted: 2026-08-20\nCompleted: 2026-08-20\nClosed: -\n",
    )

    result = _run(tmp_path)

    assert result.returncode == 1
    assert "filename must start with YYYY-MM-DD-" in result.stderr
    assert "open task must use Completed: -" in result.stderr


def test_effort_folder_requires_dated_name(tmp_path: Path) -> None:
    _write(
        tmp_path,
        "feature/issues/2026-08-20-01-open.md",
        "Status: in-progress\nStarted: 2026-08-20\nCompleted: -\nClosed: -\n",
    )

    result = _run(tmp_path)

    assert result.returncode == 1
    assert "effort directory must start with YYYY-MM-DD-" in result.stderr
