# Operator admission (UERLPolicy)

A new plan operator requires all four items in the same change set:

1. **Python**: add the registry entry in `src/uerl/core/mdp/operators.py`.
   Add applicable source specialization in `executor.py`.
2. **C++**: add a `RegisterPlanOperator` entry, usually in `RegisterBuiltinPlanOperators`.
   For a source operator, add the runtime connection.
3. **Parity corpus**: add at least one case under `tests/parity/cases/<operator>/`.
   Review its `expected` floating-point values.
4. **Negative self-check**: introduce a temporary defect in the C++ or Python implementation.
   Make sure that the corpus test detects it.
   Remove the temporary defect.
   Record the result in the issue.

All four items are required before merge.
Both implementations must match the reviewed `expected` field.
Do not use one implementation as the expected result for the other.

## Quaternion convention (xyzw)

All plan-layer quaternions use **xyzw**:

- Robot `body_pose` units are `m,quat_xyzw` (position then quat).
- Python `rotate_inverse` / `projected_gravity` read `quat[:, 0:3]` as xyz and
  `quat[:, 3]` as w.
- C++ constructs `FQuat(Q[0], Q[1], Q[2], Q[3])`.
  Unreal's `(X,Y,Z,W)` matches the plan's xyzw order.
  Do not interpret plan storage as wxyz.

The case `rotate_inverse/asymmetric_rotation.json` detects an xyzw/wxyz swap.
Identity and axis-aligned rotations can hide that defect.

## Built-in source ops

| Op | Arity | Params | Output width | Meaning |
|---|---|---|---|---|
| `select` | 0 | `field` | PlanOp width | Raw state field by name |
| `command` | 0 | `channel`, `width` | `params.width` | External command channel |
| `control_frame_dt` | 0 | `scale` (finite, non-zero) | 1 | Current control interval in seconds multiplied by `scale`; stateless |
| `previous_action` | 0 | `width` | `params.width` | Prior-frame policy action |
| `policy_action` | 0 | `width` | `params.width` | Current-frame policy output (action plans) |

For `command`, **Compile** rejects a plan width different from `AvailableCommands` / `command_channels` (`channels()`).
**Execute** rejects a missing channel key instead of supplying zeros.
The error message includes the channel name.

For action-plan `command_fields`, **Compile** compares the produced slot width with the physical-command declaration in `command_channels` / `AvailableCommands`.
Different widths cause rejection.
The `policy_action` width must equal `ActionPlan.policy_width`.

## Built-in transform ops (body frame)

| Op | Arity | Params | Output width | Meaning |
|---|---|---|---|---|
| `concat` | ≥1 | — | sum of inputs | concatenate along the feature axis |
| `slice` | 1 | `start`, `width` (non-neg ints) | `params.width` | contiguous sub-range of one input; used to peel `body_pose` quat `[3:7]` |
| `rotate_inverse` | 2 | — | 3 | `UnrotateVector(quat, vec)` — world vector → body |
| `projected_gravity` | 1 | `gravity` (length-3) | 3 | `rotate_inverse(quat, gravity)`; gravity is a plan param, not a hardcoded constant |
| `joint_pos_rel` | 1 | `default` (length = input width) | input width | `args[0] - default`; default pose is a plan param from robot declaration — never a C++ structural constant |
| `scale` | 1 | `factor` (float or length = input width) | input width | elementwise `args[0] * factor`; scalar broadcasts |
| `offset` | 1 | `bias` (float or length = input width) | input width | elementwise `args[0] + bias`; scalar broadcasts |
| `clip` | 1 | `low`, `high` (float) | input width | elementwise clamp to `[low, high]` |

Conventional world gravity for PhantomX-style observations is `(0, 0, -1)`.

For `joint_pos_rel`, **Compile** rejects a `default` length different from the input width.
The message includes both widths.
Evaluation performs subtraction without assumptions about joint counts, leg counts, or modulo indexing.

For `scale` and `offset`, **Compile** rejects a vector parameter length different from the input width.
For `clip`, **Compile** rejects `low > high`.
Do not depend on runtime clamp order for invalid bounds.

### NaN passthrough (shaping ops)

The operators `scale`, `offset`, and `clip` pass NaN inputs through unchanged.
They do not treat NaN as a soft error.
Upper-layer finiteness checks must terminate execution after physical divergence.
The post-control-step state is one such check location.
There is no dedicated NaN parity corpus.
