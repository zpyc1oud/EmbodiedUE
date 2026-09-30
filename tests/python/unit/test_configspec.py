"""Verify the @configspec configuration declaration helper."""

from __future__ import annotations

import math
from dataclasses import field
from typing import Any, Literal

import pytest

from uerl.core.config.canonical import canonical_json, sha256_hex, to_jsonable
from uerl.core.config.configspec import (
    MISSING,
    configspec,
    from_mapping,
    replace,
    spec_field,
    to_dict,
)
from uerl.errors import ConfigError


@configspec
class Level3Cfg:
    """Innermost nested configuration with a default."""

    gamma: float = 0.99
    name: str = "level3"


@configspec
class Level2Cfg:
    """Middle nested configuration with a default child."""

    beta: int = 2
    child: Level3Cfg = Level3Cfg()


@configspec
class Level1Cfg:
    """Outer configuration used by nested construction tests."""

    alpha: int = 1
    child: Level2Cfg = Level2Cfg()


@configspec
class RequiredInnerCfg:
    """Nested required leaf used for missing-field path checks."""

    value: float = spec_field(MISSING, gt=0.0, finite=True)


@configspec
class RequiredOuterCfg:
    """Outer required nesting used for missing-field path checks."""

    inner: RequiredInnerCfg = MISSING


@configspec
class SampleCfg:
    """Representative constrained fields from the ticket sample."""

    learning_rate: float = spec_field(MISSING, gt=0.0, finite=True)
    num_steps: int = spec_field(MISSING, gt=0, integral=True)
    clip: tuple[float, float] | None = None


@configspec
class BoundCfg:
    """Single-bound fields used to pin inclusive/exclusive boundary semantics."""

    gt_value: float = spec_field(0.0, gt=0.0)
    ge_value: float = spec_field(0.0, ge=0.0)
    lt_value: float = spec_field(0.0, lt=1.0)
    le_value: float = spec_field(0.0, le=1.0)


@configspec
class OptionalCfg:
    """Optional field used to distinguish explicit None from MISSING."""

    rate: float = spec_field(MISSING, gt=0.0, finite=True)
    label: str | None = None


def test_ac_py_unit_cfgspec_001_nested_defaults_apply_from_mapping() -> None:
    """AC_PY_UNIT_CFGSPEC_001: nested three-level mapping uses each layer's defaults."""

    # Arrange
    payload: dict[str, Any] = {"child": {"child": {}}}

    # Act
    cfg = from_mapping(Level1Cfg, payload)

    # Assert
    assert cfg.alpha == 1
    assert cfg.child.beta == 2
    assert cfg.child.child.gamma == 0.99
    assert cfg.child.child.name == "level3"


def test_ac_py_unit_cfgspec_002_missing_required_field_reports_full_path() -> None:
    """AC_PY_UNIT_CFGSPEC_002: MISSING fields raise CONFIG_MISSING_FIELD with full path."""

    # Arrange
    payload: dict[str, Any] = {"inner": {}}

    # Act / Assert
    with pytest.raises(ConfigError) as raised:
        from_mapping(RequiredOuterCfg, payload)

    assert raised.value.code == "CONFIG_MISSING_FIELD"
    assert raised.value.path == "inner.value"


def test_ac_py_unit_cfgspec_003_unknown_key_reports_path() -> None:
    """AC_PY_UNIT_CFGSPEC_003: unknown keys raise CONFIG_UNKNOWN_KEY at that path."""

    # Arrange
    payload = {"alpha": 1, "unexpected": True}

    # Act / Assert
    with pytest.raises(ConfigError) as raised:
        from_mapping(Level1Cfg, payload)

    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == "unexpected"


def test_ac_py_unit_cfgspec_003_nested_unknown_key_reports_path() -> None:
    """Unknown nested keys include the parent path."""

    payload = {"inner": {"value": 1.0, "extra": 1}}

    with pytest.raises(ConfigError) as raised:
        from_mapping(RequiredOuterCfg, payload)

    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == "inner.extra"


