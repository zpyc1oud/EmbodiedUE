"""Define the immutable configuration partitions used by a UE-RL run."""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass, field
from enum import StrEnum
from pathlib import Path
from types import MappingProxyType
from typing import Any

from ...errors import ConfigError
from .robot import RobotConfig

DECIMATION_INT32_MIN = 1
DECIMATION_INT32_MAX = 2**31 - 1


def validate_decimation_int32_range(decimation: Sequence[int], *, path: str = "worker.decimation") -> None:
    """Reject decimation endpoints outside the positive int32 closed interval."""
    if any(
        type(value) is not int or not DECIMATION_INT32_MIN <= value <= DECIMATION_INT32_MAX
        for value in decimation
    ):
        raise ConfigError(
            "worker.decimation endpoints must fit int32",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )


def _empty_mapping() -> Mapping[str, object]:
    return MappingProxyType({})


def _freeze_value(value: Any) -> Any:
    if isinstance(value, Mapping):
        return MappingProxyType({str(key): _freeze_value(item) for key, item in value.items()})
    if isinstance(value, (list, tuple)):
        return tuple(_freeze_value(item) for item in value)
    return value


class LaunchMode(StrEnum):
    """Select whether the session launches or attaches to a Worker."""

    LAUNCH = "launch"
    ATTACH = "attach"


class PresentationMode(StrEnum):
    """Select the Worker presentation mode."""

    NONE = "none"
    VIEWPORT = "viewport"
    GAMEPLAY = "gameplay"


@dataclass(frozen=True, slots=True)
class ProtocolRequirements:
    """Describe the protocol range requested during Bridge negotiation.

    Attributes:
        major: Required protocol major version.
        min_minor: Lowest accepted minor version.
        max_minor: Highest accepted minor version.
    """

    major: int = 2
    min_minor: int = 0
    max_minor: int = 0


@dataclass(frozen=True, slots=True)
class SessionConfig:
    """Describe process, connection, presentation, and deadline settings.

    Attributes:
        mode: Whether Python launches the Worker or attaches to an existing host.
        worker_executable: Executable used only in launch mode.
        worker_args: Exact additional launch arguments.
        map_path: Long package name of the UE World loaded for this Run.
        host: Bridge endpoint host.
        port: Bridge endpoint port; zero is accepted by the model and validated
            at the user-config boundary.
        connect_timeout_s: Total startup/connection deadline. UE Worker spawn
            plus map load dominates this budget.
        request_timeout_s: Deadline budget for each protocol exchange. Initialize
            waits for every Slot to spawn, which 64-slot terrain alone exceeds
            in tens of seconds.
        presentation_mode: Worker presentation policy.
        protocol: Requested major/minor compatibility range.
    """

    mode: LaunchMode = LaunchMode.LAUNCH
    worker_executable: Path | None = None
    worker_args: tuple[str, ...] = ()
    map_path: str = "/Engine/Maps/Entry"
    host: str = "127.0.0.1"
    port: int = 0
    connect_timeout_s: float = 120.0
    request_timeout_s: float = 120.0
    presentation_mode: PresentationMode = PresentationMode.NONE
    protocol: ProtocolRequirements = field(default_factory=ProtocolRequirements)


@dataclass(frozen=True, slots=True)
class WorkerConfig:
    """Describe the typed Worker projection sent to UE.

    Attributes:
        slot_count: Number of stable parallel Slots.
        physics_dt: Positive simulation delta in seconds.
        decimation: Inclusive ``(min, max)`` physics-frame range per external
            action. ``(N, N)`` represents a fixed control interval.
        environment_id: Registered UE Environment identifier.
        robot_id: Registered UE Robot identifier.
        environment_config: Environment-owned scalar configuration.
        robot_config: Robot-owned provider scalar configuration. Physical Robot
            facts are not user-supplied by the new training configuration.
        robot_asset_path: Optional UE object/package path for the generic Robot provider.
        robot_config_path: Stable Robot asset declaration identity retained in manifests.
        robot_semantics: Materialised Robot semantics retained for the Python projection.
        terrain_config: Optional environment terrain levels and primitive parameters.
        terrain_config_path: Canonical `configs/`-relative terrain source identity.
        run_seed: Non-negative deterministic Run seed.

    Nested mappings and sequences are frozen during construction; boundary
    validation is performed by the Config Resolver.
    """

    slot_count: int = 1
    physics_dt: float = 1.0 / 60.0
    decimation: tuple[int, int] = (1, 1)
    environment_id: str = ""
    robot_id: str = ""
    environment_config: Mapping[str, object] = field(default_factory=_empty_mapping)
    robot_config: Mapping[str, object] = field(
        default_factory=_empty_mapping,
        metadata={"user_configurable": False},
    )
    robot_asset_path: str = field(default="", metadata={"user_configurable": False})
    robot_config_path: Path | None = field(default=None, metadata={"user_configurable": False})
    robot_semantics: RobotConfig | None = field(
        default=None,
        metadata={"user_configurable": False},
    )
    terrain_config: Mapping[str, object] = field(default_factory=_empty_mapping)
    terrain_config_path: Path | None = field(default=None, metadata={"user_configurable": False})
    run_seed: int = 0

    def __post_init__(self) -> None:
        object.__setattr__(self, "environment_config", _freeze_value(self.environment_config))
        object.__setattr__(self, "robot_config", _freeze_value(self.robot_config))
        object.__setattr__(self, "robot_semantics", _freeze_value(self.robot_semantics))
        object.__setattr__(self, "terrain_config", _freeze_value(self.terrain_config))


