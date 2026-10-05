"""Resolve user configuration at the Python input boundary."""

from __future__ import annotations

import json
import math
import types
from collections.abc import Callable, Mapping, Sequence
from dataclasses import fields, is_dataclass, replace
from enum import StrEnum
from pathlib import Path
from types import MappingProxyType
from typing import Any, Protocol, Union, cast, get_args, get_origin, get_type_hints

from ...errors import BridgeProtocolError, ConfigError
from ..codec.frames import PROTOCOL
from .canonical import canonical_json, sha256_hex, to_jsonable
from .models import (
    DECIMATION_INT32_MAX,
    DECIMATION_INT32_MIN,
    DirectTaskConfig,
    LoggingConfig,
    ResolvedRunConfig,
    RslRlRunnerConfig,
    SessionConfig,
    WorkerConfig,
)
from .snapshot import decode_worker_args


class ConfigRegistration(Protocol):
    """Provide per-Run typed factories consumed by the Resolver.

    Every factory call creates a partition for one immutable
    ``ResolvedRunConfig``; the Resolver never mutates the registry's
    registration in place.
    """

    @property
    def task_version(self) -> str:
        """Return the registered Task version used for this Run."""
        ...

    @property
    def environment_id(self) -> str:
        """Return the authoritative UE Environment binding."""
        ...

    @property
    def robot_id(self) -> str:
        """Return the authoritative UE Robot binding."""
        ...

    @property
    def worker_config_factory(self) -> Callable[[], WorkerConfig]:
        """Return a factory for one fresh Worker Config partition."""
        ...

    @property
    def task_config_factory(self) -> Callable[[], DirectTaskConfig]:
        """Return a factory for one fresh DirectTask Config partition."""
        ...

    @property
    def runner_config_factory(self) -> Callable[[], RslRlRunnerConfig]:
        """Return a factory for one fresh Python runner Config partition."""
        ...

    @property
    def session_config(self) -> SessionConfig:
        """Return immutable Session endpoint and ownership defaults."""
        ...

    @property
    def logging_config(self) -> LoggingConfig:
        """Return immutable Run evidence and logging defaults."""
        ...


class RegistrationSource(Protocol):
    """Resolve a registered Task ID into typed configuration defaults."""

    def resolve(self, task_id: str) -> ConfigRegistration:
        """Return the registration for a user-supplied Task ID.

        Args:
            task_id: Stable, non-empty Task identifier selected by the caller.

        Returns:
            The unique typed registration for that identifier.

        Raises:
            RegistryError: If the identifier is unknown in the registration
                source.
        """


class RunConfigResolver:
    """Resolve typed defaults and user overrides into one immutable Run config."""

    def __init__(self, registry: RegistrationSource) -> None:
        """Bind the Resolver to one explicit registration source.

        Args:
            registry: Registry seam used to resolve the stable Task ID. Runtime
                module discovery is outside this class.
        """

        self._registry = registry

    def resolve(self, task_id: str, overrides: Mapping[str, str] | None = None) -> ResolvedRunConfig:
        """Resolve a registered Task and apply point-path overrides once.

        Args:
            task_id: Stable non-empty Task ID used to select typed defaults.
            overrides: Optional mapping from dotted dataclass/mapping paths to
                string boundary values. Each path is parsed and coerced exactly
                once; ``normalized_hash`` is not user-overridable.

        Returns:
            A new immutable configuration with validated values and a canonical
            ``normalized_hash`` covering every resolved field.

        Raises:
            ConfigError: If an override path is invalid, a value cannot be
                coerced, or a boundary invariant fails.
            RegistryError: If the registration source rejects the Task ID.

        Side effects:
            None on the registry or input mapping. The returned nested mappings
            are frozen for the lifetime of the Run.
        """

        if not task_id:
            raise ConfigError("Task ID must not be empty", code="EMPTY_TASK_ID", path="task_id")

        registration = self._registry.resolve(task_id)
        config = ResolvedRunConfig(
            task_id=task_id,
            task_version=registration.task_version,
            session=registration.session_config,
            worker=registration.worker_config_factory(),
            task=registration.task_config_factory(),
            runner=registration.runner_config_factory(),
            logging=registration.logging_config,
            normalized_hash="",
        )

        for path, raw_value in (overrides or {}).items():
            config = _replace_path(config, path.split("."), raw_value, path)

        _validate_registered_binding(config, registration)
        _validate_boundary_values(config)
        normalized_hash = _hash_config(config)
        return replace(config, normalized_hash=normalized_hash)