def test_ac_py_unit_cfgspec_004_type_mismatch() -> None:
    """AC_PY_UNIT_CFGSPEC_004: wrong types raise CONFIG_TYPE_MISMATCH."""

    with pytest.raises(ConfigError) as raised:
        from_mapping(SampleCfg, {"learning_rate": "fast", "num_steps": 1})

    assert raised.value.code == "CONFIG_TYPE_MISMATCH"
    assert raised.value.path == "learning_rate"


def test_ac_py_unit_cfgspec_005_bound_semantics() -> None:
    """AC_PY_UNIT_CFGSPEC_005: gt/ge/lt/le accept or reject values exactly on the bound."""

    # Arrange / Act — values just inside each bound succeed
    ok = from_mapping(
        BoundCfg,
        {
            "gt_value": 0.1,
            "ge_value": 0.0,
            "lt_value": 0.9,
            "le_value": 1.0,
        },
    )
    assert ok.gt_value == 0.1
    assert ok.ge_value == 0.0
    assert ok.lt_value == 0.9
    assert ok.le_value == 1.0

    # Assert — exclusive / inclusive boundary rejections
    with pytest.raises(ConfigError) as gt_err:
        from_mapping(BoundCfg, {"gt_value": 0.0, "ge_value": 0.0, "lt_value": 0.5, "le_value": 0.5})
    assert gt_err.value.code == "CONFIG_OUT_OF_RANGE"
    assert gt_err.value.path == "gt_value"

    with pytest.raises(ConfigError) as ge_err:
        from_mapping(BoundCfg, {"gt_value": 0.1, "ge_value": -0.1, "lt_value": 0.5, "le_value": 0.5})
    assert ge_err.value.code == "CONFIG_OUT_OF_RANGE"
    assert ge_err.value.path == "ge_value"

    with pytest.raises(ConfigError) as lt_err:
        from_mapping(BoundCfg, {"gt_value": 0.1, "ge_value": 0.0, "lt_value": 1.0, "le_value": 0.5})
    assert lt_err.value.code == "CONFIG_OUT_OF_RANGE"
    assert lt_err.value.path == "lt_value"

    with pytest.raises(ConfigError) as le_err:
        from_mapping(BoundCfg, {"gt_value": 0.1, "ge_value": 0.0, "lt_value": 0.5, "le_value": 1.1})
    assert le_err.value.code == "CONFIG_OUT_OF_RANGE"
    assert le_err.value.path == "le_value"


def test_ac_py_unit_cfgspec_006_finite_rejects_non_finite() -> None:
    """AC_PY_UNIT_CFGSPEC_006: finite=True rejects nan/inf/-inf with CONFIG_NOT_FINITE."""

    for bad in (math.nan, math.inf, -math.inf):
        with pytest.raises(ConfigError) as raised:
            from_mapping(SampleCfg, {"learning_rate": bad, "num_steps": 1})
        assert raised.value.code == "CONFIG_NOT_FINITE"
        assert raised.value.path == "learning_rate"


def test_ac_py_unit_cfgspec_007_integral_rejects_bool() -> None:
    """AC_PY_UNIT_CFGSPEC_007: integral=True rejects bool even though bool subclasses int."""

    with pytest.raises(ConfigError) as raised:
        from_mapping(SampleCfg, {"learning_rate": 0.1, "num_steps": True})

    assert raised.value.code == "CONFIG_TYPE_MISMATCH"
    assert raised.value.path == "num_steps"


def test_ac_py_unit_cfgspec_008_to_dict_canonical_round_trip() -> None:
    """AC_PY_UNIT_CFGSPEC_008: to_dict feeds canonical/hash and round-trips to the same instance."""

    # Arrange
    cfg = from_mapping(
        SampleCfg,
        {"learning_rate": 0.01, "num_steps": 8, "clip": [0.0, 1.0]},
    )

    # Act
    exported = to_dict(cfg)
    digest = sha256_hex(canonical_json(exported))
    restored = from_mapping(SampleCfg, exported)

    # Assert
    assert exported == {"learning_rate": 0.01, "num_steps": 8, "clip": [0.0, 1.0]}
    assert to_jsonable(exported) == exported
    assert len(digest) == 64
    assert restored == cfg


