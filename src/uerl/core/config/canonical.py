"""Provide the single canonical JSON boundary used by Config and Manifest."""

from __future__ import annotations

import hashlib
from collections.abc import Mapping
from dataclasses import fields, is_dataclass
from enum import StrEnum
from pathlib import Path
from typing import Any

from ..codec import canonical_json as _canonical_json


def to_jsonable(value: Any) -> Any:
    """Convert a typed configuration value to its canonical JSON shape.

    Args:
        value: A dataclass, enum, path, mapping, tuple, or already JSON-like
            value from the typed Config/Manifest boundary.

    Returns:
        A JSON-compatible tree with dataclasses represented as field mappings,
        enums as values, paths as POSIX strings, mappings as string-keyed
        objects, and tuples as arrays. The returned tree is newly allocated for
        supported containers.
    """

    if is_dataclass(value):
        return {item.name: to_jsonable(getattr(value, item.name)) for item in fields(value)}
    if isinstance(value, StrEnum):
        return value.value
    if isinstance(value, Path):
        return value.as_posix()
    if isinstance(value, Mapping):
        return {str(key): to_jsonable(item) for key, item in value.items()}
    if isinstance(value, tuple):
        return [to_jsonable(item) for item in value]
    return value


def canonical_json(value: Any) -> str:
    """Serialize a value with the same JCS implementation as the Bridge wire.

    Args:
        value: A typed configuration or Manifest value accepted by
            ``to_jsonable``.

    Returns:
        Canonical UTF-8 JSON text with deterministic key and number ordering.

    Raises:
        BridgeProtocolError: If the value contains unsupported, non-finite, or
            non-interoperable JSON data.
    """

    return _canonical_json(to_jsonable(value)).decode("utf-8")


def sha256_hex(value: str) -> str:
    """Hash a canonical boundary representation once for protocol or audit use.

    Args:
        value: Already canonical JSON text. Callers must not pass a differently
            serialized representation when the hash identifies a protocol DTO.

    Returns:
        The lowercase SHA-256 hex digest of the UTF-8 representation.
    """

    return hashlib.sha256(value.encode("utf-8")).hexdigest()
