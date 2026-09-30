"""Policy artifact container: observation/action plans + RSL-RL ONNX bytes.

The container binds *our* plans to an RSL-RL-exported ONNX model. It does not
describe network topology, activations, weights, or observation normalization —
those live inside the ONNX graph.

Stable ``ConfigError.code`` values:

+----------------------------------+----------------------------------------------+
| code                             | meaning                                      |
+==================================+==============================================+
| ``ARTIFACT_BAD_MAGIC``           | leading 8 bytes are not ``UERLPOL2``         |
| ``ARTIFACT_VERSION_MISMATCH``    | ``format_version`` missing or unsupported    |
| ``ARTIFACT_JSON_TRUNCATED``      | JSON segment shorter than ``json_length``    |
| ``ARTIFACT_ONNX_LENGTH_OOB``     | ``onnx_length`` exceeds remaining bytes      |
| ``ARTIFACT_TRAILING_DATA``       | bytes remain after the ONNX segment          |
| ``ARTIFACT_NOT_MAPPING``         | JSON root or nested object is not a mapping  |
| ``ARTIFACT_MISSING_FIELD``       | required JSON field absent                   |
| ``ARTIFACT_UNKNOWN_KEY``         | unexpected JSON key                          |
| ``ARTIFACT_TYPE_MISMATCH``       | JSON value has the wrong type                |
| ``ARTIFACT_ONNX_INVALID``        | ONNX segment is not a parseable model        |
| ``ARTIFACT_OBS_WIDTH_MISMATCH``  | policy group width != ONNX input feature dim |
| ``ARTIFACT_ACTION_WIDTH_MISMATCH``| ONNX output dim != action_plan.policy_width|
| ``ARTIFACT_METADATA_INVALID``    | metadata run_hash / git_identity invalid     |
| ``ARTIFACT_TIMING_INVALID``      | timing fields are missing or invalid         |
+----------------------------------+----------------------------------------------+

Binary layout (little-endian):

=======  ============================================
offset   content
=======  ============================================
0        magic ``UERLPOL2`` (8 ASCII bytes)
8        format_ver ``uint32``
12       json_length ``uint32``
16       json_bytes (UTF-8)
…        onnx_length ``uint64``
…        onnx_bytes
=======  ============================================
"""

from __future__ import annotations

import json
import math
import os
import struct
import tempfile
from collections.abc import Mapping
from dataclasses import dataclass
from pathlib import Path
from types import MappingProxyType
from typing import Any, Final

import onnx
from onnx import ModelProto

from uerl.core.config.canonical import canonical_json, to_jsonable
from uerl.core.mdp.plan import ActionPlan, ObservationPlan
from uerl.errors import ConfigError

ARTIFACT_MAGIC: Final[bytes] = b"UERLPOL2"
ARTIFACT_FORMAT_VERSION: Final[int] = 1

_HEADER = struct.Struct("<8sII")  # magic, format_ver, json_length
_ONNX_LEN = struct.Struct("<Q")

_JSON_KEYS: Final[frozenset[str]] = frozenset(
    {
        "task_id",
        "robot_id",
        "observation_plan",
        "action_plan",
        "robot_runtime",
        "timing",
        "metadata",
    }
)
_METADATA_KEYS: Final[frozenset[str]] = frozenset({"run_hash", "git_identity"})
_GIT_IDENTITY_KEYS: Final[frozenset[str]] = frozenset({"commit", "ref", "dirty"})
_ROBOT_RUNTIME_KEYS: Final[frozenset[str]] = frozenset({"actuators"})
_ACTUATOR_KEYS: Final[frozenset[str]] = frozenset(
    {"joint", "stiffness", "damping", "effort_limit", "default_position"}
)
_TIMING_KEYS: Final[frozenset[str]] = frozenset({"physics_dt", "decimation_min", "decimation_max"})
_DECIMATION_INT32_MAX: Final[int] = 2**31 - 1


