"""Closed operator registry for observation/action plan evaluation.

Source vs transform operators
-----------------------------
*Source* ops (``select``, ``command``, ``control_frame_dt``,
``previous_action``, ``policy_action``)
need ``PlanInputs`` (raw state, commands, previous/current action). The
registry stores arity, param rules, and width policy; ``evaluate`` is a
sentinel. The executor specializes them.

*Transform* ops (``concat``, ``slice``, ``rotate_inverse``, ``projected_gravity``,
``joint_pos_rel``, ``scale``, ``offset``, ``clip``, …) are pure tensor
functions: ``evaluate(args, params)`` with no access to ``PlanInputs``.

Quaternion convention
---------------------
Plan-layer quaternions are **xyzw** (matching robot ``body_pose`` unit
``m,quat_xyzw``). Never treat plan quats as wxyz.

Stable ``ConfigError.code`` values:

+---------------------+----------------------------------------------+
| code                | meaning                                      |
+=====================+==============================================+
| ``OP_DUPLICATE``    | ``register_operator`` name already present   |
| ``OP_UNKNOWN``      | ``require_operator`` name not registered     |
| ``OP_ARITY``        | plan op arity does not match the spec        |
| ``OP_PARAM``        | required param missing or wrong type         |
| ``OP_WIDTH``        | declared width disagrees with derived/actual |
+---------------------+----------------------------------------------+
"""

from __future__ import annotations

import math
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass, field
from types import MappingProxyType
from typing import Final, TypeGuard, cast

import torch

from uerl.core.mdp.plan import ParamValue, PlanOp
from uerl.errors import ConfigError

# Sentinel evaluate for source ops. PlanExecutor must specialize these; calling
# the sentinel directly is a programming error.
_SOURCE_EVALUATE: Final[object] = object()

OutputWidthFn = Callable[[Sequence[int], Mapping[str, ParamValue]], int]
EvaluateFn = Callable[[Sequence[torch.Tensor], Mapping[str, ParamValue]], torch.Tensor]


@dataclass(frozen=True, slots=True)
class OperatorSpec:
    """One registered plan operator."""

    name: str
    arity: int  # -1 = variadic
    output_width: OutputWidthFn
    evaluate: EvaluateFn | object
    # A single type, or a tuple of types meaning a union (e.g. ``(float, tuple)``).
    param_schema: Mapping[str, type | tuple[type, ...]] = field(default_factory=dict)

    def __post_init__(self) -> None:
        object.__setattr__(self, "param_schema", MappingProxyType(dict(self.param_schema)))

    @property
    def is_source(self) -> bool:
        """True when the executor must specialize this op against PlanInputs."""

        return self.evaluate is _SOURCE_EVALUATE


_REGISTRY: dict[str, OperatorSpec] = {}


def register_operator(spec: OperatorSpec) -> None:
    """Register an operator. Duplicate names raise — the set is closed."""

    if spec.name in _REGISTRY:
        raise ConfigError(
            f"operator {spec.name!r} is already registered",
            code="OP_DUPLICATE",
            path=spec.name,
        )
    _REGISTRY[spec.name] = spec


def require_operator(name: str) -> OperatorSpec:
    """Look up an operator or raise with the registered name list."""

    spec = _REGISTRY.get(name)
    if spec is None:
        registered = ", ".join(sorted(_REGISTRY)) or "(none)"
        raise ConfigError(
            f"unknown operator {name!r}; registered: {registered}",
            code="OP_UNKNOWN",
            path=name,
        )
    return spec


def registered_operators() -> tuple[str, ...]:
    """Return registered operator names in sorted order (for diagnostics/tests)."""

    return tuple(sorted(_REGISTRY))


def _concat_output_width(widths: Sequence[int], _params: Mapping[str, ParamValue]) -> int:
    return int(sum(widths))


def _concat_evaluate(
    args: Sequence[torch.Tensor],
    _params: Mapping[str, ParamValue],
) -> torch.Tensor:
    return torch.cat(tuple(args), dim=-1)


