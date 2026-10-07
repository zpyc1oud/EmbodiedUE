# Architecture and design rationale

UE RL Engine trains robot policies in Unreal Engine 5.8 Chaos.
It deploys observation and action plans with an exported network into the same physics backend.
The product includes CartPole and PhantomX.
Python owns training semantics.
The UE plugin owns physical execution.

Use the [README](../README.md), [configuration guide](configuration.md), [training guide](how-to/phantomx-robust-training.md), [recording guide](how-to/record-video.md), and [deployment guide](in-game-deployment-guide.md) for executable procedures.
For scene comparisons, use a fixed map, start, command sequence, and seed.
Compare survival, Pursuit success, root height, and non-foot contact separately.
The CLI reports metrics but does not automatically compare a baseline report.

## Motivation and scope

Physical game robots must respond to ground geometry, slipping, and disturbances during execution.
Loadable weights alone do not establish a useful training result.
Evaluate the deployed robot in its intended game environment.

Training and deployment can use different articulation representations, joint-drive responses, contact behavior, friction, units, frames, and timing.
Equal stiffness values do not establish equal closed-loop dynamics.
A policy can depend on contact transitions and actuator responses learned during training.
Static standing alone is therefore a weak transfer test.

The selected design trains on Chaos.
It replaces some repeated transfer tuning with infrastructure work.
That work includes fixed stepping, isolated Robot batches, Python/UE communication, sparse reset, declared observations and actions, and a validated host baseline.
A shared backend removes one source of difference.
Timing, geometry, commands, and deployment conditions still require evaluation.

## Responsibility boundaries

| Concern | Owner |
|---|---|
| Bodies, joints, hierarchy, limits, mass, inertia, geometry | UE assets |
| Actuator selection, gains, reference pose, scaling, reset distributions | Python robot declarations |
| Observation/action composition, rewards, terminations, events, curriculum | Python Task and MDP managers |
| Action application, state collection, fixed stepping, isolation | UE Worker and Robot runtime |
| Framing, transport, negotiated layouts | Codec / Transport |

The interface carries numerical data.
The Robot declaration compiles into training runtime semantics and deployment plans.
Export serializes the actual trained Task plans.
It does not independently rebuild them from configuration.

### Training and deployment paths

```text
Python Task / PPO runner
  -> Session / Codec
  -> local TCP / UE Transport
  -> Worker: Initialize, Step, Reset
  -> Robot + Environment: physics, observations, terrain

Policy artifact: observation plan + ONNX + action plan
  -> Policy Controller
  -> Robot physics adapter
  -> Skeletal Mesh in the game
```

Training has one synchronous request in flight.
A Step carries batched actions and returns batched state.
Timeouts, protocol errors, and ambiguous transport failures invalidate the Session.
The runtime does not silently replay requests.

The deployment controller needs no Python training process, Worker Session, or wire protocol.
Its core dependency direction is `UERLPolicy → UERLRobot → UERLProvider → UERLInterface`.
The plugin descriptor still declares Worker and Transport as Runtime modules.
Examine the actual build to establish a minimal packaged module set.
The controller dependency graph alone is insufficient.

### Python modules

| Path under `src/uerl/` | Responsibility |
|---|---|
| `cli/` | Task inspection, training, playback, recording, export, deployment, and scaffolding |
| `application/` | Shared registered-default and saved-Run configuration resolution |
| `core/config/` | Typed configuration, overrides, topology/semantics merge, identities, manifests |
| `core/codec/` | Headers, strict JSON, binary layouts, transport, protocol state |
| `core/mdp/` | Term declarations, plan compilation/execution, six managers, operators |
| `core/direct/` | Environment semantics, valid-Slot task math, terminal state capture, sparse reset merging |
| `runtime/session/` | Worker launch/attach/stop, runtime projection, tensor/segment conversion |
| `assets/robots/` | CartPole and PhantomX declarations, with derivation and regex pose selection |
| `tasks/` | Explicit task registry and task-specific factories |
| `training/` | Run orchestration, PPO, evaluation, vector environment, checkpoint state, export |
| `policy/` | Artifact reading/validation and reference inference for parity |
| `presentation/` | UE frame consumption and video encoding |

The module `policy/` must not depend on `training/`.
Generic `core/mdp/` must not depend on concrete `tasks/`.
Plan operators have matching Python and C++ implementations.
Dependency tests enforce these boundaries.

### UE modules

