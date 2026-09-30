"""Policy artifact and deployment types (independent of training)."""

from .artifact import (
    ARTIFACT_FORMAT_VERSION,
    ARTIFACT_MAGIC,
    ArtifactMetadata,
    ArtifactTiming,
    PolicyArtifact,
    RobotRuntime,
    RobotRuntimeActuator,
)

__all__ = [
    "ARTIFACT_FORMAT_VERSION",
    "ARTIFACT_MAGIC",
    "ArtifactMetadata",
    "ArtifactTiming",
    "PolicyArtifact",
    "ReferencePolicyRunner",
    "RobotRuntime",
    "RobotRuntimeActuator",
]


def __getattr__(name: str) -> object:
    """Lazy-load ReferencePolicyRunner so importing artifact does not need ort."""

    if name == "ReferencePolicyRunner":
        from .reference import ReferencePolicyRunner

        return ReferencePolicyRunner
    raise AttributeError(f"module {__name__!r} has no attribute {name!r}")
