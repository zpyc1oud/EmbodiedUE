"""Helpers for constructing versioned Run config fixtures."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

import yaml

from uerl.core.config.snapshot import RESOLVED_CONFIG_SCHEMA_VERSION


def write_resolved_config(run_directory: Path, payload: Mapping[str, object]) -> Path:
    """Write a complete-shape YAML fixture from a JSON-like config mapping."""

    run_directory.mkdir(parents=True, exist_ok=True)
    path = run_directory / "resolved_config.yaml"
    envelope: dict[str, Any] = {
        "schema_version": RESOLVED_CONFIG_SCHEMA_VERSION,
        "resolved_config": dict(payload),
    }
    path.write_text(
        yaml.safe_dump(envelope, allow_unicode=True, default_flow_style=False, sort_keys=False),
        encoding="utf-8",
    )
    return path