def _replace_path(current: Any, parts: list[str], raw_value: str, path: str) -> Any:
    if not parts or any(not part for part in parts):
        raise ConfigError("Override path must not be empty", code="INVALID_OVERRIDE_PATH", path=path)

    if isinstance(current, Mapping):
        key = parts[0]
        if key not in current:
            raise ConfigError("Unknown override path", code="UNKNOWN_OVERRIDE_PATH", path=path)
        values = dict(current)
        if len(parts) == 1:
            values[key] = _parse_dynamic_value(raw_value)
        else:
            values[key] = _replace_path(values[key], parts[1:], raw_value, path)
        return MappingProxyType(values)

    if not is_dataclass(current):
        raise ConfigError(
            "Override path does not address a configurable value",
            code="INVALID_OVERRIDE_PATH",
            path=path,
        )

    config_fields = {item.name: item for item in fields(current)}
    field_names = set(config_fields)
    name = parts[0]
    if name not in field_names or (type(current) is ResolvedRunConfig and name == "normalized_hash"):
        raise ConfigError("Unknown override path", code="UNKNOWN_OVERRIDE_PATH", path=path)

    child = getattr(current, name)
    if len(parts) == 1:
        if config_fields[name].metadata.get("user_configurable") is False:
            raise ConfigError(
                "configuration field is derived and cannot be overridden",
                code="DERIVED_FIELD_OVERRIDE",
                path=path,
            )
        annotation = get_type_hints(type(current))[name]
        return _replace_dataclass(current, {name: _coerce_value(annotation, raw_value, path)})


    return _replace_dataclass(current, {name: _replace_path(child, parts[1:], raw_value, path)})


def _replace_dataclass(current: Any, changes: dict[str, Any]) -> Any:
    return replace(current, **changes)


def _coerce_value(annotation: Any, raw_value: str, path: str) -> Any:
    origin = get_origin(annotation)
    args = get_args(annotation)
    if origin in (Union, types.UnionType):
        non_none = [arg for arg in args if arg is not type(None)]
        return _coerce_value(non_none[0], raw_value, path)

    if isinstance(annotation, type) and issubclass(annotation, StrEnum):
        try:
            return annotation(raw_value)
        except ValueError as exc:
            raise ConfigError("Invalid enum value", code="INVALID_ENUM", path=path) from exc

    if annotation is Path:
        return Path(raw_value)
    if annotation is bool:
        lowered = raw_value.lower()
        if lowered in {"true", "1"}:
            return True
        if lowered in {"false", "0"}:
            return False
        raise ConfigError("Expected a boolean", code="INVALID_TYPE", path=path)
    if annotation is int:
        try:
            if isinstance(raw_value, bool) or isinstance(raw_value, float):
                raise ValueError
            value = int(raw_value)
            return value
        except (TypeError, ValueError) as exc:
            raise ConfigError("Expected an integer", code="INVALID_TYPE", path=path) from exc
    if annotation is float:
        try:
            float_value = float(raw_value)
        except ValueError as exc:
            raise ConfigError("Expected a float", code="INVALID_TYPE", path=path) from exc
        if not math.isfinite(float_value):
            raise ConfigError("Expected a finite float", code="INVALID_TYPE", path=path)
        return float_value
    if annotation is str:
        return raw_value
    if origin is tuple:
        if path == "session.worker_args":
            try:
                return tuple(decode_worker_args(raw_value, path=path))
            except ConfigError as exc:
                raise ConfigError("Expected a YAML list of strings", code="INVALID_TYPE", path=path) from exc
        try:
            value = json.loads(raw_value)
        except json.JSONDecodeError as exc:
            raise ConfigError("Expected a JSON array", code="INVALID_TYPE", path=path) from exc
        if not isinstance(value, list):
            raise ConfigError("Expected a JSON array", code="INVALID_TYPE", path=path)
        args = get_args(annotation)
        if len(args) == 2 and args[1] is Ellipsis:
            expected = args[0]
            if expected is int and any(type(item) is not int for item in value):
                raise ConfigError("Expected integer array elements", code="INVALID_TYPE", path=path)
            return tuple(_coerce_value(expected, item, f"{path}[{index}]") for index, item in enumerate(value))
        if len(value) != len(args):
            raise ConfigError("Expected an array with the declared length", code="INVALID_TYPE", path=path)
        if any(expected is int and type(item) is not int for expected, item in zip(args, value, strict=True)):
            raise ConfigError("Expected integer array elements", code="INVALID_TYPE", path=path)
        return tuple(_coerce_value(args[index], item, f"{path}[{index}]") for index, item in enumerate(value))
    if origin is Mapping:
        value = _parse_dynamic_value(raw_value)
        if not isinstance(value, dict):
            raise ConfigError("Expected a JSON object", code="INVALID_TYPE", path=path)
        return MappingProxyType(value)

    raise ConfigError("Unsupported override type", code="INVALID_TYPE", path=path)