def test_replace_keeps_original_and_rejects_unknown_field() -> None:
    """replace leaves the source unchanged and rejects unknown override names."""

    cfg = from_mapping(SampleCfg, {"learning_rate": 0.01, "num_steps": 4})
    updated = replace(cfg, num_steps=8)

    assert cfg.num_steps == 4
    assert updated.num_steps == 8
    assert updated.learning_rate == 0.01

    with pytest.raises(ConfigError) as raised:
        replace(cfg, unknown=1)
    assert raised.value.code == "CONFIG_UNKNOWN_KEY"
    assert raised.value.path == "unknown"


def test_nested_configspec_accepts_constructed_instance() -> None:
    """Nested @configspec fields reuse an already-built instance as-is."""

    child = from_mapping(Level3Cfg, {"gamma": 0.5, "name": "given"})
    cfg = from_mapping(Level2Cfg, {"beta": 9, "child": child})

    assert cfg.child is child
    assert cfg.child.gamma == 0.5


def test_none_is_allowed_for_optional_fields_but_missing_required_is_not() -> None:
    """Explicit None is valid for optional fields; MISSING required fields are not."""

    cfg = from_mapping(OptionalCfg, {"label": None, "rate": 0.5})
    assert cfg.label is None
    assert cfg.rate == 0.5

    with pytest.raises(ConfigError) as raised:
        from_mapping(OptionalCfg, {"label": "x"})
    assert raised.value.code == "CONFIG_MISSING_FIELD"
    assert raised.value.path == "rate"


def test_from_mapping_fails_before_returning_instance() -> None:
    """Construction errors raise before any instance is returned."""

    result: SampleCfg | None = None
    with pytest.raises(ConfigError):
        result = from_mapping(SampleCfg, {"learning_rate": -1.0, "num_steps": 1})
    assert result is None


def test_missing_sentinel_is_falsy() -> None:
    """MISSING remains a falsy required-field sentinel."""

    assert repr(MISSING) == "MISSING"
    assert not MISSING


def test_config_not_mapping_at_root_and_nested() -> None:
    """Non-mapping payloads raise CONFIG_NOT_MAPPING."""

    with pytest.raises(ConfigError) as root:
        from_mapping(Level1Cfg, ["not", "a", "mapping"])  # type: ignore[arg-type]
    assert root.value.code == "CONFIG_NOT_MAPPING"

    with pytest.raises(ConfigError) as nested:
        from_mapping(RequiredOuterCfg, {"inner": 3})
    assert nested.value.code == "CONFIG_NOT_MAPPING"
    assert nested.value.path == "inner"


@configspec
class LiteralCfg:
    """Literal and sequence constraints used by training YAML migrations."""

    mode: Literal["train", "eval"] = spec_field(MISSING)
    name: str = spec_field(MISSING, min_length=1)
    dims: tuple[int, ...] = spec_field(MISSING, min_length=1, item_gt=0)
    optional_null: Literal[None] = spec_field(MISSING)