@dataclass(frozen=True, slots=True)
class ArtifactTiming:
    """Physics timing contract carried by a policy artifact."""

    physics_dt: float
    decimation_min: int
    decimation_max: int

    def __post_init__(self) -> None:
        if isinstance(self.physics_dt, bool) or not isinstance(self.physics_dt, (int, float)):
            raise ConfigError(
                "timing.physics_dt must be finite and positive",
                code="ARTIFACT_TIMING_INVALID",
                path="timing.physics_dt",
            )
        try:
            physics_dt = float(self.physics_dt)
        except (OverflowError, TypeError, ValueError) as exc:
            raise ConfigError(
                "timing.physics_dt must be finite and positive",
                code="ARTIFACT_TIMING_INVALID",
                path="timing.physics_dt",
            ) from exc
        if not math.isfinite(physics_dt) or physics_dt <= 0.0:
            raise ConfigError(
                "timing.physics_dt must be finite and positive",
                code="ARTIFACT_TIMING_INVALID",
                path="timing.physics_dt",
            )
        for name, value in (("decimation_min", self.decimation_min), ("decimation_max", self.decimation_max)):
            if type(value) is not int or not 1 <= value <= _DECIMATION_INT32_MAX:
                raise ConfigError(
                    f"timing.{name} must be a positive int32",
                    code="ARTIFACT_TIMING_INVALID",
                    path=f"timing.{name}",
                )
        if self.decimation_min > self.decimation_max:
            raise ConfigError(
                "timing.decimation_min must not exceed decimation_max",
                code="ARTIFACT_TIMING_INVALID",
                path="timing",
            )
        if not math.isfinite(physics_dt * self.decimation_max):
            raise ConfigError(
                "timing physics interval must be finite",
                code="ARTIFACT_TIMING_INVALID",
                path="timing",
            )
        object.__setattr__(self, "physics_dt", physics_dt)

    @property
    def dt_min(self) -> float:
        """Return the shortest possible control interval in seconds."""

        return self.physics_dt * self.decimation_min

    @property
    def dt_max(self) -> float:
        """Return the longest possible control interval in seconds."""

        return self.physics_dt * self.decimation_max

    def to_json(self) -> dict[str, Any]:
        return {
            "physics_dt": self.physics_dt,
            "decimation_min": self.decimation_min,
            "decimation_max": self.decimation_max,
        }

    @classmethod
    def from_json(cls, payload: Mapping[str, Any]) -> ArtifactTiming:
        mapping = _require_mapping(payload, path="timing")
        _reject_unknown_keys(mapping, _TIMING_KEYS, path="timing")
        missing = _TIMING_KEYS.difference(mapping)
        if missing:
            raise ConfigError(
                f"timing is missing required fields: {sorted(missing)}",
                code="ARTIFACT_TIMING_INVALID",
                path="timing",
            )
        try:
            return cls(
                physics_dt=mapping["physics_dt"],
                decimation_min=mapping["decimation_min"],
                decimation_max=mapping["decimation_max"],
            )
        except ConfigError:
            raise
        except (TypeError, ValueError, OverflowError) as exc:
            raise ConfigError(
                "timing contains invalid values",
                code="ARTIFACT_TIMING_INVALID",
                path="timing",
            ) from exc


@dataclass(frozen=True, slots=True)
class RobotRuntimeActuator:
    """Actuator drive parameters for deployment (no topology name table).

    Joint names here are the reflected part names used by
    ``robot.joint.<part>.*`` field selectors — the same names the UE runtime
    selects by. Observations are *not* listed here; they come from the
    observation plan's ``state_requirements``.
    """

    joint: str
    stiffness: float
    damping: float
    effort_limit: float
    default_position: float

    def __post_init__(self) -> None:
        if not isinstance(self.joint, str) or not self.joint:
            raise ConfigError(
                "actuator joint must be a non-empty string",
                code="ARTIFACT_TYPE_MISMATCH",
                path="robot_runtime.actuators.joint",
            )
        for name, value in (
            ("stiffness", self.stiffness),
            ("damping", self.damping),
            ("effort_limit", self.effort_limit),
            ("default_position", self.default_position),
        ):
            if type(value) not in {int, float} or isinstance(value, bool):
                raise ConfigError(
                    f"actuator {name} must be a number",
                    code="ARTIFACT_TYPE_MISMATCH",
                    path=f"robot_runtime.actuators.{name}",
                )
            if not (value == value) or value in (float("inf"), float("-inf")):  # noqa: PLR0124
                raise ConfigError(
                    f"actuator {name} must be finite",
                    code="ARTIFACT_TYPE_MISMATCH",
                    path=f"robot_runtime.actuators.{name}",
                )
            object.__setattr__(self, name, float(value))

    def to_json(self) -> dict[str, Any]:
        return {
            "joint": self.joint,
            "stiffness": self.stiffness,
            "damping": self.damping,
            "effort_limit": self.effort_limit,
            "default_position": self.default_position,
        }

    @classmethod
    def from_json(cls, payload: Mapping[str, Any], *, path: str) -> RobotRuntimeActuator:
        mapping = _require_mapping(payload, path=path)
        _reject_unknown_keys(mapping, _ACTUATOR_KEYS, path=path)
        for key in _ACTUATOR_KEYS:
            if key not in mapping:
                raise ConfigError(
                    f"missing required field '{key}'",
                    code="ARTIFACT_MISSING_FIELD",
                    path=f"{path}.{key}",
                )
        return cls(
            joint=mapping["joint"],
            stiffness=mapping["stiffness"],
            damping=mapping["damping"],
            effort_limit=mapping["effort_limit"],
            default_position=mapping["default_position"],
        )


