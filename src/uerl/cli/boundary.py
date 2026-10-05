"""Report CLI input-boundary failures as one actionable line.

Task IDs, override paths and Run selectors are user input resolved before a
Run starts, so a mistake there is a product message rather than a stack trace.
Failures raised after a Session is live keep their traceback.
"""

from __future__ import annotations

import argparse
import functools
from collections.abc import Callable
from difflib import get_close_matches
from typing import Literal

from ..errors import ConfigError, RegistryError

# Console-script entry points are called with no argument and tests pass argv.
Command = Callable[..., int]

_PATH_CODES = frozenset(
    {"UNKNOWN_OVERRIDE_PATH", "INVALID_OVERRIDE_PATH", "DERIVED_FIELD_OVERRIDE"}
)
_SAVED_RUN_COMMON_OVERRIDE_PATHS = frozenset(
    {
        "worker.slot_count",
        "runner.device",
        "session.mode",
        "session.worker_executable",
        "session.worker_args",
        "session.host",
        "session.port",
        "session.connect_timeout_s",
        "session.request_timeout_s",
        "session.presentation_mode",
        "session.map_path",
    }
)
_SAVED_RUN_OVERRIDE_PATHS = {
    "play": _SAVED_RUN_COMMON_OVERRIDE_PATHS | {"worker.run_seed"},
    "export": _SAVED_RUN_COMMON_OVERRIDE_PATHS,
}


def guard(command: Command) -> Command:
    """Print input-boundary failures from one CLI command instead of raising."""

    @functools.wraps(command)
    def guarded(argv: list[str] | None = None) -> int:
        try:
            return command(argv)
        except (ConfigError, RegistryError, FileNotFoundError) as exc:
            for line in describe(exc):
                print(line)
            return 1

    return guarded


def describe(exc: ConfigError | RegistryError | FileNotFoundError) -> list[str]:
    """Return the printable lines for one input-boundary failure."""

    if isinstance(exc, RegistryError):
        if exc.code != "UNKNOWN_TASK_ID" or exc.task_id is None:
            return [f"[FAIL] registry: {exc}"]
        lines = [f"[FAIL] unknown Task {exc.task_id!r}; run `uerl tasks` to list registered Tasks"]
        candidates = suggest_task_ids(exc.task_id)
        if candidates:
            lines.append(f"candidates: {', '.join(candidates)}")
        return lines
    if isinstance(exc, ConfigError):
        location = f" --{exc.path}" if exc.path else ""
        lines = [f"[FAIL] config{location}: {exc}"]
        if exc.code in _PATH_CODES:
            lines.append("run `uerl config --task <TASK> --json` to print every configurable path")
        return lines
    return [f"[FAIL] {exc}"]


def suggest_task_ids(task_id: str) -> list[str]:
    """Return the registered Task IDs closest to a rejected identifier."""

    from ..tasks.registry import create_default_registry

    known = [registration.task_id for registration in create_default_registry().list()]
    return get_close_matches(task_id, known, n=3, cutoff=0.35)


def parse_overrides(parser: argparse.ArgumentParser, tokens: list[str]) -> dict[str, str]:
    """Parse trailing ``--dotted.path value`` tokens or exit with parser usage."""

    from ..training import parse_config_options

    try:
        return parse_config_options(tokens)
    except ValueError as exc:
        parser.error(str(exc))


def validate_saved_run_overrides(
    parser: argparse.ArgumentParser,
    overrides: dict[str, str],
    *,
    operation: Literal["play", "export"],
) -> None:
    """Keep saved Run semantics fixed while allowing evaluation and host settings."""

    invalid = sorted(set(overrides) - _SAVED_RUN_OVERRIDE_PATHS[operation])
    if invalid:
        paths = ", ".join(f"--{path}" for path in invalid)
        parser.error(
            f"play/export preserve the saved Task, timing, Robot, plan and PPO configuration; "
            f"these overrides are not allowed: {paths}"
        )


__all__ = ["describe", "guard", "parse_overrides", "suggest_task_ids", "validate_saved_run_overrides"]
