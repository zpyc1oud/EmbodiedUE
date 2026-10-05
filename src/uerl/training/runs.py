"""Resolve user-facing training Run directories and resume references."""

from __future__ import annotations

import re
import subprocess
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path

import yaml

from ..core.config.snapshot import resolved_config_from_yaml
from ..core.config.yaml_loader import load_unique_yaml
from ..errors import ConfigError

_UNSAFE_NAME = re.compile(r"[^A-Za-z0-9._-]+")
_ITERATION_CHECKPOINT = re.compile(r"model_(\d+)\.pt")
_DEFAULT_CHECKPOINT = "model_final.pt"
_RUN_MARKERS = ("command.txt", "resolved_config.yaml", "manifest.yaml", _DEFAULT_CHECKPOINT, "rsl_rl")
_LATEST = "latest"


def _slug(value: str, *, fallback: str) -> str:
    """Turn a task or run name into one stable filesystem segment."""

    result = _UNSAFE_NAME.sub("-", value).strip("-._")
    return result or fallback


def make_run_directory(
    task_id: str,
    *,
    root: Path = Path("runs"),
    run_name: str | None = None,
    now: datetime | None = None,
) -> Path:
    """Create and return a unique task-scoped Run directory.

    The directory is created here so two CLI callers in the same second do not
    receive the same output path.  The caller remains responsible for writing
    the resolved config and starting the actual Run.
    """

    timestamp = (now or datetime.now()).strftime("%Y%m%d-%H%M%S")
    task_segment = _slug(task_id, fallback="task")
    name_segment = _slug(run_name or "run", fallback="run")
    base = Path(root) / task_segment / f"{timestamp}-{name_segment}"
    candidate = base
    suffix = 1
    while True:
        try:
            candidate.mkdir(parents=True, exist_ok=False)
        except FileExistsError:
            candidate = Path(f"{base}-{suffix:02d}")
            suffix += 1
        else:
            return candidate


@dataclass(frozen=True, slots=True)
class RunSummary:
    """One discoverable Run and the checkpoint it can resume from."""

    directory: Path
    task_id: str
    checkpoint: Path | None


def list_runs(*, task_id: str | None = None, root: Path = Path("runs")) -> tuple[RunSummary, ...]:
    """List Runs under ``root``, newest directory name first.

    A directory is a Run when it holds command, config, manifest, or checkpoint
    evidence. Task-scoped directories (``runs/<task>/<timestamp>-<name>/``) and
    older Runs written directly under ``root`` are both included. ``task_id``
    keeps a Run whose recorded Task ID matches, or whose parent directory is
    that Task's slug.
    """

    root = Path(root)
    if not root.is_dir():
        return ()
    found: list[RunSummary] = []
    for child in _child_dirs(root):
        if _is_run(child):
            found.append(_summarize(child, fallback_task=child.name))
            continue
        for run in _child_dirs(child):
            if _is_run(run):
                found.append(_summarize(run, fallback_task=child.name))
    if task_id is not None:
        slug = _slug(task_id, fallback="task")
        found = [item for item in found if item.task_id == task_id or item.directory.parent.name == slug]
    found.sort(key=lambda item: item.directory.name, reverse=True)
    return tuple(found)


def resolve_run_directory(
    reference: str | Path,
    *,
    task_id: str | None = None,
    root: Path = Path("runs"),
) -> Path:
    """Resolve ``latest`` or an existing Run directory.

    ``latest`` is the newest Run under ``runs/<task>/``. It does not scan other
    Tasks or arbitrary ``--run-dir`` locations.
    """

    if str(reference) == _LATEST:
        return _latest_run(task_id, root=root)
    path = Path(reference)
    if path.is_dir():
        return path
    if path.is_file():
        raise FileNotFoundError(f"Run reference expects a directory or {_LATEST!r}, got a file: {path}")
    raise FileNotFoundError(f"Run directory not found: {path}")


