"""Define typed error categories shared by the Python infrastructure."""

from __future__ import annotations

from typing import Any


class UERLError(Exception):
    """Raise the base error for a UE-RL infrastructure failure."""


class ConfigError(UERLError):
    """Raise when user-provided configuration cannot be resolved.

    Attributes:
        code: Stable machine-readable boundary error code.
        path: Optional dotted configuration path that failed validation.
    """

    def __init__(self, message: str, *, code: str = "CONFIG_ERROR", path: str | None = None) -> None:
        """Create a configuration error with stable boundary metadata.

        Args:
            message: Human-readable failure explanation.
            code: Stable machine-readable error category.
            path: Optional configuration path associated with the failure.
        """

        super().__init__(message)
        self.code = code
        self.path = path


class RegistryError(UERLError):
    """Raise when task registration or lookup fails at the registry boundary.

    Attributes:
        code: Stable machine-readable registry error code.
        task_id: Optional Task identifier associated with the failure.
    """

    def __init__(self, message: str, *, code: str = "REGISTRY_ERROR", task_id: str | None = None) -> None:
        """Create a registry error with the failed Task identity."""

        super().__init__(message)
        self.code = code
        self.task_id = task_id


class ProtocolError(UERLError):
    """Raise when an external Bridge message violates the frozen protocol."""


class BridgeProtocolError(ProtocolError):
    """Raise a structured error for an invalid external Bridge frame or DTO.

    Attributes:
        code: Stable protocol failure code.
        phase: Boundary phase reporting the failure.
        details: Optional structured diagnostic values.
    """

    def __init__(
        self,
        message: str,
        *,
        code: str = "PROTOCOL_ERROR",
        phase: str = "codec",
        details: dict[str, Any] | None = None,
    ) -> None:
        """Create a protocol error with machine-readable diagnostics.

        Args:
            message: Human-readable protocol failure.
            code: Stable failure category.
            phase: Protocol phase that detected the failure.
            details: Optional diagnostic mapping retained by the exception.
        """

        super().__init__(message)
        self.code = code
        self.phase = phase
        self.details = details or {}


class SessionError(UERLError):
    """Raise when a Worker session cannot complete its lifecycle."""