| Module | Responsibility |
|---|---|
| `UERLInterface` | Shared POD types, topology, configuration, schemas/views, plan data and validation |
| `UERLProvider` | Robot/Environment interfaces and factories, Slot context, reset batches, faults, collision plans |
| `UERLRobot` | Generic Skeletal Mesh reflection, actuation, kinematics, observations, contact, reset, deployment adapter |
| `UERLTerrain` | Planes, heightfields, box arrays, and terrain atlases |
| `UERLPolicy` | Artifact loading, C++ plan execution, NNE inference, control loop |
| `UERLTransport` | Framing, sockets, JSON/canonical identities, binary layout compilation |
| `UERLWorker` | Lifecycle, stepping, Slot pool, Environments, safety monitoring, viewport/recording |
| `UERLPolicyEditor` | Editor-only import, reimport, asset checks, and project checks |

The host owns global physics settings that must be active before physics-scene creation.
The plugin reads and validates those settings.
UE generates or loads terrain geometry.
The training protocol does not transmit that geometry.

## Configuration and robot integration

Robot declarations in `assets/robots/*.py` describe physical asset use.
Task YAML and factories describe the training Run.
Terrain YAML describes ground generation.
Mass, inertia, geometry, and joint limits remain asset facts.

Python declarations replaced duplicate Robot YAML because they directly support derivation and regular-expression matching.
Numerical experiments use validated dotted-path overrides.
Runtime projections are generated, not maintained as a second configuration source.
Initialization reflects topology and field descriptors to establish observation shapes and stable indices.
It then commits the immutable Session contract.
The Run keeps resolved configuration, framework provenance, build identity, and layout identity.
Source checkouts record Git commit, ref and dirty status. Installed wheels record
`commit: unavailable`, `ref: package:ue-rl-engine==<version>` and `dirty: false`;
Git state is unavailable for a wheel, so that value does not claim a clean checkout.
The same identity is retained in policy artifacts and video audit metadata.

### Robot integration

A supported Robot integration adds UE content, a Python Robot declaration, a Task, and trained artifacts.
It uses existing generic runtime code.
See [Add a robot](how-to/add-a-robot.md).

The runtime rejects missing PhysicsAssets and unsupported structural contracts.
It does not guess a Robot configuration.
Reflection supplies bodies, joints, motion types, the supported unlocked coordinate, SI limits, and hierarchy.
Training selects names for actuation and observation.
A declaration/descriptor mismatch fails initialization with a field path.

CartPole has one actuator and supplies a minimal balance and training test.
PhantomX has 18 actuators and a floating base.
Mechanisms outside the supported topology or operator contracts require explicit runtime design and tests.
The existing integration path does not promise arbitrary Robot support.

## Fixed physics steps and variable control intervals

The Worker advances training time only after it accepts a Step request.
A custom timestep gate advances the application by `physics_dt` for each permitted physics frame.
Idle ticks do not advance training time.
When `step_decimation` reaches zero, the Worker returns state and closes the gate.
Action targets stay constant for the complete window.
This includes drive modes that apply the same target again on each physics frame.

A Session fixes `physics_dt` and the inclusive `decimation` range.
Each Step carries one value within that range.
For frame k:

```text
dt_k = physics_dt × step_decimation_k
o_k -> a_k -> physics(step_decimation_k) -> o_(k+1)
```

The returned observation interval belongs to the completed control frame.
The first observation after Initialize or Reset uses `DtMin`.
Deployment runs that reset-state decision once in PrePhysics, then reads each completed window in PostPhysics.
The bootstrap has zero elapsed physics and no completed solver-step impulse denominator.
PhantomX uses 5 ms physics steps and `[1,7]`, for 5–35 ms control intervals.
CartPole uses `1/120 s` and `[2,2]`, for a `1/60 s` control interval.
This is variable **decimation**.
The solver timestep does not change with each Step.

## Parallel Slots and collision isolation

Each Slot contains one Robot instance and its Task state.
Robot collisions and contacts must not cross Slot boundaries.
Distance between Slots alone does not enforce isolation.

| Scope | Geometry and queries |
|---|---|
| Slot-isolated | Each Slot owns geometry and uses its collision/query isolation plan |
| Shared World | Robots share procedural or authored static terrain while robot-to-robot collisions remain isolated |

The Environment places Slots on a grid, traces ground height and normal, and establishes local Ground frames.
Body poses and velocities use these frames instead of absolute UE positions.
This removes absolute placement from those observations.
It does not guarantee equal observation distributions on different terrain.

Flat walking defaults to 512 Slots, and CartPole defaults to 64.
Continuous terrain uses 64 Slots with independent geometry and an 8×8 layout.
Discrete terrain uses 64 Slots on a shared atlas.
Each terrain definition is maintained once, independently of the generated geometry count.
Before an increase in parallelism, examine isolation and terrain coverage.