def test_literal_min_length_and_item_gt_constraints() -> None:
    """Literal, min_length, and item_gt reject invalid values with stable codes."""

    ok = from_mapping(
        LiteralCfg,
        {"mode": "train", "name": "x", "dims": [1, 2], "optional_null": None},
    )
    assert ok.mode == "train"
    assert ok.dims == (1, 2)

    with pytest.raises(ConfigError) as literal_error:
        from_mapping(
            LiteralCfg,
            {"mode": "other", "name": "x", "dims": [1], "optional_null": None},
        )
    assert literal_error.value.code == "CONFIG_OUT_OF_RANGE"
    assert literal_error.value.path == "mode"

    with pytest.raises(ConfigError) as empty_name:
        from_mapping(
            LiteralCfg,
            {"mode": "train", "name": "", "dims": [1], "optional_null": None},
        )
    assert empty_name.value.code == "CONFIG_OUT_OF_RANGE"
    assert empty_name.value.path == "name"

    with pytest.raises(ConfigError) as empty_dims:
        from_mapping(
            LiteralCfg,
            {"mode": "train", "name": "x", "dims": [], "optional_null": None},
        )
    assert empty_dims.value.code == "CONFIG_OUT_OF_RANGE"
    assert empty_dims.value.path == "dims"

    with pytest.raises(ConfigError) as item_gt:
        from_mapping(
            LiteralCfg,
            {"mode": "train", "name": "x", "dims": [2, 0], "optional_null": None},
        )
    assert item_gt.value.code == "CONFIG_OUT_OF_RANGE"
    assert item_gt.value.path == "dims[1]"

    with pytest.raises(ConfigError) as null_literal:
        from_mapping(
            LiteralCfg,
            {"mode": "train", "name": "x", "dims": [1], "optional_null": {}},
        )
    assert null_literal.value.code == "CONFIG_OUT_OF_RANGE"
    assert null_literal.value.path == "optional_null"


@configspec
class OrderedRangeCfg:
    """Ordered pair constraint for nested range tuples."""

    span: tuple[float, float] = spec_field(MISSING, ordered=True)


def test_ordered_tuple_requires_lower_not_greater_than_upper() -> None:
    """ordered=True rejects inverted or non-finite pairs with stable codes."""

    ok = from_mapping(OrderedRangeCfg, {"span": [-0.1, 0.2]})
    assert ok.span == (-0.1, 0.2)

    with pytest.raises(ConfigError) as inverted:
        from_mapping(OrderedRangeCfg, {"span": [1.0, 0.0]})
    assert inverted.value.code == "CONFIG_OUT_OF_RANGE"
    assert inverted.value.path == "span"

    with pytest.raises(ConfigError) as non_finite:
        from_mapping(OrderedRangeCfg, {"span": [0.0, math.nan]})
    assert non_finite.value.code == "CONFIG_NOT_FINITE"
    assert non_finite.value.path == "span[1]"


@configspec
class UnionNamesCfg:
    """Multi-alternative Union coerce used by RobotEntityCfg-style fields."""

    names: str | tuple[str, ...] | None = None


@configspec
class DerivedRoundTripCfg:
    """Public fields plus a derived underscore field that must not export."""

    label: str = ""
    _resolved_ids: tuple[int, ...] = field(default=())


def test_union_str_tuple_none_coercion() -> None:
    """Union alternatives coerce independently; null stays None."""

    as_str = from_mapping(UnionNamesCfg, {"names": "hip_.*"})
    assert as_str.names == "hip_.*"

    as_tuple = from_mapping(UnionNamesCfg, {"names": ["a", "b"]})
    assert as_tuple.names == ("a", "b")

    as_none = from_mapping(UnionNamesCfg, {"names": None})
    assert as_none.names is None

    omitted = from_mapping(UnionNamesCfg, {})
    assert omitted.names is None


def test_underscore_fields_round_trip_and_reject_overrides() -> None:
    """to_dict omits _fields; from_mapping/replace reject underscore keys."""

    cfg = DerivedRoundTripCfg(label="base", _resolved_ids=(1, 2))
    exported = to_dict(cfg)
    assert exported == {"label": "base"}
    assert "_resolved_ids" not in exported

    restored = from_mapping(DerivedRoundTripCfg, exported)
    assert restored.label == "base"
    assert restored._resolved_ids == ()

    with pytest.raises(ConfigError) as from_payload:
        from_mapping(DerivedRoundTripCfg, {"label": "x", "_resolved_ids": [9]})
    assert from_payload.value.code == "CONFIG_UNKNOWN_KEY"
    assert from_payload.value.path == "_resolved_ids"

    with pytest.raises(ConfigError) as replaced:
        replace(cfg, _resolved_ids=(3,))
    assert replaced.value.code == "CONFIG_UNKNOWN_KEY"
    assert replaced.value.path == "_resolved_ids"
