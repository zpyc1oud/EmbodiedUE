"""Resolve continuation from a saved Run and explain permitted changes."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from ..core.config import ResolvedRunConfig
from ..core.config.canonical import to_jsonable
from ..errors import ConfigError
from .run_config import RESOLVED_CONFIG_FILENAME, RunConfigSource, load_run_config_source

# Changes here affect process placement, evidence or the additional training budget.
_ALLOWED_CHANGES = frozenset(
    {
        "session.mode",
        "session.worker_executable",
        "session.host",
        "session.port",
        "session.connect_timeout_s",
        "session.request_timeout_s",
        "session.presentation_mode",
        "logging.run_directory",
        "runner.device",
        "runner.max_iterations",
        "runner.checkpoint",
        "runner.parameters.run_name",
        "runner.parameters.experiment_name",
        "runner.parameters.save_interval",
    }
)


@dataclass(frozen=True, slots=True)
class ConfigChange:
    path: str
    saved: object
    effective: object
    source: str


@dataclass(frozen=True, slots=True)
class Continuation:
    """One checkpoint and its captured configuration, independent of host selection."""

    directory: Path
    checkpoint: Path
    source: RunConfigSource

    @classmethod
    def open(cls, task_id: str, checkpoint: Path) -> Continuation:
        checkpoint = checkpoint.resolve()
        directory = checkpoint.parent
        if not (directory / RESOLVED_CONFIG_FILENAME).is_file() and directory.name in ("rsl_rl", "checkpoints"):
            directory = directory.parent
        return cls(
            directory,
            checkpoint,
            load_run_config_source(
                task_id,
                directory,
                strict=True,
                checkpoint=checkpoint,
                allow_unreadable_checkpoint=True,
            ),
        )

    def resolve(
        self,
        overrides: Mapping[str, str],
        *,
        launch_overrides: Mapping[str, str] | None = None,
    ) -> ResolvedRunConfig:
        # First check explicit input separately: raw Worker arguments must not
        # bypass map/physics restrictions. The host builder owns generated args.
        explicit = self.source.resolve({**overrides, "runner.checkpoint": str(self.checkpoint)}).config
        for change in self.changes(explicit, overrides):
            if change.path not in _ALLOWED_CHANGES and change.path != "session.worker_args":
                self._reject(change)
            if change.path == "session.worker_args" and "session.worker_args" in overrides:
                self._reject(change)
        generated = launch_overrides or {}
        config = self.source.resolve({**generated, **overrides, "runner.checkpoint": str(self.checkpoint)}).config
        for change in self.changes(config, overrides, generated):
            if change.path not in _ALLOWED_CHANGES and change.path != "session.worker_args":
                self._reject(change)
        return config

    def changes(
        self,
        config: ResolvedRunConfig,
        overrides: Mapping[str, str],
        launch_overrides: Mapping[str, str] | None = None,
    ) -> tuple[ConfigChange, ...]:
        recorded = self.source.recorded
        assert recorded is not None
        before = _flatten(to_jsonable(recorded))
        after = _flatten(to_jsonable(config))
        changes = []
        for path in sorted(before.keys() | after.keys()):
            if path == "normalized_hash" or before.get(path) == after.get(path):
                continue
            source = "generated output" if path == "logging.run_directory" else "local defaults"
            if path in (launch_overrides or {}):
                source = "launch settings"
            if any(path == key or path.startswith(key + ".") for key in overrides):
                source = "explicit override"
            if path == "runner.checkpoint":
                source = "selected checkpoint"
            changes.append(ConfigChange(path, before.get(path), after.get(path), source))
        return tuple(changes)

    def validate_output(self, directory: Path) -> None:
        target = directory.resolve()
        if target == self.directory or self.directory in target.parents:
            raise ConfigError(
                "continuation must write outside the source Run",
                code="RESUME_OUTPUT_CONFLICT",
                path="logging.run_directory",
            )
        if target.exists() and (not target.is_dir() or any(target.iterdir())):
            raise ConfigError(
                "continuation output must be a new or empty directory",
                code="RESUME_OUTPUT_CONFLICT",
                path="logging.run_directory",
            )

    @staticmethod
    def _reject(change: ConfigChange) -> None:
        raise ConfigError(
            f"continuation cannot change {change.saved!r} to {change.effective!r}; "
            "start a new experiment for changed training semantics",
            code="RESUME_SEMANTIC_CHANGE",
            path=change.path,
        )


def _flatten(value: Any, prefix: str = "") -> dict[str, object]:
    if isinstance(value, dict) and value:
        return {
            path: item
            for key, child in value.items()
            for path, item in _flatten(child, f"{prefix}.{key}" if prefix else key).items()
        }
    return {prefix: value}
