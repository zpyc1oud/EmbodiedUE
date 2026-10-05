"""Resolve packaged configuration paths with stable logical identity."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from ...errors import ConfigError


@dataclass(frozen=True, slots=True)
class ResolvedConfigPath:
    """Pair a stable config-root-relative identity with its local read path."""

    logical_path: str
    absolute_path: Path


class ConfigPathResolver:
    """Resolve only safe logical paths below one canonical config root."""

    def __init__(self, config_root: Path) -> None:
        self._root: Path = config_root.resolve()

    def resolve(self, value: str, *, path: str) -> ResolvedConfigPath:
        candidate = Path(value)
        if (
            not value
            or candidate.is_absolute()
            or candidate.anchor
            or value.startswith(("/", "\\"))
            or (len(value) > 1 and value[1] == ":")
            or ".." in candidate.parts
        ):
            raise ConfigError(
                "config path must be relative to the canonical config root",
                code="INVALID_CONFIG_PATH",
                path=path,
            )

        absolute = (self._root / candidate).resolve()
        try:
            _ = absolute.relative_to(self._root)
        except ValueError as exc:
            raise ConfigError(
                "config path escapes the canonical config root",
                code="INVALID_CONFIG_PATH",
                path=path,
            ) from exc
        return ResolvedConfigPath(candidate.as_posix(), absolute)


def repository_config_root() -> Path:
    """Return the installed package config root without depending on the caller CWD.

    The historical function name is retained for existing callers. Standard
    wheel and editable installations both expose these resources as files."""

    return Path(__file__).resolve().parents[2] / "configs"


def resolve_repository_config(value: str, *, path: str) -> ResolvedConfigPath:
    """Resolve one logical path below the package's canonical config root."""

    return ConfigPathResolver(repository_config_root()).resolve(value, path=path)
