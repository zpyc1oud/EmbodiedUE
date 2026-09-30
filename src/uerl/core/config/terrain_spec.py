"""Declarative terrain profile configspec used by PhantomX terrain YAML loaders."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any, Literal

from uerl.errors import ConfigError

from .configspec import MISSING, configspec, from_mapping, spec_field, to_dict

_PERLIN_KEYS = frozenset(
    {"perlin_scale", "perlin_octaves", "perlin_persistence", "perlin_lacunarity"}
)


@configspec
class TerrainHeightfieldParamsCfg:
    """Heightfield tier params; ``noise_range`` uses configspec ``ordered=True``."""

    noise_range: tuple[float, float] = spec_field(MISSING, ordered=True)
    noise_step: float = spec_field(MISSING, gt=0.0, finite=True)
    horizontal_scale: float = spec_field(MISSING, gt=0.0, finite=True)
    vertical_scale: float = spec_field(MISSING, gt=0.0, finite=True)
    downsampled_scale: float | None = spec_field(MISSING)


@configspec
class TerrainBoxesRandomGridParamsCfg:
    """Boxes random-grid params used by discrete PhantomX terrain YAML."""

    grid_width: float = spec_field(MISSING, gt=0.0, finite=True)
    grid_height_range: tuple[float, float] = spec_field(MISSING, ordered=True)
    holes: bool = spec_field(MISSING)
    generator: Literal["random_grid"] = spec_field(MISSING)
    difficulty: float = spec_field(MISSING, ge=0.0, le=1.0, finite=True)


@configspec
class TerrainBoxesGridParamsCfg:
    """Boxes grid params without an explicit generator tag."""

    grid_width: float = spec_field(MISSING, gt=0.0, finite=True)
    grid_height_range: tuple[float, float] = spec_field(MISSING, ordered=True)
    holes: bool = spec_field(MISSING)


@configspec
class TerrainPerlinParamsCfg:
    """Optional continuous Perlin source shared by mesh and box terrain."""

    perlin_scale: float = spec_field(MISSING, gt=0.0, finite=True)
    perlin_octaves: int = spec_field(MISSING, ge=1, le=8, integral=True)
    perlin_persistence: float = spec_field(MISSING, gt=0.0, le=1.0, finite=True)
    perlin_lacunarity: float = spec_field(MISSING, gt=1.0, finite=True)


@configspec
class TerrainTierCfg:
    """One curriculum tier inside a terrain profile."""

    level: int = spec_field(MISSING, ge=0, integral=True)
    primitive: Literal["plane", "heightfield", "boxes"] = spec_field(MISSING)
    seed: str = spec_field(MISSING, min_length=1)
    platform_width: float = spec_field(MISSING, ge=0.0, finite=True)
    params: dict[str, Any] = spec_field(MISSING)


@configspec
class TerrainCfg:
    """Python-owned terrain document validated at the YAML boundary."""

    num_levels: int = spec_field(MISSING, gt=0, le=65535, integral=True)
    cell_size: tuple[float, float] = spec_field(MISSING, item_gt=0.0)
    border_width: float = spec_field(MISSING, ge=0.0, finite=True)
    tiers: tuple[TerrainTierCfg, ...] = spec_field(MISSING, min_length=1)
    # Default true keeps PhantomX walkable. CartPole sets false so the plane is
    # QueryOnly — the skeletal-mesh prismatic joint is the rail, and physics
    # collision on the plane freezes the cart (same reason the old track used
    # QueryOnly).
    physics_collision: bool = True


def parse_terrain_config(document: Mapping[str, object], *, path: str = "") -> dict[str, Any]:
    """Parse a terrain mapping through configspec and return a JSON-ready dict."""

    cfg = from_mapping(TerrainCfg, dict(document), path=path)
    _validate_terrain_invariants(cfg, root_path=path)
    return to_dict(cfg)


def _validate_terrain_invariants(cfg: TerrainCfg, *, root_path: str) -> None:
    """Enforce cross-field terrain rules that configspec cannot express alone."""

    def _path(suffix: str) -> str:
        return f"{root_path}.{suffix}" if root_path else suffix

    if len(cfg.tiers) != cfg.num_levels:
        raise ConfigError(
            "terrain tiers must contain exactly num_levels entries",
            code="CONFIG_OUT_OF_RANGE",
            path=_path("tiers"),
        )
    for index, tier in enumerate(cfg.tiers):
        path = _path(f"tiers[{index}]")
        if tier.level != index:
            raise ConfigError(
                "terrain levels must be contiguous from zero",
                code="CONFIG_OUT_OF_RANGE",
                path=f"{path}.level",
            )
        _validate_seed(tier.seed, f"{path}.seed")
        _validate_tier_params(tier.primitive, tier.params, f"{path}.params")


def _validate_seed(seed: str, path: str) -> None:
    if (
        not seed
        or (len(seed) > 1 and seed.startswith("0"))
        or not seed.isascii()
        or not seed.isdecimal()
        or len(seed) > 20
        or int(seed) > 2**64 - 1
    ):
        raise ConfigError(
            "terrain seed must be a canonical unsigned 64-bit decimal string",
            code="CONFIG_OUT_OF_RANGE",
            path=path,
        )


def _validate_tier_params(primitive: str, params: Mapping[str, Any], path: str) -> None:
    if primitive == "plane":
        if params:
            raise ConfigError("plane params must be empty", code="CONFIG_OUT_OF_RANGE", path=path)
        return
    if primitive == "heightfield":
        _validate_heightfield_params(params, path)
        return
    _validate_boxes_params(params, path)


def _validate_heightfield_params(params: Mapping[str, Any], path: str) -> None:
    required = {"noise_range", "noise_step", "horizontal_scale", "vertical_scale", "downsampled_scale"}
    if not required.issubset(params) or not set(params).issubset(required | _PERLIN_KEYS):
        raise ConfigError("heightfield params keys are invalid", code="CONFIG_UNKNOWN_KEY", path=path)
    base = {key: params[key] for key in required}
    cfg = from_mapping(TerrainHeightfieldParamsCfg, base, path=path)
    if cfg.downsampled_scale is not None and cfg.downsampled_scale < cfg.horizontal_scale:
        raise ConfigError(
            "heightfield downsampled_scale must be null or no smaller than horizontal_scale",
            code="CONFIG_OUT_OF_RANGE",
            path=f"{path}.downsampled_scale",
        )
    if set(params) & _PERLIN_KEYS:
        _validate_perlin_params(params, path)


def _validate_perlin_params(params: Mapping[str, Any], path: str) -> None:
    present = set(params) & _PERLIN_KEYS
    if present != _PERLIN_KEYS:
        raise ConfigError(
            "Perlin parameters must be supplied together",
            code="CONFIG_MISSING_FIELD",
            path=path,
        )
    from_mapping(
        TerrainPerlinParamsCfg,
        {key: params[key] for key in _PERLIN_KEYS},
        path=path,
    )


def _validate_boxes_params(params: Mapping[str, Any], path: str) -> None:
    if "explicit" in params:
        raw_explicit = params["explicit"]
        if set(params) != {"explicit"} or not isinstance(raw_explicit, (list, tuple)):
            raise ConfigError("boxes explicit params are invalid", code="CONFIG_OUT_OF_RANGE", path=path)
        return
    if "generator" in params:
        from_mapping(TerrainBoxesRandomGridParamsCfg, dict(params), path=path)
        return
    required = {"grid_width", "grid_height_range", "holes"}
    if not required.issubset(params) or not set(params).issubset(required | _PERLIN_KEYS):
        raise ConfigError("boxes params keys are invalid", code="CONFIG_UNKNOWN_KEY", path=path)
    from_mapping(TerrainBoxesGridParamsCfg, {key: params[key] for key in required}, path=path)
    if set(params) & _PERLIN_KEYS:
        _validate_perlin_params(params, path)


__all__ = [
    "TerrainBoxesGridParamsCfg",
    "TerrainBoxesRandomGridParamsCfg",
    "TerrainCfg",
    "TerrainHeightfieldParamsCfg",
    "TerrainPerlinParamsCfg",
    "TerrainTierCfg",
    "parse_terrain_config",
]