def _parse_dynamic_value(raw_value: str) -> Any:
    try:
        return json.loads(raw_value)
    except json.JSONDecodeError:
        return raw_value


def _validate_boundary_values(config: ResolvedRunConfig) -> None:
    if config.runner.max_iterations < 1:
        raise ConfigError(
            "max_iterations must be positive",
            code="INVALID_RUNNER_CONFIG",
            path="runner.max_iterations",
        )
    if config.worker.slot_count < 1:
        raise ConfigError("slot_count must be positive", code="INVALID_WORKER_CONFIG", path="worker.slot_count")
    if config.worker.physics_dt <= 0 or not math.isfinite(config.worker.physics_dt):
        raise ConfigError(
            "physics_dt must be finite and positive",
            code="INVALID_WORKER_CONFIG",
            path="worker.physics_dt",
        )
    decimation = config.worker.decimation
    if (
        not isinstance(decimation, tuple)
        or len(decimation) != 2
        or any(type(value) is not int for value in decimation)
        or any(value < DECIMATION_INT32_MIN or value > DECIMATION_INT32_MAX for value in decimation)
        or decimation[0] > decimation[1]
    ):
        raise ConfigError(
            "decimation must be an ordered pair of positive int32 values",
            code="INVALID_WORKER_CONFIG",
            path="worker.decimation",
        )
    if config.task.max_episode_steps < 0:
        raise ConfigError(
            "max_episode_steps must be non-negative",
            code="INVALID_TASK_CONFIG",
            path="task.max_episode_steps",
        )
    if not math.isfinite(config.task.slot_fault_reward):
        raise ConfigError(
            "slot_fault_reward must be finite",
            code="INVALID_TASK_CONFIG",
            path="task.slot_fault_reward",
        )
    if (
        not math.isfinite(config.session.connect_timeout_s)
        or not math.isfinite(config.session.request_timeout_s)
        or config.session.connect_timeout_s <= 0
        or config.session.request_timeout_s <= 0
    ):
        raise ConfigError("timeouts must be positive", code="INVALID_SESSION_CONFIG", path="session")
    if not 0 <= config.session.port <= 65535:
        raise ConfigError("port must be between 0 and 65535", code="INVALID_SESSION_CONFIG", path="session.port")
    if (
        not config.session.map_path.startswith("/")
        or "\\" in config.session.map_path
        or "." in config.session.map_path
    ):
        raise ConfigError(
            "map_path must be a UE long package name such as /Game/Maps/Training",
            code="INVALID_MAP_PATH",
            path="session.map_path",
        )
    if config.session.protocol.major != PROTOCOL[0]:
        raise ConfigError(
            f"protocol major must be {PROTOCOL[0]}",
            code="INVALID_SESSION_CONFIG",
            path="session.protocol.major",
        )
    if config.session.protocol.min_minor > config.session.protocol.max_minor:
        raise ConfigError(
            "protocol minor range is invalid",
            code="INVALID_SESSION_CONFIG",
            path="session.protocol",
        )
    if config.worker.run_seed < 0:
        raise ConfigError("run_seed must be non-negative", code="INVALID_WORKER_CONFIG", path="worker.run_seed")
    if config.worker.robot_asset_path and (
        not config.worker.robot_asset_path.startswith("/")
        or "\\" in config.worker.robot_asset_path
    ):
        raise ConfigError(
            "robot_asset_path must be a UE object path beginning with '/'",
            code="INVALID_ROBOT_ASSET_PATH",
            path="worker.robot_asset_path",
        )
    _validate_terrain_config(config.worker.terrain_config)
    if any(not math.isfinite(config.worker.physics_dt * value) for value in decimation):
        raise ConfigError("physics step must be finite", code="INVALID_WORKER_CONFIG", path="worker")