@dataclass(frozen=True, slots=True)
class RobotRuntime:
    """Deployment robot-runtime segment: actuator parameters only."""

    actuators: tuple[RobotRuntimeActuator, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "actuators", tuple(self.actuators))
        if not self.actuators:
            raise ConfigError(
                "robot_runtime.actuators must be a non-empty list",
                code="ARTIFACT_TYPE_MISMATCH",
                path="robot_runtime.actuators",
            )

    def to_json(self) -> dict[str, Any]:
        return {"actuators": [actuator.to_json() for actuator in self.actuators]}

    @classmethod
    def from_json(cls, payload: Mapping[str, Any]) -> RobotRuntime:
        mapping = _require_mapping(payload, path="robot_runtime")
        _reject_unknown_keys(mapping, _ROBOT_RUNTIME_KEYS, path="robot_runtime")
        if "actuators" not in mapping:
            raise ConfigError(
                "missing required field 'actuators'",
                code="ARTIFACT_MISSING_FIELD",
                path="robot_runtime.actuators",
            )
        raw = mapping["actuators"]
        if not isinstance(raw, list) or not raw:
            raise ConfigError(
                "robot_runtime.actuators must be a non-empty list",
                code="ARTIFACT_TYPE_MISMATCH",
                path="robot_runtime.actuators",
            )
        actuators = tuple(
            RobotRuntimeActuator.from_json(entry, path=f"robot_runtime.actuators[{index}]")
            for index, entry in enumerate(raw)
        )
        return cls(actuators=actuators)


@dataclass(frozen=True, slots=True)
class ArtifactMetadata:
    """Audit metadata carried with a policy artifact.

    Reuses the manifest identity shape: a run hash (SHA-256 hex of the
    canonical manifest identity) plus ``git_identity`` with ``commit`` /
    ``ref`` / ``dirty`` — the same fields ``RunManifest`` and
    ``capture_git_identity`` produce.
    """

    run_hash: str
    git_identity: Mapping[str, object]

    def __post_init__(self) -> None:
        if not isinstance(self.run_hash, str) or not self.run_hash:
            raise ConfigError(
                "run_hash must be a non-empty string",
                code="ARTIFACT_METADATA_INVALID",
                path="metadata.run_hash",
            )
        identity = self.git_identity
        if not isinstance(identity, Mapping):
            raise ConfigError(
                "git_identity must be a mapping",
                code="ARTIFACT_METADATA_INVALID",
                path="metadata.git_identity",
            )
        missing = _GIT_IDENTITY_KEYS.difference(identity)
        if missing:
            raise ConfigError(
                f"git identity is missing required fields: {sorted(missing)}",
                code="ARTIFACT_METADATA_INVALID",
                path="metadata.git_identity",
            )
        unknown = set(identity).difference(_GIT_IDENTITY_KEYS)
        if unknown:
            raise ConfigError(
                f"git identity has unknown fields: {sorted(unknown)}",
                code="ARTIFACT_METADATA_INVALID",
                path="metadata.git_identity",
            )
        commit = identity["commit"]
        ref = identity["ref"]
        dirty = identity["dirty"]
        if not isinstance(commit, str) or not commit:
            raise ConfigError(
                "git identity commit must be a non-empty string",
                code="ARTIFACT_METADATA_INVALID",
                path="metadata.git_identity.commit",
            )
        if not isinstance(ref, str) or not ref:
            raise ConfigError(
                "git identity ref must be a non-empty string",
                code="ARTIFACT_METADATA_INVALID",
                path="metadata.git_identity.ref",
            )
        if not isinstance(dirty, bool):
            raise ConfigError(
                "git identity dirty must be a boolean",
                code="ARTIFACT_METADATA_INVALID",
                path="metadata.git_identity.dirty",
            )
        object.__setattr__(
            self,
            "git_identity",
            MappingProxyType({"commit": commit, "ref": ref, "dirty": dirty}),
        )

    def to_json(self) -> dict[str, Any]:
        return {
            "run_hash": self.run_hash,
            "git_identity": {
                "commit": self.git_identity["commit"],
                "ref": self.git_identity["ref"],
                "dirty": self.git_identity["dirty"],
            },
        }

    @classmethod
    def from_json(cls, payload: Mapping[str, Any]) -> ArtifactMetadata:
        mapping = _require_mapping(payload, path="metadata")
        _reject_unknown_keys(mapping, _METADATA_KEYS, path="metadata")
        if "run_hash" not in mapping:
            raise ConfigError(
                "missing required field 'run_hash'",
                code="ARTIFACT_MISSING_FIELD",
                path="metadata.run_hash",
            )
        if "git_identity" not in mapping:
            raise ConfigError(
                "missing required field 'git_identity'",
                code="ARTIFACT_MISSING_FIELD",
                path="metadata.git_identity",
            )
        return cls(run_hash=mapping["run_hash"], git_identity=mapping["git_identity"])


