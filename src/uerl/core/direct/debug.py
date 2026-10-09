"""Optional read-only observers for bounded, full-batch training diagnostics."""

from collections.abc import Mapping
from typing import Protocol


class TrainingDebugSink(Protocol):
    """Consume observations without changing the environment or random state."""

    def captures(self, step: int) -> bool:
        """Return whether this zero-based control step is inside the capture window."""
        ...

    def record(self, stage: str, step: int, values: Mapping[str, object]) -> None:
        """Synchronously copy and persist this event before its tensors can change."""
        ...