def _finite_number(value: object) -> float | None:
    """Return one finite JSON number as a float, excluding booleans."""

    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    try:
        if not math.isfinite(value):
            return None
        return float(value)
    except (OverflowError, ValueError):
        return None


def _finite_vector(value: object, length: int) -> tuple[float, ...] | None:
    """Return a finite numeric JSON vector with an exact length."""

    if not isinstance(value, (list, tuple)) or len(value) != length:
        return None
    result = tuple(_finite_number(item) for item in value)
    if any(item is None for item in result):
        return None
    return cast(tuple[float, ...], result)


def _ordered_range(value: object) -> tuple[float, float] | None:
    """Return an ordered finite two-number JSON range."""

    pair = _finite_vector(value, 2)
    if pair is None or pair[0] > pair[1]:
        return None
    return pair[0], pair[1]


def _validate_terrain_config(terrain_config: Mapping[str, object]) -> None:
    """Validate the Python-owned terrain object before INIT projection."""

    if not terrain_config:
        return
    required = {"num_levels", "cell_size", "border_width", "tiers"}
    optional = {"physics_collision"}
    keys = set(terrain_config)
    if not required.issubset(keys) or not keys.issubset(required | optional):
        raise ConfigError(
            "terrain_config keys must be num_levels, cell_size, border_width, tiers"
            " and optional physics_collision",
            code="INVALID_TERRAIN_CONFIG",
            path="worker.terrain_config",
        )
    if "physics_collision" in terrain_config and type(terrain_config["physics_collision"]) is not bool:
        raise ConfigError(
            "terrain physics_collision must be a boolean",
            code="INVALID_TERRAIN_CONFIG",
            path="worker.terrain_config.physics_collision",
        )
    raw_num_levels = terrain_config["num_levels"]
    cell_size = _finite_vector(terrain_config["cell_size"], 2)
    border_width = _finite_number(terrain_config["border_width"])
    raw_tiers = terrain_config["tiers"]
    if type(raw_num_levels) is not int or not 1 <= raw_num_levels <= 65535:
        raise ConfigError(
            "terrain num_levels must be an integer in [1, 65535]",
            code="INVALID_TERRAIN_CONFIG",
            path="worker.terrain_config.num_levels",
        )
    if cell_size is None or any(value <= 0 for value in cell_size):
        raise ConfigError(
            "terrain cell_size must contain two finite positive numbers",
            code="INVALID_TERRAIN_CONFIG",
            path="worker.terrain_config.cell_size",
        )
    if border_width is None or border_width < 0:
        raise ConfigError(
            "terrain border_width must be finite and non-negative",
            code="INVALID_TERRAIN_CONFIG",
            path="worker.terrain_config.border_width",
        )
    if not isinstance(raw_tiers, (list, tuple)) or len(raw_tiers) != raw_num_levels:
        raise ConfigError(
            "terrain tiers must contain exactly num_levels entries",
            code="INVALID_TERRAIN_CONFIG",
            path="worker.terrain_config.tiers",
        )
    for expected_level, raw_tier in enumerate(cast(Sequence[object], raw_tiers)):
        path = f"worker.terrain_config.tiers[{expected_level}]"
        if not isinstance(raw_tier, Mapping) or set(raw_tier) != {
            "level",
            "primitive",
            "seed",
            "platform_width",
            "params",
        }:
            raise ConfigError("terrain tier keys are invalid", code="INVALID_TERRAIN_CONFIG", path=path)
        tier = cast(Mapping[str, object], raw_tier)
        if type(tier["level"]) is not int or tier["level"] != expected_level:
            raise ConfigError(
                "terrain levels must be contiguous from zero",
                code="INVALID_TERRAIN_CONFIG",
                path=f"{path}.level",
            )
        primitive = tier["primitive"]
        if primitive not in {"plane", "heightfield", "boxes"}:
            raise ConfigError("unsupported terrain primitive", code="INVALID_TERRAIN_CONFIG", path=f"{path}.primitive")
        seed = tier["seed"]
        if (
            not isinstance(seed, str)
            or not seed
            or (len(seed) > 1 and seed.startswith("0"))
            or not seed.isascii()
            or not seed.isdecimal()
            or len(seed) > 20
            or int(seed) > 2**64 - 1
        ):
            raise ConfigError(
                "terrain seed must be a canonical unsigned 64-bit decimal string",
                code="INVALID_TERRAIN_CONFIG",
                path=f"{path}.seed",
            )
        platform_width = _finite_number(tier["platform_width"])
        if platform_width is None or platform_width < 0:
            raise ConfigError(
                "terrain platform_width must be finite and non-negative",
                code="INVALID_TERRAIN_CONFIG",
                path=f"{path}.platform_width",
            )
        raw_params = tier["params"]
        if not isinstance(raw_params, Mapping):
            raise ConfigError(
                "terrain tier params must be an object",
                code="INVALID_TERRAIN_CONFIG",
                path=f"{path}.params",
            )
        params = cast(Mapping[str, object], raw_params)
        if primitive == "plane" and params:
            raise ConfigError("plane params must be empty", code="INVALID_TERRAIN_CONFIG", path=f"{path}.params")
        if primitive == "heightfield":
            _validate_heightfield_params(params, path)
        if primitive == "boxes":
            _validate_boxes_params(params, path)


