# Operator admission (UERLPolicy)

A new plan operator must land in one change set with all four of:

1. **Python** — registry entry in `src/uerl/core/mdp/operators.py` and any
   source specialization in `executor.py`.
2. **C++** — `RegisterPlanOperator` entry (usually in
   `RegisterBuiltinPlanOperators`) and runtime wiring if it is a source op.
3. **Parity corpus** — at least one case under
   `tests/parity/cases/<operator>/` with reviewed `expected` floats.
4. **Negative self-check** — temporarily break the C++ (or Python) implementation
   and confirm the corpus test fails; record the check on the ticket.

Missing any of the four blocks merge. Both sides assert against the reviewed
`expected` field — never against each other.

## Quaternion convention (xyzw)

Plan-layer quaternions are **xyzw** everywhere:

- Robot `body_pose` units are `m,quat_xyzw` (position then quat).
- Python `rotate_inverse` / `projected_gravity` read `quat[:, 0:3]` as xyz and
  `quat[:, 3]` as w.
- C++ builds `FQuat(Q[0], Q[1], Q[2], Q[3])` — Unreal's `(X,Y,Z,W)` matches
  plan xyzw. Do **not** pass plan storage as if it were wxyz.

Corpus case `rotate_inverse/asymmetric_rotation.json` exists specifically so an
xyzw/wxyz swap fails parity (identity and axis-aligned rotations can hide the bug).

## Built-in source ops

| Op | Arity | Params | Output width | Meaning |
|---|---|---|---|---|
| `select` | 0 | `field` | PlanOp width | Raw state field by name |
| `command` | 0 | `channel`, `width` | `params.width` | External command channel |
| `control_frame_dt` | 0 | `scale` (finite, non-zero) | 1 | Current control interval in seconds multiplied by `scale`; stateless |
| `previous_action` | 0 | `width` | `params.width` | Prior-frame policy action |
| `policy_action` | 0 | `width` | `params.width` | Current-frame policy output (action plans) |

`command` error timing: **Compile** rejects plan width ≠ `AvailableCommands` /
`command_channels` (`channels()`); **Execute** rejects a missing channel key
(no silent zero-fill). Message includes the channel name.

Action-plan `command_fields` error timing: **Compile** rejects produced slot
width ≠ physical-command declaration in `command_channels` /
`AvailableCommands`. `policy_action` width must equal `ActionPlan.policy_width`.

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

`joint_pos_rel` rejects `default` length ≠ input width at **Compile** (message includes both widths). Evaluate is plain subtraction with no joint-count / leg-count / modulo assumptions.

`scale` / `offset` reject vector-param length ≠ input width at **Compile**. `clip` rejects `low > high` at **Compile** (do not rely on runtime clamp order for illegal bounds).

### NaN passthrough (shaping ops)

`scale`, `offset`, and `clip` pass NaN inputs through unchanged. Operators do not treat NaN as a soft error; upper-layer finiteness checks (e.g. post-control-step state) are responsible for terminating on physical divergence. There is no dedicated NaN parity corpus.
