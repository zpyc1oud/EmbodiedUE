"""Resolve registered defaults or saved Run settings without starting a Worker."""

from __future__ import annotations

import types
from collections.abc import Mapping
from dataclasses import dataclass, fields, is_dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Any, Union, get_args, get_origin, get_type_hints

import yaml

from ..core.config import ResolvedRunConfig, RunConfigResolver
from ..core.config.canonical import canonical_json, to_jsonable
from ..core.config.configspec import from_mapping
from ..core.config.snapshot import (
    CHECKPOINT_CONFIG_KEY,
    resolved_config_from_yaml,
)
from ..core.config.training_spec import RslRlRunnerParametersCfg
from ..core.config.yaml_loader import load_unique_yaml
from ..errors import ConfigError
from ..tasks.registry import TaskRegistration, create_default_registry

RESOLVED_CONFIG_FILENAME = "resolved_config.yaml"
MANIFEST_FILENAME = "manifest.yaml"


@dataclass(frozen=True, slots=True)
class RunConfig:
    """The configuration for one command and the training identity it reproduces."""

    config: ResolvedRunConfig
    run_hash: str
    from_run: bool
    source_path: str | None = None


@dataclass(frozen=True, slots=True)
class RunConfigSource:
    """A captured source reused while launch and output settings are assembled."""

    registration: TaskRegistration
    recorded: ResolvedRunConfig | None = None
    run_hash: str | None = None
    source_path: str | None = None

    def resolve(self, overrides: Mapping[str, str]) -> RunConfig:
        config = RunConfigResolver(_SingleRegistration(self.registration)).resolve(
            self.registration.task_id,
            overrides,
        )
        return RunConfig(config, self.run_hash or config.normalized_hash, self.recorded is not None, self.source_path)


def resolve_run_config(
    task_id: str | None,
    run_directory: Path | None,
    overrides: Mapping[str, str],
    *,
    strict: bool = False,
    checkpoint: Path | None = None,
    allow_unreadable_checkpoint: bool = False,
) -> RunConfig:
    """Resolve one Task from its Run snapshot or, when allowed, its registration.

    ``strict`` requires a complete saved snapshot and an unchanged Task version.
    It is intended for operations that must reproduce a trained policy rather
    than rebuild it from current Task defaults.
    """

    return load_run_config_source(
        task_id,
        run_directory,
        strict=strict,
        checkpoint=checkpoint,
        allow_unreadable_checkpoint=allow_unreadable_checkpoint,
    ).resolve(overrides)


def load_run_config_source(
    task_id: str | None,
    run_directory: Path | None,
    *,
    strict: bool = False,
    checkpoint: Path | None = None,
    allow_unreadable_checkpoint: bool = False,
) -> RunConfigSource:
    """Capture configuration once; strict restore never fills semantic fields.

    Session endpoint and logging defaults are local to the new command. The
    recorded map and protocol are semantic settings, not machine settings.
    Strict callers require saved identity and every typed configuration field.
    """

    registry = create_default_registry()
    checkpoint_payload = (
        _checkpoint_config_payload(checkpoint, tolerate_read_error=allow_unreadable_checkpoint)
        if checkpoint is not None
        else None
    )
    sidecar_path = _run_config_path(run_directory)
    manifest_path = _manifest_config_path(run_directory)
    if checkpoint_payload is None and sidecar_path is None and manifest_path is None:
        unsupported_json = _unsupported_json_path(run_directory)
        if unsupported_json is not None:
            raise ConfigError(
                "this JSON-config Run format is unsupported; preview a separate current-schema recovery copy with "
                "`python -m uerl.application.run_migration --source <run> --output <recovered-run>` and add "
                "`--apply` only after reviewing the plan. Incomplete settings are not inferred, and the historical "
                "JSON file was left unchanged",
                code="UNSUPPORTED_RUN_CONFIG_FORMAT",
                path=str(unsupported_json),
            )
        if strict:
            raise ConfigError(
                "strict Run restoration requires a complete resolved_config.yaml or a checkpoint with embedded "
                "Run config; restore it from the original Run or reconstruct it from recorded experiment evidence. "
                "Current Task defaults cannot recover a Run",
                code="MISSING_RUN_CONFIG",
                path=(str(checkpoint) if checkpoint is not None else str(run_directory) if run_directory else None),
            )
        if task_id is None:
            raise ConfigError(
                "Task identity is unavailable; provide --task or a Run/checkpoint with saved config",
                code="MISSING_RUN_CONFIG",
            )
        return RunConfigSource(registry.resolve(task_id), source_path="task defaults")

    embedded_source = None
    if checkpoint_payload is not None:
        embedded_source = _source_from_payload(
            checkpoint_payload,
            task_id,
            strict=strict,
            path=f"{checkpoint} ({CHECKPOINT_CONFIG_KEY})",
            registry=registry,
        )
    sidecar_sources: list[RunConfigSource] = []
    if sidecar_path is not None:
        payload = _read_sidecar(sidecar_path)
        sidecar_sources.append(_source_from_payload(
            payload,
            task_id,
            strict=strict,
            path=str(sidecar_path),
            registry=registry,
        ))
    if manifest_path is not None:
        manifest_payload = _read_manifest_config(manifest_path)
        if manifest_payload is not None:
            sidecar_sources.append(_source_from_payload(
                manifest_payload,
                task_id,
                strict=strict,
                path=str(manifest_path),
                registry=registry,
            ))
    all_sources = ([embedded_source] if embedded_source is not None else []) + sidecar_sources
    if not all_sources:
        raise ConfigError(
            "Run manifest does not contain a resolved_config; recover the original Run config",
            code="MISSING_RUN_CONFIG",
            path=str(manifest_path) if manifest_path is not None else str(run_directory),
        )
    baseline = all_sources[0]
    for candidate in all_sources[1:]:
        _require_matching_config(baseline, candidate, path=candidate.source_path)
    if embedded_source is not None:
        return embedded_source
    # Prefer the dedicated config sidecar, then the manifest recovery copy.
    return sidecar_sources[0]