## Actuation and observations

The actuator calculation uses stiffness, damping, effort limit, target, position, and velocity.
It clamps the resulting effort.
Position, effort, and passive behavior share actuator semantics instead of robot-specific control code.

| Observation | Shape and semantics |
|---|---|
| Joint position / velocity | Scalar per selected joint, in SI units |
| Body pose | Seven values: meters plus `xyzw` quaternion in the Slot Ground frame |
| Body linear / angular velocity | Three values in the Slot-local frame |
| Ground clearance | Selected body's height above ground |
| Contact | Binary geometric support at the last completed solver step of the control window |
| Contact force | Net impulse magnitude from that solver step divided by its duration, converted to newtons |
| Terrain-height scan | 7×5 world-horizontal footprint, heights relative to robot root |

Contact samples the final completed solver step at the window end.
It does not accumulate earlier collision events.
Contact force is `|I| / (100 × SolverStepSeconds)`.
It is neither the window maximum nor an average over the control interval.
Without a new completed solve, the runtime does not reuse old impulses.

The terrain scan stores `(hit.Z-root.Z)/100` in meters.
Training ray queries use the Environment owner whitelist in Shared World or the Slot channel in isolated geometry.
Deployment uses blocking WorldStatic.
During brief query loss, cached successful hit points supply new root-relative values.
Missing initial or reset hits cause errors.
Contact filtering is separate from terrain-scan filtering.

At initialization, simulation publishes each field’s dtype, shape, units, reference frame, and semantics.
Python compiles the observation plan from those descriptors and validates binding before stepping.
Unit and frame conversion occurs at the runtime observation boundary.

## Protocol and reset

Local TCP carries strict JSON control messages and negotiated binary data batches.
The fixed 48-byte big-endian header contains magic, version, message type, flags, payload length, sequence, Session UUID, and layout identity.
Control payloads have a 1 MiB limit.
Data payloads have a 64 MiB limit.

The lifecycle includes Hello, Initialize, InitialState, Ready, Shutdown, Step, Reset, and structured errors with stable codes.
Both sides enforce phase transitions before physics execution.
Canonical JSON supplies configuration, schema, and layout identities.

Five negotiated layouts cover initial state, action, step result, reset request, and reset result.
Named segments declare dtype, shape, alignment, and zero padding.
Batched Robot fields have a Slot dimension.
Per-Step metadata, such as the shared `step_decimation` scalar, is separate.
Initialization compiles layout offsets.
Sparse reset carries a Slot mask, per-Slot terrain level, and reset values.

Each selected Slot first restores its complete canonical physical state.
It then applies sampled overrides.

The first policy-visible episode starts with a normal reset of all Slots after Ready.
Initialize state establishes the runtime.
It does not replace a sample from the initial episode distribution.

## Declarative MDP and cross-language plans

Observation and action terms compile into a topologically ordered operator graph.
The graph has explicit inputs, outputs, widths, and parameters.
Python executes the same plan object that export serializes for C++ deployment.
Rewards, termination, events, and curriculum remain Python Task mathematics.

Operators include field selection, concatenation, slicing, inverse rotation, gravity projection, commands, and control-frame duration.
They also include previous/current policy action, relative joint position, scaling, offset, and clipping.
PhantomX observes `control_frame_dt` scaled by 100.
All plan quaternions use `xyzw`.

A new operator requires Python and C++ implementations, reviewed parity values, and a negative self-check in one change.
Each implementation must match the reviewed expected values.
Neither implementation is the reference result for the other.
See [operator admission](../engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md).

Manager-composed declarative Tasks supply the exportable Task path.
Export reads both plans directly from the Task.
Interfaces remain where alternative implementations exist, such as command sources.

## PPO, curriculum, and physical time

Training uses rsl-rl PPO, a vector-environment adapter, saved curriculum state, and PhantomX physical-time return calculations.
Continuous rewards scale with elapsed time relative to 20 ms.
Event costs occur once.
Discount and GAE factors use the exponent `actual_dt / reference_dt`.
Episodes end after 20 seconds of accumulated solver time.
Initial episode timeout phases are random per Slot, including after resume.

CartPole keeps fixed-step PPO behavior.

Reset distributions use deterministic stream identities.
The Session generates terrain geometry during initialization.
At episode reset, curriculum selects a level from command-direction progress and commanded distance.
Normal changes move one level.
Promotion above the highest tier causes random selection across all tiers.
Turning and straight paths use the same rule.

