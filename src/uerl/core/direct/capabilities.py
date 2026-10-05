"""Describe the Task paths known to be available before a run starts."""

from __future__ import annotations

from dataclasses import dataclass
from enum import StrEnum
from typing import Literal

from uerl.errors import ConfigError

CapabilityOperation = Literal["train", "evaluate", "export"]


class CapabilityStatus(StrEnum):
    """Report only what the Direct Task contract can establish safely."""

    SUPPORTED = "supported"
    UNSUPPORTED = "unsupported"
    UNKNOWN = "unknown"


@dataclass(frozen=True, slots=True)
class TaskCapability:
    """One operation status and the reason it cannot be confirmed, if any."""

    status: CapabilityStatus
    reason: str | None = None


@dataclass(frozen=True, slots=True)
class TaskCapabilities:
    """Stable preflight result for training, evaluation, and artifact export."""

    train: TaskCapability
    evaluate: TaskCapability
    export: TaskCapability

    def for_operation(self, operation: CapabilityOperation) -> TaskCapability:
        """Return the result for a named public operation."""

        if operation == "train":
            return self.train
        if operation == "evaluate":
            return self.evaluate
        return self.export

    def require(
        self,
        operation: CapabilityOperation,
        *,
        allow_unknown: bool = False,
    ) -> None:
        """Raise a typed preflight error unless an operation is supported.

        ``allow_unknown`` is for an early check that runs before the Session has
        supplied the RobotSpec needed to assemble Manager plans. A later check
        should omit it once the Task has been bound.
        """

        capability = self.for_operation(operation)
        if capability.status is CapabilityStatus.SUPPORTED:
            return
        if allow_unknown and capability.status is CapabilityStatus.UNKNOWN:
            return
        detail = capability.reason or "the Task contract does not establish this operation"
        raise ConfigError(
            f"Task {operation} capability is {capability.status.value}: {detail}",
            code=f"TASK_CAPABILITY_{capability.status.value.upper()}",
            path=f"task.capabilities.{operation}",
        )

    def to_dict(self) -> dict[str, dict[str, str | None]]:
        """Return a JSON-ready report without implying more than each status."""

        return {
            operation: {
                "status": self.for_operation(operation).status.value,
                "reason": self.for_operation(operation).reason,
            }
            for operation in ("train", "evaluate", "export")
        }

    def format(self) -> str:
        """Render a concise preflight line with actionable unresolved reasons."""

        parts = []
        for operation in ("train", "evaluate", "export"):
            capability = self.for_operation(operation)
            part = f"{operation}={capability.status.value}"
            if capability.reason is not None:
                part += f" ({capability.reason})"
            parts.append(part)
        return " ".join(parts)


__all__ = [
    "CapabilityOperation",
    "CapabilityStatus",
    "TaskCapability",
    "TaskCapabilities",
]