def _require_matching_config(left: RunConfigSource, right: RunConfigSource, *, path: str | None) -> None:
    assert left.recorded is not None and right.recorded is not None
    if canonical_json(to_jsonable(left.recorded)) != canonical_json(to_jsonable(right.recorded)):
        raise ConfigError(
            "saved Run config sources disagree; recover matching checkpoint, resolved config, and manifest "
            "metadata before play, export, or resume",
            code="RUN_CONFIG_CONFLICT",
            path=path,
        )


def _run_config_path(run_directory: Path | None) -> Path | None:
    if run_directory is None:
        return None
    path = run_directory / RESOLVED_CONFIG_FILENAME
    return path if path.is_file() else None


def _manifest_config_path(run_directory: Path | None) -> Path | None:
    if run_directory is None:
        return None
    path = run_directory / MANIFEST_FILENAME
    return path if path.is_file() else None


def _unsupported_json_path(run_directory: Path | None) -> Path | None:
    if run_directory is None:
        return None
    for filename in ("resolved_config.json", "manifest.json"):
        path = run_directory / filename
        if path.is_file():
            return path
    return None


def _read_sidecar(path: Path) -> dict[str, object]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise ConfigError("cannot read saved Run config", code="INVALID_RUN_CONFIG", path=str(path)) from exc
    try:
        payload = resolved_config_from_yaml(text, path=str(path))
    except ConfigError as exc:
        raise ConfigError(
            f"{exc}; current Run metadata requires schema version 1. For a historical JSON or unversioned YAML Run, "
            "preview a separate copy with `python -m uerl.application.run_migration --source <run> --output "
            "<recovered-run>`; missing settings are not inferred",
            code=exc.code,
            path=exc.path or str(path),
        ) from exc
    if not isinstance(payload, dict):
        raise ConfigError("resolved config must be a mapping", code="INVALID_RUN_CONFIG", path=str(path))
    return payload


def _read_manifest_config(path: Path) -> dict[str, object] | None:
    try:
        text = path.read_text(encoding="utf-8")
        payload = load_unique_yaml(text)
    except (OSError, UnicodeDecodeError, yaml.YAMLError, TypeError, ValueError) as exc:
        raise ConfigError("cannot read saved Run manifest", code="INVALID_RUN_CONFIG", path=str(path)) from exc
    if not isinstance(payload, Mapping):
        raise ConfigError("Run manifest must be a mapping", code="INVALID_RUN_CONFIG", path=str(path))
    config = payload.get("resolved_config")
    if config is None:
        return None
    if not isinstance(config, Mapping) or any(not isinstance(key, str) for key in config):
        raise ConfigError("manifest resolved_config must be a mapping", code="INVALID_RUN_CONFIG", path=str(path))
    return dict(config)


