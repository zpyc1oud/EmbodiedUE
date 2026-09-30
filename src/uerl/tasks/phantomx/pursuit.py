"""Pursuit command parameters and environment target field (no task subclass)."""

from __future__ import annotations

from collections.abc import Mapping
from dataclasses import dataclass
from types import MappingProxyType
from typing import Any

PHANTOMX_PURSUIT_TARGET_FIELD = "environment.pursuit_target_position"

PHANTOMX_PURSUIT_TARGET_DESCRIPTOR: Mapping[str, Any] = MappingProxyType(
    {
        "name": PHANTOMX_PURSUIT_TARGET_FIELD,
        "dtype": "float32",
        "shape": (3,),
        "unit": "m",
        "frame": "slot/local",
        "semantic": "pursuit_target_position",
        "source": "uerl.environment",
        "extensions": MappingProxyType({}),
    }
)


@dataclass(frozen=True, slots=True)
class PhantomXPursuitCommand:
    """Convert Player position into commands the trained locomotion policy understands."""

    speed_mps: float = 0.45
    stopping_distance_m: float = 1.25


DEFAULT_PHANTOMX_PURSUIT_COMMAND = PhantomXPursuitCommand()


__all__ = [
    "DEFAULT_PHANTOMX_PURSUIT_COMMAND",
    "PHANTOMX_PURSUIT_TARGET_DESCRIPTOR",
    "PHANTOMX_PURSUIT_TARGET_FIELD",
    "PhantomXPursuitCommand",
]