Task terms cover velocity/yaw tracking and progress, vertical velocity, angular velocity, uprightness, clearance, action changes, joint velocity, and falls.
Commands enter observations through explicit channels.
PhantomX checkpoints identify their training objective.
Incompatible old-objective resume fails before UE startup.
See the [training guide](how-to/phantomx-robust-training.md) for complete rules.

## Artifact and in-game inference

The deployment container has this structure:

```text
magic        "UERLPOL2" (8 bytes)
format_ver   uint32
json_length  uint32
json_bytes   plans, task/robot identity, timing, runtime metadata
onnx_length  uint64
onnx_bytes   network exported by the training framework
```

The JSON does not duplicate network layers, weights, normalization, or deterministic output transforms already in ONNX.
Loading compares the observation-plan width with network input width.
It compares network output width with the action-plan policy width.

The controller compiles plans and binds the Robot adapter.
It then executes state collection → observation plan → inference → action plan → physical commands.
Host gameplay selects the artifact, map, and commands.
Actuator and observation definitions come from the artifact.
Gameplay uses a completed solver clock and a deployment-specific synchronous-substep gate.
See [deployment](in-game-deployment-guide.md).

## Reproducibility and fault boundaries

A Slot fault affects one Robot’s physical state.
The runtime marks that state invalid, and Task mathematics excludes it.
An explicit sparse reset can attempt recovery.
A Session-fatal fault affects protocol, assets, plans, fixed-frame relationships, or isolation and stops the complete Session.
It does not become ordinary Task termination or cause a silent retry.

Keep each checkpoint with its resolved configuration, manifest, commit, dirty diff, seed, build/layout identities, and evaluation conditions.
A weights file alone is insufficient evidence.
Deterministic streams and fixed timing support repeatable execution.
They do not guarantee equal numerical trajectories across hardware or engine versions.

Python tests cover configuration and Task mathematics.
Protocol cases use an independent wire oracle.
Shared parity cases establish language equivalence against reviewed data.
UE Automation covers native behavior, and E2E cases start a real Editor.
Missing required external dependencies cause the full runtime gate to fail.
See [tests](../tests/README.md) and [Write tests](how-to/write-tests.md).

## Reference defaults and workflow

| Parameter | CartPole | PhantomX flat walking |
|---|---|---|
| Actuators | 1 | 18 |
| Base | Track-constrained | Floating |
| Slots | 64 | 512 |
| Physics timestep | 1/120 s | 0.005 s |
| Decimation | `[2,2]` | `[1,7]` |
| Control interval | 1/60 s | 5–35 ms |
| Environment | Shared World, flat ground | Shared World; no procedural atlas in base walking YAML |
| Exteroception | None | 7×5 terrain scan and foot contacts; base contact force is available to task logic |
| Episode limit | 300 control steps | 20 simulated seconds |
| PPO MLP | 2×32, ELU, learning rate 1e-3, no observation normalization | 3×128, ELU, learning rate 3e-4, observation normalization |

These values come from base Task YAML, not every registered Task.
Continuous terrain has eight levels with noise ranges from ±0.015 to ±0.12 m.
Discrete terrain has six levels, and Pursuit uses an authored map.
Terrain Tasks default to 64 Slots.
Deployment parity cases use `atol=rtol=2e-5`.
That numerical limit is not a physical trajectory guarantee.

The normal procedure is configuration resolution → UE startup or attachment → handshake/initialization/Ready → initial reset.
Next come the Step/Reset loop, checkpoints and manifest, evaluation, export, asset import, and in-game validation.
Video recording uses UE viewport frames without changes to the configured physics frequency.
For a long training session, a prepared environment can avoid dependency synchronization at startup.

Use the [README](../README.md), [configuration guide](configuration.md), [training guide](how-to/phantomx-robust-training.md), [recording guide](how-to/record-video.md), and [deployment guide](in-game-deployment-guide.md) for executable procedures. Use a fixed map, start, command sequence, and seed when comparing scene generalization. Compare survival, pursuit success, root height, and non-foot contact individually; the CLI reports metrics but does not automatically compare a baseline report.

## Additional terminology

| Term | Meaning |
|---|---|
| Robot asset declaration | Python asset reference and semantics, supporting derivation |
| Observation shape table | Shapes, units, frames, and semantics derived from simulation field descriptors |
| Plan | Compiled operator graph stored as data and evaluated by Python or C++ |
| Policy artifact | Observation/action plans bound to an exported network and runtime contract |
| Scene randomization | Training variation in ground friction and disturbances; terrain tiers are selected at reset |
| Authored-map baseline | Repeatable evaluation on a fixed map/start/seed, compared metric by metric |