def _checkpoint_config_payload(checkpoint: Path, *, tolerate_read_error: bool = False) -> dict[str, object] | None:
    """Read only the safe, primitive YAML string from RSL checkpoint metadata."""

    import torch

    if not checkpoint.is_file():
        return None
    try:
        payload = torch.load(checkpoint, map_location="cpu", weights_only=True)
    except Exception as exc:
        if tolerate_read_error:
            return None
        raise ConfigError(
            "cannot safely inspect checkpoint config metadata; use a valid RSL-RL checkpoint saved with "
            "weights-only-compatible metadata",
            code="INVALID_RUN_CONFIG",
            path=str(checkpoint),
        ) from exc
    if not isinstance(payload, Mapping):
        raise ConfigError("checkpoint must contain a mapping", code="INVALID_RUN_CONFIG", path=str(checkpoint))
    infos = payload.get("infos")
    if not isinstance(infos, Mapping):
        return None
    serialized = infos.get(CHECKPOINT_CONFIG_KEY)
    if serialized is None:
        return None
    if not isinstance(serialized, str):
        raise ConfigError(
            "checkpoint Run config metadata must be YAML text",
            code="INVALID_RUN_CONFIG",
            path=f"{checkpoint}:{CHECKPOINT_CONFIG_KEY}",
        )
    return resolved_config_from_yaml(serialized, path=f"{checkpoint}:{CHECKPOINT_CONFIG_KEY}")


def _source_from_payload(
    payload: dict[str, object],
    task_id: str | None,
    *,
    strict: bool,
    path: str,
    registry: Any | None = None,
) -> RunConfigSource:
    registry = registry or create_default_registry()
    recorded_task_id = payload.get("task_id")
    if not isinstance(recorded_task_id, str) or not recorded_task_id:
        if strict:
            raise ConfigError(
                "saved config has no Task identity; recover a complete resolved_config.yaml or checkpoint snapshot",
                code="MISSING_RUN_CONFIG",
                path="task_id",
            )
        if task_id is not None:
            raise ConfigError(
                f"Run was trained for {recorded_task_id!r}, not {task_id!r}",
                code="RUN_TASK_MISMATCH",
                path=path,
            )
        raise ConfigError("saved config has no Task identity", code="INVALID_RUN_CONFIG", path="task_id")
    if task_id is not None and recorded_task_id != task_id:
        raise ConfigError(
            f"Run was trained for {recorded_task_id!r}, not {task_id!r}",
            code="RUN_TASK_MISMATCH",
            path=str(path),
        )
    task_id = recorded_task_id
    run_hash = payload.get("normalized_hash")
    task_version = payload.get("task_version")
    if not isinstance(run_hash, str) or not run_hash or not isinstance(task_version, str) or not task_version:
        raise ConfigError(
            "resolved config requires a non-empty normalized_hash and task_version",
            code="MISSING_RUN_CONFIG" if strict else "INVALID_RUN_CONFIG",
            path=str(path),
        )
    registration = registry.resolve(task_id)
    if strict and task_version != registration.task_version:
        raise ConfigError(
            "saved Task version differs from the registered version", code="RUN_TASK_MISMATCH", path="task_version"
        )
    defaults = RunConfigResolver(registry).resolve(task_id)
    worker = _typed(type(defaults.worker), defaults.worker, payload.get("worker"), "worker", strict=strict)
    task = _typed(type(defaults.task), defaults.task, payload.get("task"), "task", strict=strict)
    runner = _typed(type(defaults.runner), defaults.runner, payload.get("runner"), "runner", strict=strict)
    session = payload.get("session")
    map_path = session.get("map_path") if isinstance(session, dict) else None
    if strict:
        if not isinstance(map_path, str) or not map_path:
            raise ConfigError(
                "saved Run config is missing its map; recover the complete original snapshot",
                code="MISSING_RUN_CONFIG",
                path="session.map_path",
            )
        from_mapping(RslRlRunnerParametersCfg, dict(runner.parameters), path="runner.parameters")
    session_config = registration.session_config
    if isinstance(map_path, str) and map_path:
        session_config = replace(session_config, map_path=map_path)
    recorded_session = _typed(type(defaults.session), defaults.session, session or {}, "session")
    if strict:
        assert isinstance(session, dict)
        protocol = _typed(
            type(defaults.session.protocol),
            defaults.session.protocol,
            session.get("protocol"),
            "session.protocol",
            strict=True,
        )
        session_config = replace(session_config, protocol=protocol)
    recorded_logging = _typed(type(defaults.logging), defaults.logging, payload.get("logging", {}), "logging")
    recorded = replace(
        defaults,
        task_version=task_version,
        worker=worker,
        task=task,
        runner=runner,
        session=recorded_session,
        logging=recorded_logging,
        normalized_hash=run_hash,
    )
    run_registration = replace(
        registration,
        task_version=task_version,
        session_config=session_config,
        logging_config=(
            replace(recorded_logging, run_directory=registration.logging_config.run_directory)
            if strict
            else registration.logging_config
        ),
        environment_id=worker.environment_id,
        robot_id=worker.robot_id,
        worker_config_factory=lambda: worker,
        task_config_factory=lambda: task,
        runner_config_factory=lambda: runner,
    )
    return RunConfigSource(run_registration, recorded, run_hash, path)