@dataclass(frozen=True, slots=True)
class DirectTaskConfig:
    """Describe the raw State and Action requirements owned by a task.

    Attributes:
        state_requirements: Stable raw state field names consumed by task math.
        action_schema: Stable physical command names produced by task math.
        parameters: Task-specific typed or JSON-like parameters.
        max_episode_steps: Python timeout, or zero when no timeout is requested.
        max_episode_duration_s: Simulated-time timeout, or ``None`` when unused.
        reference_dt_s: Training objective reference interval, or ``None`` when unused.
        slot_fault_reward: Finite reward assigned to Worker-faulted rows.
    """

    state_requirements: tuple[str, ...] = ()
    action_schema: tuple[str, ...] = ()
    parameters: Mapping[str, object] = field(default_factory=_empty_mapping)
    max_episode_steps: int = 0
    max_episode_duration_s: float | None = None
    reference_dt_s: float | None = None
    slot_fault_reward: float = 0.0

    def __post_init__(self) -> None:
        object.__setattr__(self, "parameters", _freeze_value(self.parameters))


@dataclass(frozen=True, slots=True)
class RslRlRunnerConfig:
    """Describe runner settings that remain in Python and never reach UE.

    Attributes:
        rollout_length: Runner rollout length.
        max_iterations: Number of rollout/update iterations for one Run. Zero
            means that a registration has not supplied an executable default.
        device: Training device identifier.
        checkpoint: Optional checkpoint path.
        parameters: Runner-specific settings retained by Python.
    """

    rollout_length: int = 0
    max_iterations: int = 0
    device: str = "cpu"
    checkpoint: Path | None = None
    parameters: Mapping[str, object] = field(default_factory=_empty_mapping)

    def __post_init__(self) -> None:
        object.__setattr__(self, "parameters", _freeze_value(self.parameters))


@dataclass(frozen=True, slots=True)
class LoggingConfig:
    """Describe the Run directory and logging policy owned by Python.

    Attributes:
        run_directory: Directory for resolved Config, Manifest, and Run evidence.
        save_metrics: Whether the training layer persists metrics.
        parameters: Logging-specific settings retained by Python.
    """

    run_directory: Path = Path("runs")
    save_metrics: bool = True
    parameters: Mapping[str, object] = field(default_factory=_empty_mapping)

    def __post_init__(self) -> None:
        object.__setattr__(self, "parameters", _freeze_value(self.parameters))


@dataclass(frozen=True, slots=True)
class ResolvedRunConfig:
    """Hold the complete immutable configuration for one resolved Run.

    The normalized hash identifies the canonical resolved values excluding the
    hash field itself. Nested mappings in every partition are frozen before the
    configuration crosses into Session orchestration.

    Attributes:
        task_id: Stable registered Task identifier.
        task_version: Resolved Task version.
        session: Process, endpoint, and protocol settings.
        worker: Typed UE projection settings.
        task: Python DirectTask settings.
        runner: Python training settings.
        logging: Python Run evidence settings.
        normalized_hash: Canonical identity of all preceding configuration.
    """

    task_id: str
    task_version: str
    session: SessionConfig
    worker: WorkerConfig
    task: DirectTaskConfig
    runner: RslRlRunnerConfig
    logging: LoggingConfig
    normalized_hash: str