def _quat_rotate_inverse(quat_xyzw: torch.Tensor, vector: torch.Tensor) -> torch.Tensor:
    """Rotate ``vector`` by the inverse of unit quaternion ``quat_xyzw`` (xyzw)."""

    xyz = quat_xyzw[:, :3]
    w = quat_xyzw[:, 3:4]
    return cast(
        torch.Tensor,
        vector * (2.0 * w.square() - 1.0)
        - 2.0 * w * torch.linalg.cross(xyz, vector, dim=1)
        + 2.0 * xyz * (xyz * vector).sum(dim=1, keepdim=True),
    )


def _rotate_inverse_evaluate(
    args: Sequence[torch.Tensor],
    _params: Mapping[str, ParamValue],
) -> torch.Tensor:
    return _quat_rotate_inverse(args[0], args[1])


def _broadcast_vec3(gravity: ParamValue, quat: torch.Tensor) -> torch.Tensor:
    if not isinstance(gravity, tuple) or len(gravity) != 3:
        raise ConfigError(
            "operator 'projected_gravity' param 'gravity' must be a length-3 numeric tuple",
            code="OP_PARAM",
            path="params.gravity",
        )
    return torch.as_tensor(gravity, dtype=quat.dtype, device=quat.device).expand(quat.shape[0], 3)


def _projected_gravity_evaluate(
    args: Sequence[torch.Tensor],
    params: Mapping[str, ParamValue],
) -> torch.Tensor:
    quat = args[0]
    return _quat_rotate_inverse(quat, _broadcast_vec3(params["gravity"], quat))


def _fixed_width_3(_widths: Sequence[int], _params: Mapping[str, ParamValue]) -> int:
    return 3


def _fixed_width_1(_widths: Sequence[int], _params: Mapping[str, ParamValue]) -> int:
    return 1


def _identity_width(widths: Sequence[int], _params: Mapping[str, ParamValue]) -> int:
    return int(widths[0])


def _is_numeric_scalar(value: ParamValue) -> TypeGuard[float | int]:
    return isinstance(value, (int, float)) and not isinstance(value, bool)


def _as_row(value: ParamValue, reference: torch.Tensor, *, path: str = "params") -> torch.Tensor:
    """Broadcast a scalar or tile a vector to ``(batch, width)`` matching ``reference``.

    Scalars (int/float) broadcast to full width. Vectors must already equal the
    input width (Compile enforces that); a length-1 vector is not a scalar.
    """

    width = int(reference.shape[-1])
    if _is_numeric_scalar(value):
        return torch.full(
            (reference.shape[0], width),
            float(value),
            dtype=reference.dtype,
            device=reference.device,
        )
    if isinstance(value, tuple):
        row = torch.as_tensor(value, dtype=reference.dtype, device=reference.device)
        if row.ndim != 1 or row.shape[0] != width:
            raise ConfigError(
                f"vector param has {row.numel()} entries but its input is {width} wide",
                code="OP_PARAM",
                path=path,
            )
        return row.unsqueeze(0).expand(reference.shape[0], -1)
    raise ConfigError(
        f"param must be a number or numeric tuple, got {type(value).__name__}",
        code="OP_PARAM",
        path=path,
    )


def _joint_pos_rel_evaluate(
    args: Sequence[torch.Tensor],
    params: Mapping[str, ParamValue],
) -> torch.Tensor:
    return args[0] - _as_row(params["default"], args[0], path="params.default")


def check_joint_pos_rel(op: PlanOp, input_widths: Sequence[int], *, path: str) -> None:
    """Compile-time: ``default`` length must equal the single input width."""

    default = op.params["default"]
    assert isinstance(default, tuple)
    if len(default) != input_widths[0]:
        raise ConfigError(
            f"joint_pos_rel default has {len(default)} entries "
            f"but its input is {input_widths[0]} wide",
            code="OP_PARAM",
            path=f"{path}.params.default",
        )


def _check_vector_param_width(
    op: PlanOp,
    *,
    key: str,
    input_width: int,
    path: str,
) -> None:
    """Compile-time: if ``key`` is a vector, its length must equal ``input_width``."""

    value = op.params[key]
    if isinstance(value, tuple) and len(value) != input_width:
        raise ConfigError(
            f"{op.op} {key} has {len(value)} entries but its input is {input_width} wide",
            code="OP_PARAM",
            path=f"{path}.params.{key}",
        )