def find_run_directory_for_checkpoint(checkpoint: Path) -> Path | None:
    """Find a checkpoint's nearest ancestor containing its saved Run config."""

    resolved_checkpoint = checkpoint.resolve()
    return next(
        (
            parent
            for parent in resolved_checkpoint.parents
            if _run_config_path(parent) is not None
            or _manifest_config_path(parent) is not None
            or _unsupported_json_path(parent) is not None
        ),
        None,
    )


@dataclass(frozen=True, slots=True)
class _SingleRegistration:
    registration: TaskRegistration

    def resolve(self, task_id: str) -> TaskRegistration:
        return self.registration


def _typed(annotation: Any, template: Any, value: Any, path: str, *, strict: bool = False) -> Any:
    """Invert ``to_jsonable`` for one value of the given annotation.

    Fields absent from the Run fall back to ``template``; fields this code does
    not define are rejected rather than silently dropped.
    """

    origin = get_origin(annotation)
    if annotation is Any or annotation is object:
        return value
    if origin in (Union, types.UnionType):
        members = [item for item in get_args(annotation) if item is not type(None)]
        if value is None and len(members) < len(get_args(annotation)):
            return None
        if len(members) != 1:
            raise ConfigError("unsupported union in resolved config", code="INVALID_RUN_CONFIG", path=path)
        return _typed(members[0], template, value, path, strict=strict)
    if isinstance(annotation, type) and is_dataclass(annotation):
        if not isinstance(value, dict):
            if strict and value is None:
                raise ConfigError("saved config is missing this section", code="MISSING_RUN_CONFIG", path=path)
            raise ConfigError("expected a JSON object", code="INVALID_RUN_CONFIG", path=path)
        declared = {item.name: item for item in fields(annotation) if item.init}
        unknown = sorted(set(value) - set(declared))
        if unknown:
            raise ConfigError(
                f"resolved config has fields this code does not define: {', '.join(unknown)}",
                code="INVALID_RUN_CONFIG",
                path=path,
            )
        missing = sorted(set(declared) - set(value))
        if strict and missing:
            raise ConfigError(
                f"saved config is missing fields: {', '.join(missing)}", code="MISSING_RUN_CONFIG", path=path
            )
        hints = get_type_hints(annotation)
        values: dict[str, Any] = {}
        for name in declared:
            child_template = getattr(template, name) if template is not None else None
            if name in value:
                values[name] = _typed(hints[name], child_template, value[name], f"{path}.{name}", strict=strict)
            elif template is not None:
                values[name] = child_template
        return annotation(**values)
    if isinstance(annotation, type) and issubclass(annotation, Enum):
        return annotation(value)
    if annotation is Path and isinstance(value, str):
        return Path(value)
    if annotation is bool and isinstance(value, bool):
        return value
    if annotation is int and isinstance(value, int) and not isinstance(value, bool):
        return value
    if annotation is float and isinstance(value, (int, float)) and not isinstance(value, bool):
        return float(value)
    if annotation is str and isinstance(value, str):
        return value
    if origin is tuple and isinstance(value, list):
        tuple_args = get_args(annotation)
        if len(tuple_args) == 2 and tuple_args[1] is Ellipsis:
            return tuple(
                _typed(tuple_args[0], None, item, f"{path}[{index}]", strict=strict) for index, item in enumerate(value)
            )
        if len(tuple_args) == len(value):
            return tuple(
                _typed(member, None, item, f"{path}[{index}]", strict=strict)
                for index, (member, item) in enumerate(zip(tuple_args, value, strict=True))
            )
    if origin in (Mapping, dict) and isinstance(value, dict):
        if strict and path.startswith("task.parameters") and isinstance(template, Mapping):
            missing = sorted(set(template) - set(value))
            if missing:
                raise ConfigError(
                    f"saved config is missing parameters: {', '.join(missing)}", code="MISSING_RUN_CONFIG", path=path
                )
        return value
    raise ConfigError(f"cannot read {value!r} as {annotation!r}", code="INVALID_RUN_CONFIG", path=path)


__all__ = [
    "CHECKPOINT_CONFIG_KEY",
    "MANIFEST_FILENAME",
    "RESOLVED_CONFIG_FILENAME",
    "RunConfig",
    "RunConfigSource",
    "find_run_directory_for_checkpoint",
    "load_run_config_source",
    "resolve_run_config",
]
