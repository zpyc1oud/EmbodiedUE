"""Define the public lifecycle states of one UERLSession."""

from enum import StrEnum


class SessionState(StrEnum):
    """Describe the orchestration state visible to Session callers."""

    NEW = "new"
    OPEN = "open"
    INITIALIZED = "initialized"
    READY = "ready"
    FAILED = "failed"
    CLOSED = "closed"
