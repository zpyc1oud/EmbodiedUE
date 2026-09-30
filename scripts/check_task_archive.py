"""Enforce dated task records and archive completed records before commit."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from datetime import date
from pathlib import Path
from typing import cast

DATE_PATTERN = r"\d{4}-\d{2}-\d{2}"
EFFORT_PATTERN = re.compile(rf"^(?P<started>{DATE_PATTERN})-.+$")
FILENAME_PATTERN = re.compile(rf"^(?P<started>{DATE_PATTERN})-.+\.md$")
STATUS_PATTERN = re.compile(r"^Status\s*:\s*(?P<status>[a-z-]+)\b", re.MULTILINE)
TERMINAL_STATUSES = frozenset({"done", "resolved", "completed", "closed"})


@dataclass(frozen=True)
class TaskDocument:
    path: Path
    archived: bool
    status: str | None
    started: str | None
    completed: str | None
    closed: str | None


def _field(text: str, name: str) -> str | None:
    match = re.search(rf"^{re.escape(name)}\s*:\s*(\S+)\s*$", text, re.MULTILINE)
    return match.group(1) if match else None


def _is_date(value: str | None) -> bool:
    if value is None or not re.fullmatch(DATE_PATTERN, value):
        return False
    try:
        _ = date.fromisoformat(value)
    except ValueError:
        return False
    return True


def _read_document(path: Path, scratch_root: Path) -> TaskDocument:
    text = path.read_text(encoding="utf-8")
    relative = path.relative_to(scratch_root)
    status_match = STATUS_PATTERN.search(text)
    return TaskDocument(
        path=relative,
        archived=relative.parts[0] == "archive",
        status=status_match.group("status") if status_match else None,
        started=_field(text, "Started"),
        completed=_field(text, "Completed"),
        closed=_field(text, "Closed"),
    )


def _check_document(document: TaskDocument) -> list[str]:
    errors: list[str] = []
    display_path = f".scratch/{document.path.as_posix()}"
    effort_name = document.path.parts[1] if document.archived else document.path.parts[0]
    effort_match = EFFORT_PATTERN.fullmatch(effort_name)
    filename_match = FILENAME_PATTERN.match(document.path.name)

    if effort_match is None:
        errors.append("effort directory must start with YYYY-MM-DD-")
    elif not _is_date(effort_match.group("started")):
        errors.append(f"effort directory date is not an ISO date: {effort_match.group('started')}")
    if filename_match is None:
        errors.append("filename must start with YYYY-MM-DD-")
    if document.status is None:
        errors.append("missing Status")
    if document.started is None:
        errors.append("missing Started")
    elif not _is_date(document.started):
        errors.append(f"Started is not an ISO date: {document.started}")
    if (
        filename_match is not None
        and document.started is not None
        and filename_match.group("started") != document.started
    ):
        errors.append("filename date does not match Started")

    status_is_terminal = document.status in TERMINAL_STATUSES
    if status_is_terminal and not document.archived:
        errors.append("terminal task must be moved under .scratch/archive/")
    if document.archived and not status_is_terminal:
        errors.append("archive may contain only terminal tasks")

    if status_is_terminal:
        if document.completed is None:
            errors.append("terminal task is missing Completed")
        elif not _is_date(document.completed):
            errors.append(f"Completed is not an ISO date: {document.completed}")
        if document.closed is None:
            errors.append("terminal task is missing Closed")
        elif not _is_date(document.closed):
            errors.append(f"Closed is not an ISO date: {document.closed}")
        if (
            document.completed is not None
            and document.closed is not None
            and _is_date(document.completed)
            and _is_date(document.closed)
            and date.fromisoformat(document.closed) < date.fromisoformat(document.completed)
        ):
            errors.append("Closed must not be earlier than Completed")
    else:
        if document.completed != "-":
            errors.append("open task must use Completed: -")
        if document.closed != "-":
            errors.append("open task must use Closed: -")

    if errors:
        return [f"{display_path}: {error}" for error in errors]
    return []


def check_task_documents(scratch_root: Path) -> list[str]:
    """Return all task metadata and archive violations under ``scratch_root``."""
    if not scratch_root.is_dir():
        return [f"missing task directory: {scratch_root}"]
    errors: list[str] = []
    for path in sorted(scratch_root.rglob("*.md")):
        errors.extend(_check_document(_read_document(path, scratch_root)))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    _ = parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (defaults to the repository containing this script)",
    )
    args = parser.parse_args(argv)
    root = cast(Path, args.root)
    errors = check_task_documents(root / ".scratch")
    if errors:
        print("Task archive check failed:", file=sys.stderr)
        for error in errors:
            print(f"- {error}", file=sys.stderr)
        return 1
    print("Task archive check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
