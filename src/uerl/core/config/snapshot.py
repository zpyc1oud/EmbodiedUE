"""Versioned YAML serialization for resolved Run configuration snapshots."""

from __future__ import annotations

import math
from collections.abc import Mapping
from typing import Any, cast

import yaml

from ...errors import ConfigError
from .canonical import to_jsonable
from .models import ResolvedRunConfig
from .yaml_loader import load_unique_yaml

RESOLVED_CONFIG_SCHEMA_VERSION = 1
CHECKPOINT_CONFIG_KEY = "uerl_resolved_config_yaml"
_ENVELOPE_KEYS = frozenset({"schema_version", "resolved_config"})


def resolved_config_to_yaml(config: ResolvedRunConfig) -> str:
    """Serialize one typed configuration as safe, versioned YAML text."""

    envelope = {
        "schema_version": RESOLVED_CONFIG_SCHEMA_VERSION,
        "resolved_config": to_jsonable(config),
    }
    try:
        return cast(str, yaml.safe_dump(envelope, allow_unicode=True, default_flow_style=False, sort_keys=False))
    except yaml.YAMLError as exc:
        raise ConfigError("cannot serialize resolved Run config as YAML", code="INVALID_RUN_CONFIG") from exc


def resolved_config_from_yaml(text: str, *, path: str = "resolved_config.yaml") -> dict[str, Any]:
    """Read a versioned YAML snapshot with duplicate-key and shape checks."""

    try:
        payload = load_unique_yaml(text)
    except (yaml.YAMLError, TypeError, ValueError) as exc:
        raise ConfigError("invalid resolved Run YAML", code="INVALID_RUN_CONFIG", path=path) from exc
    if not isinstance(payload, Mapping):
        raise ConfigError("resolved Run YAML must be a mapping", code="INVALID_RUN_CONFIG", path=path)
    unknown = sorted(set(payload) - _ENVELOPE_KEYS)
    missing = sorted(_ENVELOPE_KEYS - set(payload))
    if unknown:
        raise ConfigError(
            f"resolved Run YAML has unknown fields: {', '.join(str(key) for key in unknown)}",
            code="INVALID_RUN_CONFIG",
            path=path,
        )
    if missing:
        raise ConfigError(
            f"resolved Run YAML is missing fields: {', '.join(sorted(missing))}",
            code="MISSING_RUN_CONFIG",
            path=path,
        )
    schema_version = payload["schema_version"]
    if type(schema_version) is not int:
        raise ConfigError("schema_version must be an integer", code="INVALID_RUN_CONFIG", path=path)
    if schema_version != RESOLVED_CONFIG_SCHEMA_VERSION:
        raise ConfigError(
            f"unsupported resolved Run schema version {schema_version}",
            code="UNSUPPORTED_RUN_CONFIG_SCHEMA",
            path=f"{path}.schema_version",
        )
    resolved = payload["resolved_config"]
    if not isinstance(resolved, Mapping) or any(not isinstance(key, str) for key in resolved):
        raise ConfigError("resolved_config must be a string-keyed mapping", code="INVALID_RUN_CONFIG", path=path)
    _validate_data_value(resolved, path=f"{path}.resolved_config", ancestors=set())
    return dict(resolved)


def _validate_data_value(value: object, *, path: str, ancestors: set[int]) -> None:
    """Keep YAML snapshots within JSON-like data types after safe loading."""

    if value is None or isinstance(value, (str, bool, int)):
        return
    if isinstance(value, float):
        if math.isfinite(value):
            return
        raise ConfigError("saved config values must be finite numbers", code="INVALID_RUN_CONFIG", path=path)
    if isinstance(value, Mapping):
        identity = id(value)
        if identity in ancestors:
            raise ConfigError(
                "saved config cannot contain recursive YAML aliases",
                code="INVALID_RUN_CONFIG",
                path=path,
            )
        ancestors.add(identity)
        try:
            for key, child in value.items():
                if not isinstance(key, str):
                    raise ConfigError("saved config mapping keys must be strings", code="INVALID_RUN_CONFIG", path=path)
                _validate_data_value(child, path=f"{path}.{key}", ancestors=ancestors)
        finally:
            ancestors.remove(identity)
        return
    if isinstance(value, list):
        identity = id(value)
        if identity in ancestors:
            raise ConfigError(
                "saved config cannot contain recursive YAML aliases",
                code="INVALID_RUN_CONFIG",
                path=path,
            )
        ancestors.add(identity)
        try:
            for index, child in enumerate(value):
                _validate_data_value(child, path=f"{path}[{index}]", ancestors=ancestors)
        finally:
            ancestors.remove(identity)
        return
    raise ConfigError(
        f"saved config contains unsupported YAML value type {type(value).__name__}",
        code="INVALID_RUN_CONFIG",
        path=path,
    )


def encode_worker_args(values: list[str] | tuple[str, ...]) -> str:
    """Encode UE command arguments as a YAML flow sequence."""

    if any(not isinstance(value, str) for value in values):
        raise TypeError("worker arguments must be strings")
    serialized = yaml.safe_dump(list(values), allow_unicode=True, default_flow_style=True, sort_keys=False)
    return cast(str, serialized).strip()


def decode_worker_args(text: str, *, path: str = "session.worker_args") -> list[str]:
    """Read a strict string list, including historical JSON list spellings."""

    try:
        values = load_unique_yaml(text)
    except (yaml.YAMLError, TypeError, ValueError) as exc:
        raise ConfigError("worker args must be a YAML string list", code="INVALID_RUN_CONFIG", path=path) from exc
    if not isinstance(values, list) or any(not isinstance(value, str) for value in values):
        raise ConfigError("worker args must be a list of strings", code="INVALID_RUN_CONFIG", path=path)
    return values


__all__ = [
    "CHECKPOINT_CONFIG_KEY",
    "RESOLVED_CONFIG_SCHEMA_VERSION",
    "decode_worker_args",
    "encode_worker_args",
    "resolved_config_from_yaml",
    "resolved_config_to_yaml",
]
