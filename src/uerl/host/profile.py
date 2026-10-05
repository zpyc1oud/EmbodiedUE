"""Resolve local machine paths independently of Task and saved-run configuration."""

from __future__ import annotations

import os
import tomllib
from dataclasses import dataclass
from pathlib import Path

from ..errors import ConfigError

DEFAULT_UE_EXECUTABLE = Path("C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe")
DEFAULT_PROJECT = Path(__file__).resolve().parents[3] / "engine/UERLHost.uproject"


@dataclass(frozen=True, slots=True)
class HostProfile:
    """Selected paths and their provenance; no filesystem validity is implied."""

    ue_executable: Path
    project: Path
    profile_path: Path
    sources: dict[str, str]


def resolve_host_profile(
    *,
    profile_path: Path | None = None,
    ue_executable: Path | None = None,
    project: Path | None = None,
) -> HostProfile:
    """Resolve explicit paths > TOML fields > defaults, without launching UE.

    Select the TOML via explicit path > UERL_HOST_PROFILE > ~/.uerl/host.toml.
    Only an absent implicit default file is optional. Relative TOML values are
    anchored to its directory; explicit values are relative to the working directory.
    """

    env_path = os.environ.get("UERL_HOST_PROFILE") or None
    selected = (
        profile_path if profile_path is not None else Path(env_path) if env_path else Path.home() / ".uerl/host.toml"
    )
    selected = selected.expanduser().resolve()
    payload: dict[str, object] = {}
    if selected.exists() or profile_path is not None or env_path is not None:
        try:
            with selected.open("rb") as stream:
                payload = tomllib.load(stream)
        except (OSError, ValueError) as exc:
            raise ConfigError(
                f"Cannot read host profile {selected}: {exc}. Fix the TOML file or select --host-profile <file>.",
                code="INVALID_HOST_PROFILE",
            ) from exc
    unknown = payload.keys() - {"ue_executable", "project"}
    if unknown:
        raise ConfigError(
            f"Unknown host profile fields in {selected}: {', '.join(sorted(unknown))}. "
            "Use only ue_executable and project.",
            code="INVALID_HOST_PROFILE",
        )
    for key, value in payload.items():
        if not isinstance(value, str) or not value.strip():
            raise ConfigError(
                f"Host profile {selected}: {key} must be a non-empty path string.",
                code="INVALID_HOST_PROFILE",
            )

    sources: dict[str, str] = {}

    def path_value(key: str, explicit: Path | None, default: Path) -> Path:
        if explicit is not None:
            sources[key] = "explicit"
            return explicit.expanduser().resolve()
        value = payload.get(key)
        if isinstance(value, str):
            sources[key] = "profile"
            path = Path(value).expanduser()
            return (selected.parent / path).resolve()
        sources[key] = "default"
        return default

    return HostProfile(
        path_value("ue_executable", ue_executable, DEFAULT_UE_EXECUTABLE),
        path_value("project", project, DEFAULT_PROJECT),
        selected,
        sources,
    )