def check_scale(op: PlanOp, input_widths: Sequence[int], *, path: str) -> None:
    """Compile-time: vector ``factor`` length must equal the input width."""

    _check_vector_param_width(op, key="factor", input_width=input_widths[0], path=path)


def check_offset(op: PlanOp, input_widths: Sequence[int], *, path: str) -> None:
    """Compile-time: vector ``bias`` length must equal the input width."""

    _check_vector_param_width(op, key="bias", input_width=input_widths[0], path=path)


def check_clip(op: PlanOp, input_widths: Sequence[int], *, path: str) -> None:
    """Compile-time: ``low`` must not exceed ``high``."""

    del input_widths  # clip bounds are scalars; width unused
    low = op.params["low"]
    high = op.params["high"]
    assert _is_numeric_scalar(low) and _is_numeric_scalar(high)
    if float(low) > float(high):
        raise ConfigError(
            f"clip low ({float(low):g}) > high ({float(high):g})",
            code="OP_PARAM",
            path=f"{path}.params",
        )


def check_control_frame_dt(op: PlanOp, *, path: str) -> None:
    """Compile-time validation for the stateless control-frame source."""

    scale = op.params["scale"]
    assert _is_numeric_scalar(scale)
    if not math.isfinite(float(scale)) or float(scale) == 0.0:
        raise ConfigError(
            "control_frame_dt scale must be finite and non-zero",
            code="OP_PARAM",
            path=f"{path}.params.scale",
        )


def check_slice(op: PlanOp, input_widths: Sequence[int], *, path: str) -> None:
    """Compile-time: ``start``/``width`` must be non-negative ints inside the input."""

    start = op.params["start"]
    width = op.params["width"]
    assert _is_numeric_scalar(start) and _is_numeric_scalar(width)
    start_i = int(start)
    width_i = int(width)
    if start != start_i or width != width_i or start_i < 0 or width_i <= 0:
        raise ConfigError(
            "slice start/width must be non-negative integers with width > 0",
            code="OP_PARAM",
            path=f"{path}.params",
        )
    if start_i + width_i > input_widths[0]:
        raise ConfigError(
            f"slice start {start_i} + width {width_i} exceeds input width {input_widths[0]}",
            code="OP_PARAM",
            path=f"{path}.params",
        )


def _slice_output_width(
    _widths: Sequence[int], params: Mapping[str, ParamValue]
) -> int:
    width = params["width"]
    assert _is_numeric_scalar(width)
    return int(width)


def _slice_evaluate(
    args: Sequence[torch.Tensor],
    params: Mapping[str, ParamValue],
) -> torch.Tensor:
    start = int(params["start"])  # type: ignore[arg-type]
    width = int(params["width"])  # type: ignore[arg-type]
    return args[0][..., start : start + width]


def _scale_evaluate(
    args: Sequence[torch.Tensor],
    params: Mapping[str, ParamValue],
) -> torch.Tensor:
    return args[0] * _as_row(params["factor"], args[0], path="params.factor")


def _offset_evaluate(
    args: Sequence[torch.Tensor],
    params: Mapping[str, ParamValue],
) -> torch.Tensor:
    return args[0] + _as_row(params["bias"], args[0], path="params.bias")


def _clip_evaluate(
    args: Sequence[torch.Tensor],
    params: Mapping[str, ParamValue],
) -> torch.Tensor:
    low = params["low"]
    high = params["high"]
    assert _is_numeric_scalar(low) and _is_numeric_scalar(high)
    return args[0].clamp(min=float(low), max=float(high))


def _matches_param_type(value: ParamValue, expected: type | tuple[type, ...]) -> bool:
    """Return True when ``value`` matches ``expected`` (single type or union)."""

    options = expected if isinstance(expected, tuple) else (expected,)
    for option in options:
        if option is float:
            if _is_numeric_scalar(value):
                return True
        elif option is int:
            if type(value) is int:
                return True
        elif isinstance(value, option):
            return True
    return False


def _param_type_label(expected: type | tuple[type, ...]) -> str:
    options = expected if isinstance(expected, tuple) else (expected,)
    return " or ".join(t.__name__ for t in options)


