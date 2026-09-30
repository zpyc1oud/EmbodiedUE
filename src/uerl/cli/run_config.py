"""Rebuild the configuration a Run was trained with for export and playback."""

from __future__ import annotations

import json
import types
from collections.abc import Mapping
from dataclasses import dataclass, fields, is_dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Any, Union, get_args, get_origin, get_type_hints

from ..core.config import ResolvedRunConfig, RunConfigResolver
from ..errors import ConfigError
from ..tasks.registry import TaskRegistration, create_default_registry

RESOLVED_CONFIG_FILENAME = "resolved_config.json"


@dataclass(frozen=True, slots=True)
class RunConfig:
    """The configuration for one command and the training identity it reproduces."""

    config: ResolvedRunConfig
    run_hash: str
    from_run: bool


def resolve_run_config(
    task_id: str,
    run_directory: Path | None,
    overrides: Mapping[str, str],
) -> RunConfig:
    """Resolve the Run's own Worker, Task and runner settings plus this command's launch shape.

    Session and logging start from the Task registration so the command never
    reuses the training port or writes evidence into the training Run. Only the
    given ``overrides`` (launch arguments, Slot count, device, seed) differ from
    the Run. Without ``resolved_config.json`` the registered defaults are used.
    """

    registry = create_default_registry()
    path = None if run_directory is None else run_directory / RESOLVED_CONFIG_FILENAME
    if path is None or not path.is_file():
        config = RunConfigResolver(registry).resolve(task_id, overrides)
        return RunConfig(config, config.normalized_hash, from_run=False)
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ConfigError("resolved config must be a JSON object", code="INVALID_RUN_CONFIG", path=str(path))
    if payload.get("task_id") != task_id:
        raise ConfigError(
            f"Run was trained for {payload.get('task_id')!r}, not {task_id!r}",
            code="RUN_TASK_MISMATCH",
            path=str(path),
        )
    run_hash = payload.get("normalized_hash")
    task_version = payload.get("task_version")
    if not isinstance(run_hash, str) or not run_hash or not isinstance(task_version, str):
        raise ConfigError(
            "resolved config has no normalized_hash or task_version",
            code="INVALID_RUN_CONFIG",
            path=str(path),
        )
    registration = registry.resolve(task_id)
    defaults = RunConfigResolver(registry).resolve(task_id)
    worker = _typed(type(defaults.worker), defaults.worker, payload.get("worker"), "worker")
    task = _typed(type(defaults.task), defaults.task, payload.get("task"), "task")
    runner = _typed(type(defaults.runner), defaults.runner, payload.get("runner"), "runner")
    run_registration = replace(
        registration,
        task_version=task_version,
        environment_id=worker.environment_id,
        robot_id=worker.robot_id,
        worker_config_factory=lambda: worker,
        task_config_factory=lambda: task,
        runner_config_factory=lambda: runner,
    )
    config = RunConfigResolver(_SingleRegistration(run_registration)).resolve(task_id, overrides)
    return RunConfig(config, run_hash, from_run=True)


def recorded_map_path(run_directory: Path | None) -> str | None:
    """Return the World the Run trained in, when it recorded one."""

    path = None if run_directory is None else run_directory / RESOLVED_CONFIG_FILENAME
    if path is None or not path.is_file():
        return None
    session = json.loads(path.read_text(encoding="utf-8")).get("session")
    map_path = session.get("map_path") if isinstance(session, dict) else None
    return map_path if isinstance(map_path, str) and map_path else None


@dataclass(frozen=True, slots=True)
class _SingleRegistration:
    registration: TaskRegistration

    def resolve(self, task_id: str) -> TaskRegistration:
        return self.registration


def _typed(annotation: Any, template: Any, value: Any, path: str) -> Any:
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
        return _typed(members[0], template, value, path)
    if isinstance(annotation, type) and is_dataclass(annotation):
        if not isinstance(value, dict):
            raise ConfigError("expected a JSON object", code="INVALID_RUN_CONFIG", path=path)
        declared = {item.name: item for item in fields(annotation) if item.init}
        unknown = sorted(set(value) - set(declared))
        if unknown:
            raise ConfigError(
                f"resolved config has fields this code does not define: {', '.join(unknown)}",
                code="INVALID_RUN_CONFIG",
                path=path,
            )
        hints = get_type_hints(annotation)
        values: dict[str, Any] = {}
        for name in declared:
            child_template = getattr(template, name) if template is not None else None
            if name in value:
                values[name] = _typed(hints[name], child_template, value[name], f"{path}.{name}")
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
            return tuple(_typed(tuple_args[0], None, item, f"{path}[{index}]") for index, item in enumerate(value))
        if len(tuple_args) == len(value):
            return tuple(
                _typed(member, None, item, f"{path}[{index}]")
                for index, (member, item) in enumerate(zip(tuple_args, value, strict=True))
            )
    if origin in (Mapping, dict) and isinstance(value, dict):
        return value
    raise ConfigError(f"cannot read {value!r} as {annotation!r}", code="INVALID_RUN_CONFIG", path=path)


__all__ = ["RESOLVED_CONFIG_FILENAME", "RunConfig", "recorded_map_path", "resolve_run_config"]