def _validate_heightfield_params(params: Mapping[str, object], path: str) -> None:
    required = {"noise_range", "noise_step", "horizontal_scale", "vertical_scale", "downsampled_scale"}
    perlin = {"perlin_scale", "perlin_octaves", "perlin_persistence", "perlin_lacunarity"}
    if not required.issubset(params) or not set(params).issubset(required | perlin):
        raise ConfigError("heightfield params keys are invalid", code="INVALID_TERRAIN_CONFIG", path=f"{path}.params")
    noise_range = _ordered_range(params["noise_range"])
    if noise_range is None:
        raise ConfigError(
            "heightfield noise_range must be an ordered finite pair",
            code="INVALID_TERRAIN_CONFIG",
            path=f"{path}.params.noise_range",
        )
    scales: dict[str, float] = {}
    for name in ("noise_step", "horizontal_scale", "vertical_scale"):
        value = _finite_number(params[name])
        if value is None or value <= 0:
            raise ConfigError(
                "heightfield scales must be finite and positive",
                code="INVALID_TERRAIN_CONFIG",
                path=f"{path}.params.{name}",
            )
        scales[name] = value
    raw_downsampled_scale = params["downsampled_scale"]
    downsampled_scale = (
        scales["horizontal_scale"]
        if raw_downsampled_scale is None
        else _finite_number(raw_downsampled_scale)
    )
    if downsampled_scale is None or downsampled_scale < scales["horizontal_scale"]:
        raise ConfigError(
            "heightfield downsampled_scale must be null or no smaller than horizontal_scale",
            code="INVALID_TERRAIN_CONFIG",
            path=f"{path}.params.downsampled_scale",
        )
    if set(params) & perlin:
        _validate_perlin_params(params, path, "heightfield")


def _validate_perlin_params(params: Mapping[str, object], path: str, primitive: str) -> None:
    """Validate the optional continuous source shared by mesh and box terrain."""

    perlin = {"perlin_scale", "perlin_octaves", "perlin_persistence", "perlin_lacunarity"}
    if set(params) & perlin and (set(params) & perlin) != perlin:
        raise ConfigError(
            f"{primitive} Perlin parameters must be supplied together",
            code="INVALID_TERRAIN_CONFIG",
            path=f"{path}.params",
        )
    if not set(params) & perlin:
        return
    perlin_scale = _finite_number(params["perlin_scale"])
    raw_octaves = params["perlin_octaves"]
    persistence = _finite_number(params["perlin_persistence"])
    lacunarity = _finite_number(params["perlin_lacunarity"])
    if perlin_scale is None or perlin_scale <= 0:
        raise ConfigError(
            f"{primitive} perlin_scale must be finite and positive",
            code="INVALID_TERRAIN_CONFIG",
            path=f"{path}.params.perlin_scale",
        )
    if type(raw_octaves) is not int or not 1 <= raw_octaves <= 8:
        raise ConfigError(
            f"{primitive} perlin_octaves must be an integer in [1, 8]",
            code="INVALID_TERRAIN_CONFIG",
            path=f"{path}.params.perlin_octaves",
        )
    if persistence is None or not 0.0 < persistence <= 1.0:
        raise ConfigError(
            f"{primitive} perlin_persistence must be in (0, 1]",
            code="INVALID_TERRAIN_CONFIG",
            path=f"{path}.params.perlin_persistence",
        )
    if lacunarity is None or lacunarity <= 1.0:
        raise ConfigError(
            f"{primitive} perlin_lacunarity must be greater than 1",
            code="INVALID_TERRAIN_CONFIG",
            path=f"{path}.params.perlin_lacunarity",
        )