def _check_operator_arity(spec: OperatorSpec, input_count: int, *, path: str) -> None:
    """Reject arity mismatches (``-1`` means any non-negative count)."""

    if spec.arity == -1:
        return
    if input_count != spec.arity:
        raise ConfigError(
            f"operator {spec.name!r} expects arity {spec.arity}, got {input_count}",
            code="OP_ARITY",
            path=path,
        )


def _check_operator_params(
    spec: OperatorSpec,
    params: Mapping[str, ParamValue],
    *,
    path: str,
) -> None:
    """Validate required params against ``param_schema`` (extra keys allowed)."""

    for key, expected in spec.param_schema.items():
        param_path = f"{path}.params.{key}"
        if key not in params:
            raise ConfigError(
                f"operator {spec.name!r} missing required param {key!r}",
                code="OP_PARAM",
                path=param_path,
            )
        value = params[key]
        if not _matches_param_type(value, expected):
            raise ConfigError(
                f"operator {spec.name!r} param {key!r} must be {_param_type_label(expected)}, "
                f"got {type(value).__name__}",
                code="OP_PARAM",
                path=param_path,
            )


def _param_width(_widths: Sequence[int], params: Mapping[str, ParamValue]) -> int:
    return cast(int, params["width"])


# Built-in closed set.
register_operator(
    OperatorSpec(
        name="select",
        arity=0,
        # Width lives on PlanOp (ticket 02); source derivation is skipped in the executor.
        output_width=lambda _widths, _params: 0,
        evaluate=_SOURCE_EVALUATE,
        param_schema={"field": str},
    )
)
register_operator(
    OperatorSpec(
        name="command",
        arity=0,
        output_width=_param_width,
        evaluate=_SOURCE_EVALUATE,
        param_schema={"channel": str, "width": int},
    )
)
register_operator(
    OperatorSpec(
        name="control_frame_dt",
        arity=0,
        output_width=_fixed_width_1,
        evaluate=_SOURCE_EVALUATE,
        param_schema={"scale": float},
    )
)
register_operator(
    OperatorSpec(
        name="previous_action",
        arity=0,
        output_width=_param_width,
        evaluate=_SOURCE_EVALUATE,
        param_schema={"width": int},
    )
)
register_operator(
    OperatorSpec(
        name="policy_action",
        arity=0,
        output_width=_param_width,
        evaluate=_SOURCE_EVALUATE,
        param_schema={"width": int},
    )
)
register_operator(
    OperatorSpec(
        name="concat",
        arity=-1,
        output_width=_concat_output_width,
        evaluate=_concat_evaluate,
        param_schema={},
    )
)
register_operator(
    OperatorSpec(
        name="slice",
        arity=1,
        output_width=_slice_output_width,
        evaluate=_slice_evaluate,
        param_schema={"start": (int, float), "width": (int, float)},
    )
)
register_operator(
    OperatorSpec(
        name="rotate_inverse",
        arity=2,
        output_width=_fixed_width_3,
        evaluate=_rotate_inverse_evaluate,
        param_schema={},
    )
)
register_operator(
    OperatorSpec(
        name="projected_gravity",
        arity=1,
        output_width=_fixed_width_3,
        evaluate=_projected_gravity_evaluate,
        # Conventional world gravity is (0, 0, -1); always pass explicitly.
        param_schema={"gravity": tuple},
    )
)
register_operator(
    OperatorSpec(
        name="joint_pos_rel",
        arity=1,
        output_width=_identity_width,
        evaluate=_joint_pos_rel_evaluate,
        # Default pose is an operator param from robot declaration — never a C++ constant.
        param_schema={"default": tuple},
    )
)
register_operator(
    OperatorSpec(
        name="scale",
        arity=1,
        output_width=_identity_width,
        evaluate=_scale_evaluate,
        param_schema={"factor": (float, tuple)},
    )
)
register_operator(
    OperatorSpec(
        name="offset",
        arity=1,
        output_width=_identity_width,
        evaluate=_offset_evaluate,
        param_schema={"bias": (float, tuple)},
    )
)
register_operator(
    OperatorSpec(
        name="clip",
        arity=1,
        output_width=_identity_width,
        evaluate=_clip_evaluate,
        param_schema={"low": float, "high": float},
    )
)