def resolve_resume_checkpoint(
    reference: str | Path,
    *,
    task_id: str | None = None,
    root: Path = Path("runs"),
) -> Path:
    """Resolve a checkpoint file, a Run directory, or ``latest``.

    A Run prefers ``model_final.pt``. When training stopped before that file
    was written, the highest ``rsl_rl/model_<iteration>.pt`` is used instead.
    """

    path = Path(reference)
    if str(reference) != _LATEST and path.is_file():
        return path
    run_directory = resolve_run_directory(reference, task_id=task_id, root=root)
    return _checkpoint_in_run(run_directory)


def record_command(run_directory: Path, argv: list[str]) -> Path:
    """Persist the exact CLI argv used to create a Run."""

    run_directory.mkdir(parents=True, exist_ok=True)
    command_path = run_directory / "command.txt"
    command_path.write_text(subprocess.list2cmdline(argv) + "\n", encoding="utf-8")
    return command_path


def best_checkpoint(run_directory: Path) -> Path | None:
    """Return the final checkpoint, or the highest numbered intermediate one."""

    for candidate in (run_directory / _DEFAULT_CHECKPOINT, run_directory / "checkpoints" / _DEFAULT_CHECKPOINT):
        if candidate.is_file():
            return candidate
    numbered = _iteration_checkpoints(run_directory / "rsl_rl")
    numbered.extend(_iteration_checkpoints(run_directory / "checkpoints"))
    if not numbered:
        return None
    return max(numbered, key=lambda item: item[0])[1]


def _checkpoint_in_run(run_directory: Path) -> Path:
    checkpoint = best_checkpoint(run_directory)
    if checkpoint is None:
        raise FileNotFoundError(
            "resume Run has no checkpoint: "
            f"expected {run_directory / _DEFAULT_CHECKPOINT} "
            f"or {run_directory / 'rsl_rl' / 'model_<iteration>.pt'}"
        )
    return checkpoint


def _latest_run(task_id: str | None, *, root: Path) -> Path:
    if not task_id:
        raise FileNotFoundError(f"{_LATEST} requires a Task ID; pass --task")
    task_root = Path(root) / _slug(task_id, fallback="task")
    runs = [path for path in _child_dirs(task_root) if _is_run(path)]
    if not runs:
        raise FileNotFoundError(f"no Runs for {task_id} under {task_root}; run `uerl runs --task {task_id}`")
    return max(runs, key=lambda path: path.name)


def _iteration_checkpoints(directory: Path) -> list[tuple[int, Path]]:
    if not directory.is_dir():
        return []
    found: list[tuple[int, Path]] = []
    for path in directory.glob("model_*.pt"):
        match = _ITERATION_CHECKPOINT.fullmatch(path.name)
        if match and path.is_file():
            found.append((int(match.group(1)), path))
    return found


def _is_run(path: Path) -> bool:
    return path.is_dir() and any((path / name).exists() for name in _RUN_MARKERS)


def _child_dirs(directory: Path) -> list[Path]:
    if not directory.is_dir():
        return []
    return sorted(path for path in directory.iterdir() if path.is_dir())


def _summarize(directory: Path, *, fallback_task: str) -> RunSummary:
    return RunSummary(directory, _task_id_from_config(directory) or fallback_task, best_checkpoint(directory))


def _task_id_from_config(directory: Path) -> str | None:
    for name in ("resolved_config.yaml", "manifest.yaml"):
        path = directory / name
        if not path.is_file():
            continue
        try:
            text = path.read_text(encoding="utf-8")
            if name == "resolved_config.yaml":
                raw_payload: object = resolved_config_from_yaml(text, path=str(path))
            else:
                raw_payload = load_unique_yaml(text)
        except (OSError, UnicodeDecodeError, yaml.YAMLError, TypeError, ValueError, ConfigError):
            continue
        if not isinstance(raw_payload, dict):
            continue
        if name.startswith("manifest."):
            config_payload = raw_payload.get("resolved_config")
        else:
            config_payload = raw_payload
        task_id = config_payload.get("task_id") if isinstance(config_payload, dict) else None
        if isinstance(task_id, str) and task_id:
            return task_id
    return None


__all__ = [
    "RunSummary",
    "best_checkpoint",
    "list_runs",
    "make_run_directory",
    "record_command",
    "resolve_resume_checkpoint",
    "resolve_run_directory",
]