def _validate_boxes_params(params: Mapping[str, object], path: str) -> None:
    if "explicit" in params:
        raw_explicit = params["explicit"]
        if set(params) != {"explicit"} or not isinstance(raw_explicit, (list, tuple)):
            raise ConfigError("boxes explicit params are invalid", code="INVALID_TERRAIN_CONFIG", path=f"{path}.params")
        for index, raw_box in enumerate(cast(Sequence[object], raw_explicit)):
            if not isinstance(raw_box, Mapping) or set(raw_box) != {"pose", "extent"}:
                raise ConfigError(
                    "boxes explicit entry is invalid",
                    code="INVALID_TERRAIN_CONFIG",
                    path=f"{path}.params.explicit[{index}]",
                )
            box = cast(Mapping[str, object], raw_box)
            pose = _finite_vector(box["pose"], 4)
            extent = _finite_vector(box["extent"], 3)
            if pose is None or extent is None:
                raise ConfigError(
                    "boxes vectors are invalid",
                    code="INVALID_TERRAIN_CONFIG",
                    path=f"{path}.params.explicit[{index}]",
                )
            if any(item <= 0 for item in extent):
                raise ConfigError(
                    "boxes extent must be positive",
                    code="INVALID_TERRAIN_CONFIG",
                    path=f"{path}.params.explicit[{index}].extent",
                )
        return
    required = {"grid_width", "grid_height_range", "holes"}
    perlin = {"perlin_scale", "perlin_octaves", "perlin_persistence", "perlin_lacunarity"}
    if "generator" in params:
        random_grid = required | {"generator", "difficulty"}
        if set(params) != random_grid or params["generator"] != "random_grid":
            raise ConfigError(
                "boxes random-grid params keys are invalid",
                code="INVALID_TERRAIN_CONFIG",
                path=f"{path}.params",
            )
        difficulty = _finite_number(params["difficulty"])
        if difficulty is None or not 0.0 <= difficulty <= 1.0:
            raise ConfigError(
                "boxes difficulty must be in [0, 1]",
                code="INVALID_TERRAIN_CONFIG",
                path=f"{path}.params.difficulty",
            )
    elif not required.issubset(params) or not set(params).issubset(required | perlin):
        raise ConfigError("boxes params keys are invalid", code="INVALID_TERRAIN_CONFIG", path=f"{path}.params")
    grid_width = _finite_number(params["grid_width"])
    grid_height_range = _ordered_range(params["grid_height_range"])
    holes = params["holes"]
    if grid_width is None or grid_width <= 0 or grid_height_range is None or type(holes) is not bool:
        raise ConfigError("boxes grid params are invalid", code="INVALID_TERRAIN_CONFIG", path=f"{path}.params")
    if "generator" in params:
        return
    _validate_perlin_params(params, path, "boxes")


def _validate_registered_binding(
    config: ResolvedRunConfig,
    registration: ConfigRegistration,
) -> None:
    """Keep a Run bound to the physical pair selected by its Task registration."""

    if config.worker.environment_id != registration.environment_id:
        raise ConfigError(
            "environment_id is fixed by the Task registration",
            code="REGISTERED_BINDING_OVERRIDE",
            path="worker.environment_id",
        )
    if config.worker.robot_id != registration.robot_id:
        raise ConfigError(
            "robot_id is fixed by the Task registration",
            code="REGISTERED_BINDING_OVERRIDE",
            path="worker.robot_id",
        )


def _hash_config(config: ResolvedRunConfig) -> str:
    payload: dict[str, Any] = _to_jsonable(config)
    payload.pop("normalized_hash")
    try:
        return sha256_hex(canonical_json(payload))
    except BridgeProtocolError as exc:
        raise ConfigError("Config contains a non-canonical value", code="INVALID_CONFIG") from exc


def _to_jsonable(value: Any) -> Any:
    return to_jsonable(value)