@dataclass(frozen=True, slots=True)
class PolicyArtifact:
    """Immutable policy artifact: plans, timing, ONNX bytes, and metadata."""

    format_version: int
    task_id: str
    robot_id: str
    observation_plan: ObservationPlan
    action_plan: ActionPlan
    robot_runtime: RobotRuntime
    timing: ArtifactTiming
    onnx: bytes
    metadata: ArtifactMetadata

    def __post_init__(self) -> None:
        if type(self.format_version) is not int or isinstance(self.format_version, bool):
            raise ConfigError(
                f"format_version must be an int, got {type(self.format_version).__name__}",
                code="ARTIFACT_VERSION_MISMATCH",
                path="format_version",
            )
        if self.format_version != ARTIFACT_FORMAT_VERSION:
            raise ConfigError(
                f"unsupported format_version {self.format_version}; "
                f"expected {ARTIFACT_FORMAT_VERSION}",
                code="ARTIFACT_VERSION_MISMATCH",
                path="format_version",
            )
        if not isinstance(self.task_id, str) or not self.task_id:
            raise ConfigError(
                "task_id must be a non-empty string",
                code="ARTIFACT_TYPE_MISMATCH",
                path="task_id",
            )
        if not isinstance(self.robot_id, str) or not self.robot_id:
            raise ConfigError(
                "robot_id must be a non-empty string",
                code="ARTIFACT_TYPE_MISMATCH",
                path="robot_id",
            )
        if not isinstance(self.timing, ArtifactTiming):
            raise ConfigError(
                "timing must be an ArtifactTiming",
                code="ARTIFACT_TIMING_INVALID",
                path="timing",
            )
        if not isinstance(self.onnx, (bytes, bytearray)):
            raise ConfigError(
                "onnx must be bytes",
                code="ARTIFACT_TYPE_MISMATCH",
                path="onnx",
            )
        object.__setattr__(self, "onnx", bytes(self.onnx))

    def write(self, path: Path) -> None:
        """Atomically write the binary artifact to ``path``."""

        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        payload = self._json_payload()
        json_bytes = canonical_json(payload).encode("utf-8")
        body = bytearray()
        body.extend(
            _HEADER.pack(ARTIFACT_MAGIC, self.format_version, len(json_bytes))
        )
        body.extend(json_bytes)
        body.extend(_ONNX_LEN.pack(len(self.onnx)))
        body.extend(self.onnx)

        temporary: Path | None = None
        try:
            with tempfile.NamedTemporaryFile(
                mode="wb",
                dir=target.parent,
                prefix=f".{target.name}.",
                suffix=".tmp",
                delete=False,
            ) as stream:
                temporary = Path(stream.name)
                stream.write(body)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, target)
            temporary = None
        finally:
            if temporary is not None and temporary.exists():
                temporary.unlink()

    @classmethod
    def read(cls, path: Path) -> PolicyArtifact:
        """Load an artifact from disk; reject corrupt containers and fake ONNX."""

        data = Path(path).read_bytes()
        return cls.from_bytes(data)

    @classmethod
    def from_bytes(cls, data: bytes) -> PolicyArtifact:
        """Parse artifact bytes; do not return an instance on container failure."""

        if len(data) < _HEADER.size:
            raise ConfigError(
                "artifact header is truncated",
                code="ARTIFACT_JSON_TRUNCATED",
                path="header",
            )
        magic, format_version, json_length = _HEADER.unpack_from(data, 0)
        if magic != ARTIFACT_MAGIC:
            raise ConfigError(
                f"artifact magic {magic!r} does not match {ARTIFACT_MAGIC!r}",
                code="ARTIFACT_BAD_MAGIC",
                path="magic",
            )
        if format_version != ARTIFACT_FORMAT_VERSION:
            raise ConfigError(
                f"unsupported format_version {format_version}; "
                f"expected {ARTIFACT_FORMAT_VERSION}",
                code="ARTIFACT_VERSION_MISMATCH",
                path="format_version",
            )
        json_start = _HEADER.size
        json_end = json_start + json_length
        if json_length < 0 or json_end > len(data):
            raise ConfigError(
                "JSON segment is truncated",
                code="ARTIFACT_JSON_TRUNCATED",
                path="json",
            )
        if json_end + _ONNX_LEN.size > len(data):
            raise ConfigError(
                "onnx_length field is truncated",
                code="ARTIFACT_ONNX_LENGTH_OOB",
                path="onnx_length",
            )
        (onnx_length,) = _ONNX_LEN.unpack_from(data, json_end)
        onnx_start = json_end + _ONNX_LEN.size
        onnx_end = onnx_start + onnx_length
        if onnx_length < 0 or onnx_end > len(data):
            raise ConfigError(
                "onnx_length exceeds remaining bytes",
                code="ARTIFACT_ONNX_LENGTH_OOB",
                path="onnx",
            )
        if onnx_end != len(data):
            raise ConfigError(
                "artifact contains trailing data after ONNX segment",
                code="ARTIFACT_TRAILING_DATA",
                path="onnx",
            )

        try:
            raw_json = data[json_start:json_end].decode("utf-8")
            payload = json.loads(raw_json)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ConfigError(
                f"JSON segment is truncated or invalid: {exc}",
                code="ARTIFACT_JSON_TRUNCATED",
                path="json",
            ) from exc

        mapping = _require_mapping(payload, path="")
        _reject_unknown_keys(mapping, _JSON_KEYS, path="")
        task_id = _require_nonempty_str(mapping.get("task_id"), path="task_id")
        robot_id = _require_nonempty_str(mapping.get("robot_id"), path="robot_id")
        if "observation_plan" not in mapping:
            raise ConfigError(
                "missing required field 'observation_plan'",
                code="ARTIFACT_MISSING_FIELD",
                path="observation_plan",
            )
        if "action_plan" not in mapping:
            raise ConfigError(
                "missing required field 'action_plan'",
                code="ARTIFACT_MISSING_FIELD",
                path="action_plan",
            )
        if "robot_runtime" not in mapping:
            raise ConfigError(
                "missing required field 'robot_runtime'",
                code="ARTIFACT_MISSING_FIELD",
                path="robot_runtime",
            )
        if "timing" not in mapping:
            raise ConfigError(
                "missing required field 'timing'",
                code="ARTIFACT_MISSING_FIELD",
                path="timing",
            )
        if "metadata" not in mapping:
            raise ConfigError(
                "missing required field 'metadata'",
                code="ARTIFACT_MISSING_FIELD",
                path="metadata",
            )

        observation_plan = ObservationPlan.from_json(
            _require_mapping(mapping["observation_plan"], path="observation_plan")
        )
        action_plan = ActionPlan.from_json(
            _require_mapping(mapping["action_plan"], path="action_plan")
        )
        robot_runtime = RobotRuntime.from_json(
            _require_mapping(mapping["robot_runtime"], path="robot_runtime")
        )
        timing = ArtifactTiming.from_json(
            _require_mapping(mapping["timing"], path="timing")
        )
        metadata = ArtifactMetadata.from_json(
            _require_mapping(mapping["metadata"], path="metadata")
        )
        onnx_bytes = data[onnx_start:onnx_end]
        _parse_onnx_model(onnx_bytes)

        return cls(
            format_version=format_version,
            task_id=task_id,
            robot_id=robot_id,
            observation_plan=observation_plan,
            action_plan=action_plan,
            robot_runtime=robot_runtime,
            onnx=onnx_bytes,
            metadata=metadata,
            timing=timing,
        )

    def validate(self) -> None:
        """Check plan widths against ONNX input/output feature dimensions."""

        model = _parse_onnx_model(self.onnx)
        input_width = _feature_dim(model.graph.input, path="onnx.input")
        output_width = _feature_dim(model.graph.output, path="onnx.output")

        if "policy" not in self.observation_plan.group_widths:
            raise ConfigError(
                "observation plan is missing the 'policy' group",
                code="ARTIFACT_OBS_WIDTH_MISMATCH",
                path="observation_plan.group_widths.policy",
            )
        policy_width = self.observation_plan.group_widths["policy"]
        if policy_width != input_width:
            raise ConfigError(
                f"observation policy group width {policy_width} != ONNX input dim {input_width}",
                code="ARTIFACT_OBS_WIDTH_MISMATCH",
                path="observation_plan.group_widths.policy",
            )
        if output_width != self.action_plan.policy_width:
            raise ConfigError(
                f"ONNX output dim {output_width} != action_plan.policy_width "
                f"{self.action_plan.policy_width}",
                code="ARTIFACT_ACTION_WIDTH_MISMATCH",
                path="action_plan.policy_width",
            )

    def _json_payload(self) -> dict[str, Any]:
        return {
            "task_id": self.task_id,
            "robot_id": self.robot_id,
            "observation_plan": self.observation_plan.to_json(),
            "action_plan": self.action_plan.to_json(),
            "robot_runtime": to_jsonable(self.robot_runtime.to_json()),
            "timing": self.timing.to_json(),
            "metadata": to_jsonable(self.metadata.to_json()),
        }


