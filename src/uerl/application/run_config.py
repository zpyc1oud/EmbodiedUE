"""Resolve registered defaults or saved Run settings without starting a Worker."""

from __future__ import annotations

import json
import types
from collections.abc import Mapping
from dataclasses import dataclass, fields, is_dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Any, Union, get_args, get_origin, get_type_hints

from ..core.config import ResolvedRunConfig, RunConfigResolver
from ..core.config.configspec import from_mapping
from ..core.config.training_spec import RslRlRunnerParametersCfg
from ..errors import ConfigError
from ..tasks.registry import TaskRegistration, create_default_registry

RESOLVED_CONFIG_FILENAME = "resolved_config.json"


@dataclass(frozen=True, slots=True)
class RunConfig:
    """The configuration for one command and the training identity it reproduces."""

    config: ResolvedRunConfig
    run_hash: str
    from_run: bool


@dataclass(frozen=True, slots=True)
class RunConfigSource:
    """A captured source reused while launch and output settings are assembled."""

    registration: TaskRegistration
    recorded: ResolvedRunConfig | None = None
    run_hash: str | None = None

    def resolve(self, overrides: Mapping[str, str]) -> RunConfig:
        config = RunConfigResolver(_SingleRegistration(self.registration)).resolve(
            self.registration.task_id,
            overrides,
        )
        return RunConfig(config, self.run_hash or config.normalized_hash, self.recorded is not None)


def resolve_run_config(
    task_id: str | None,
    run_directory: Path | None,
    overrides: Mapping[str, str],
    *,
    strict: bool = False,
) -> RunConfig:
    """Resolve one Task from its Run snapshot or, when allowed, its registration.

    ``strict`` requires a complete saved snapshot and an unchanged Task version.
    It is intended for operations that must reproduce a trained policy rather
    than rebuild it from current Task defaults.
    """

    return load_run_config_source(task_id, run_directory, strict=strict).resolve(overrides)


def load_run_config_source(
    task_id: str | None,
    run_directory: Path | None,
    *,
    strict: bool = False,
) -> RunConfigSource:
    """Capture configuration once; strict restore never fills semantic fields.

    Session endpoint and logging defaults are local to the new command. The
    recorded map and protocol are semantic settings, not machine settings.
    Strict callers require saved identity and every typed configuration field.
    """

    registry = create_default_registry()
    path = None if run_directory is None else run_directory / RESOLVED_CONFIG_FILENAME
    if path is None or not path.is_file():
        if strict:
            location = "resolved_config.json" if path is None else str(path)
            raise ConfigError(
                "strict Run restoration requires the complete resolved_config.json; restore it from the original "
                "Run or reconstruct it from recorded experiment evidence. Current Task defaults cannot recover a Run",
                code="MISSING_RUN_CONFIG",
                path=location,
            )
        if task_id is None:
            raise ConfigError(
                "Task identity is unavailable; provide --task or a Run with resolved_config.json",
                code="MISSING_RUN_CONFIG",
            )
        return RunConfigSource(registry.resolve(task_id))
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise ConfigError("invalid saved JSON", code="INVALID_RUN_CONFIG", path=str(path)) from exc
    if not isinstance(payload, dict):
        raise ConfigError("resolved config must be a JSON object", code="INVALID_RUN_CONFIG", path=str(path))
    recorded_task_id = payload.get("task_id")
    if not isinstance(recorded_task_id, str) or not recorded_task_id:
        if strict:
            raise ConfigError(
                "saved config has no Task identity; recover a complete resolved_config.json from the Run",
                code="MISSING_RUN_CONFIG",
                path="task_id",
            )
        if task_id is not None:
            raise ConfigError(
                f"Run was trained for {recorded_task_id!r}, not {task_id!r}",
                code="RUN_TASK_MISMATCH",
                path=str(path),
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
    return RunConfigSource(run_registration, recorded, run_hash)


def find_run_directory_for_checkpoint(checkpoint: Path) -> Path | None:
    """Find a checkpoint's nearest ancestor containing its saved Run config."""

    resolved_checkpoint = checkpoint.resolve()
    return next(
        (parent for parent in resolved_checkpoint.parents if (parent / RESOLVED_CONFIG_FILENAME).is_file()),
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
    "RESOLVED_CONFIG_FILENAME",
    "RunConfig",
    "RunConfigSource",
    "find_run_directory_for_checkpoint",
    "load_run_config_source",
    "resolve_run_config",
]