def _parse_onnx_model(onnx_bytes: bytes) -> ModelProto:
    try:
        model = onnx.load_model_from_string(onnx_bytes)
        onnx.checker.check_model(model)
    except Exception as exc:  # noqa: BLE001 — surface any onnx failure as ConfigError
        raise ConfigError(
            f"ONNX segment is not a valid model: {exc}",
            code="ARTIFACT_ONNX_INVALID",
            path="onnx",
        ) from exc
    return model


def _feature_dim(value_infos: Any, *, path: str) -> int:
    if not value_infos:
        raise ConfigError(
            f"ONNX graph has no {path.split('.')[-1]}",
            code="ARTIFACT_ONNX_INVALID",
            path=path,
        )
    dims = list(value_infos[0].type.tensor_type.shape.dim)
    if not dims:
        raise ConfigError(
            f"ONNX {path} has empty shape",
            code="ARTIFACT_ONNX_INVALID",
            path=path,
        )
    # Feature dim is the last axis; leading dims may be dynamic batch.
    feature = dims[-1]
    if feature.dim_value <= 0:
        raise ConfigError(
            f"ONNX {path} feature dim is not a positive static value",
            code="ARTIFACT_ONNX_INVALID",
            path=path,
        )
    return int(feature.dim_value)


def _require_mapping(value: Any, *, path: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        label = path or "<root>"
        raise ConfigError(
            f"{label} must be a mapping",
            code="ARTIFACT_NOT_MAPPING",
            path=path or None,
        )
    return value


def _reject_unknown_keys(mapping: Mapping[str, Any], allowed: frozenset[str], *, path: str) -> None:
    for key in mapping:
        if key not in allowed:
            key_path = f"{path}.{key}" if path else str(key)
            raise ConfigError(
                f"unknown artifact key {key!r}",
                code="ARTIFACT_UNKNOWN_KEY",
                path=key_path,
            )


def _require_nonempty_str(value: Any, *, path: str) -> str:
    if value is None:
        raise ConfigError(
            f"missing required field '{path}'",
            code="ARTIFACT_MISSING_FIELD",
            path=path,
        )
    if not isinstance(value, str) or not value:
        raise ConfigError(
            f"{path} must be a non-empty string",
            code="ARTIFACT_TYPE_MISMATCH",
            path=path,
        )
    return value
