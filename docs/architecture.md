# Architecture and design rationale

UE RL Engine trains robot policies in Unreal Engine 5.8 Chaos and deploys observation/action plans and an exported network into the same physics backend. The product includes CartPole and PhantomX. Python owns training semantics; the UE plugin owns physical execution.

For executable commands, use the [README](../README.md); for precise vocabulary, use [CONTEXT](../CONTEXT.md).

## Motivation and scope

Physically driven game robots need to respond to ground geometry, slipping, and disturbances at runtime. A useful training system must produce more than loadable weights: the deployed robot must be evaluated in its intended game environment.

Training and deployment can differ in articulation representation, joint-drive response, contact/friction behavior, and unit/frame/timing conventions. Matching a stiffness number does not establish matching closed-loop dynamics. A policy can depend on contact transitions and actuator responses learned during training, so static standing alone is a weak transfer test.

The chosen approach was to build the training loop on Chaos. It trades repeated transfer tuning for infrastructure work: request-driven fixed stepping, batched isolated robot instances, Python/UE communication, sparse reset, declared observations/actions, and a validated host physics baseline. Sharing a backend reduces one source of mismatch; changes in timing, geometry, commands, or deployment conditions still require evaluation.

## Responsibility boundaries

| Concern | Owner |
|---|---|
| Bodies, joints, hierarchy, limits, mass, inertia, geometry | UE assets |
| Actuator selection, gains, reference pose, scaling, reset distributions | Python robot declarations |
| Observation/action composition, rewards, terminations, events, curriculum | Python Task and MDP managers |
| Action application, state collection, fixed stepping, isolation | UE Worker and Robot runtime |
| Framing, transport, negotiated layouts | Codec / Transport |

The interface carries numeric data. A robot declaration is compiled into the runtime semantics used during training and the plans stored in the deployment artifact. Deployment serializes the actual trained task's plans, rather than independently reconstructing them from configuration.

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

Training has one synchronous request in flight. A Step carries the batched actions and returns batched state. Timeouts, protocol errors, and ambiguous transport failures invalidate the Session instead of silently replaying requests.

The deployment controller does not need a Python training process, Worker session, or wire protocol. Its core dependency direction is `UERLPolicy → UERLRobot → UERLProvider → UERLInterface`. The plugin descriptor still declares Worker and Transport as Runtime modules; a minimal packaged module set must be checked in the actual build rather than inferred from the controller's dependency graph.

### Python modules

| Path under `src/uerl/` | Responsibility |
|---|---|
| `cli/` | Task inspection, training, playback, recording, export, deployment, and scaffolding |
| `core/config/` | Typed configuration, overrides, topology/semantics merge, identities, manifests |
| `core/codec/` | Headers, strict JSON, binary layouts, transport, protocol state |
| `core/mdp/` | Term declarations, plan compilation/execution, six managers, operators |
| `core/direct/` | Environment semantics, valid-Slot task math, terminal state capture, sparse reset merging |
| `runtime/session/` | Worker launch/attach/stop, runtime projection, tensor/segment conversion |
| `assets/robots/` | CartPole and PhantomX declarations, with derivation and regex pose selection |
| `tasks/` | Explicit task registry and task-specific factories |
| `training/` | Run configuration, PPO, evaluation, vector environment, checkpoint state, export |
| `policy/` | Artifact reading/validation and reference inference for parity |
| `presentation/` | UE frame consumption and video encoding |

`policy/` must not depend on `training/`, and generic `core/mdp/` must not depend on concrete `tasks/`. Plan operators have matching Python and C++ implementations. Dependency tests enforce these boundaries.

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

The host project owns global physics settings that must be effective before the physics scene is created. The plugin reads and validates them. Terrain geometry is generated or loaded locally in UE, not transmitted over the training protocol.

## Configuration and robot integration

Robot declarations in `assets/robots/*.py` describe how to use physical assets. Task YAML and factories describe the training run. Terrain YAML describes ground generation. Mass, inertia, geometry, and joint limits remain asset facts.

Python declarations replaced duplicate robot YAML because they support derivation and regular-expression matching directly. Numeric experiments use validated dotted-path overrides; runtime projections are generated, not maintained as a second source of truth. At initialization, reflected topology and field descriptors establish observation shapes and stable indices before the immutable Session contract is committed. Resolved configuration, git identity, build identity, and layout identity are retained with the Run.

### Robot integration

A supported robot integration adds UE content, a Python robot declaration, a Task, and trained artifacts. Existing generic runtime code is reused. See [Add a robot](how-to/add-a-robot.md).

The runtime rejects missing PhysicsAssets and unsupported structural contracts rather than guessing a robot. Reflection supplies bodies, joints, motion types, the supported unlocked coordinate, SI limits, and hierarchy. Training selects which names are actuated and observed. Declaration/descriptor mismatches fail during initialization with a field path.

CartPole provides the minimal balance/training test with one actuator; PhantomX has 18 actuators and a floating base. New mechanisms outside the current topology or operator contracts require explicit runtime design and tests; the existing integration path is not a promise of arbitrary robot support.

## Fixed physics steps and variable control intervals

The Worker advances training time only after accepting a Step request. A custom timestep gate advances the application by `physics_dt` for each permitted physics frame; idle ticks do not advance training time. The requested `step_decimation` counts down to zero, then the Worker returns state and closes the gate. Action targets remain unchanged for the whole window, including drive modes that reapply the target on each physics frame.

A Session fixes `physics_dt` and the inclusive `decimation` range. Each Step carries one value from that range. For frame k:

```text
dt_k = physics_dt × step_decimation_k
o_k -> a_k -> physics(step_decimation_k) -> o_(k+1)
```

The returned observation interval belongs to the completed control frame. The first observation after Initialize or Reset uses `DtMin`. PhantomX uses 5 ms physics steps and `[1,7]`, producing 5–35 ms control intervals. CartPole uses `1/120 s` and `[2,2]`, producing a `1/60 s` control interval. This is variable **decimation**, not per-Step variation of the solver timestep.

## Parallel Slots and collision isolation

Each Slot contains a robot instance and its task state. Robot collisions and contacts must not cross Slot boundaries; spacing alone does not enforce isolation.

| Scope | Geometry and queries |
|---|---|
| Slot-isolated | Each Slot owns geometry and uses its collision/query isolation plan |
| Shared World | Robots share procedural or authored static terrain while robot-to-robot collisions remain isolated |

The Environment places Slots on a grid, traces ground height/normal, and establishes local Ground frames. Body poses and velocities use those frames, rather than absolute UE positions. This removes absolute placement from those observations; it does not guarantee identical observation distributions on different terrain.

Flat walking defaults to 512 Slots; CartPole to 64. Continuous terrain defaults to 64 Slots with independent geometry and an 8×8 layout. Discrete terrain defaults to 64 Slots on a shared atlas. Each terrain definition is maintained once, independent of the number of generated geometry instances. Review runtime isolation and coverage before increasing parallelism.

## Actuation and observations

The actuator calculation combines stiffness, damping, effort limit, target, position, and velocity, then clamps the resulting effort. Position, effort, and passive behavior use the common actuator semantics rather than robot-specific control code.

| Observation | Shape and semantics |
|---|---|
| Joint position / velocity | Scalar per selected joint, in SI units |
| Body pose | Seven values: meters plus `xyzw` quaternion in the Slot Ground frame |
| Body linear / angular velocity | Three values in the Slot-local frame |
| Ground clearance | Selected body's height above ground |
| Contact | Binary geometric support at the last completed solver step of the control window |
| Contact force | Net impulse magnitude from that solver step divided by its duration, converted to newtons |
| Terrain-height scan | 7×5 world-horizontal footprint, heights relative to robot root |

Contact is sampled at the window end; it does not accumulate earlier collision events. Contact force is `|I| / (100 × SolverStepSeconds)`, neither the maximum force during the window nor an average over the control interval. Old impulses are not reused when no new solve completes.

The terrain scan stores `(hit.Z-root.Z)/100` in meters. Training ray queries use the Environment owner whitelist in Shared World or the Slot channel in isolated geometry. Deployment uses blocking WorldStatic. Successful hit points are cached and re-expressed relative to the current robot during brief query loss; missing initial/reset hits are errors. Contact filtering is distinct from terrain-scan filtering.

The simulation publishes dtype, shape, units, reference frame, and semantics for every field at initialization. Python compiles its observation plan from those descriptors and validates binding before stepping. Unit/frame conversion belongs at the runtime observation boundary.

## Protocol and reset

Local TCP carries strict JSON for control and negotiated binary batches for data. Frames have a fixed 48-byte big-endian header containing magic, protocol version, message type, flags, payload length, sequence, Session UUID, and layout identity. Control payloads are limited to 1 MiB and data payloads to 64 MiB.

The lifecycle includes Hello, Initialize, InitialState, Ready, Shutdown, Step, Reset, and structured errors with stable codes. Both sides enforce phase transitions before entering physics. Canonical JSON supplies configuration/schema/layout identities.

Five negotiated layouts cover initial state, action, step result, reset request, and reset result. Named segments declare dtype, shape, alignment, and zero padding. Batched robot fields have a Slot dimension; per-Step metadata such as the shared `step_decimation` scalar is separate. Layout offsets are compiled at initialization. Sparse reset carries a Slot mask, per-Slot terrain level, and reset values. Each selected Slot first restores its complete canonical physical state, then applies sampled overrides.

The first policy-visible episode starts with a normal reset of all Slots after Ready. The Initialize state establishes the runtime; it is not a substitute for sampling the initial episode distribution.

## Declarative MDP and cross-language plans

Observation and action terms compile to a topologically ordered operator graph with explicit inputs, outputs, widths, and parameters. The same plan object is executed by Python during training and serialized for C++ deployment. Rewards, termination, events, and curriculum remain Python task mathematics.

Operators cover field selection, concatenation, slicing, inverse rotation, gravity projection, commands, control-frame duration, previous/current policy action, relative joint position, scaling, offset, and clipping. PhantomX observes `control_frame_dt` scaled by 100. The plan quaternion convention is `xyzw` throughout.

A new operator requires Python and C++ implementations, reviewed expected values in shared parity cases, and a negative self-check in one change. Each implementation is compared with the reviewed expected values, rather than treating the other implementation as the oracle. See [operator admission](../engine/Plugins/UERLEngine/Source/UERLPolicy/Docs/Operators.md).

Manager-composed declarative tasks are the exportable task path. Export reads both plans directly from the task. Interfaces remain where there are actual alternative implementations, such as command sources.

## PPO, curriculum, and physical time

The training stack uses rsl-rl PPO, a vector-environment adapter, checkpointed curriculum state, and PhantomX-specific physical-time return calculations. Continuous rewards scale with actual elapsed time relative to 20 ms; event costs occur once. Discount and GAE factors are exponentiated by `actual_dt / reference_dt`. Episodes end after 20 seconds of accumulated solver time. Initial episode timeout phases are randomized per Slot, including on resume. CartPole retains fixed-step PPO behavior.

Reset distributions use deterministic stream identities. Terrain geometry is generated at Session initialization; curriculum selects a level at episode reset using progress along command direction and commanded distance. Normal changes move one level, and promotion beyond the highest tier resamples all tiers. Turning and straight paths follow the same rule.

Task terms cover velocity/yaw tracking and progress, vertical velocity, body angular velocity, uprightness, clearance, action changes, joint velocity, and falls. Commands enter observations through explicit channels. PhantomX checkpoints mark their training objective, and incompatible old-objective resumes are rejected before UE startup. Full rules are in the [training guide](how-to/phantomx-robust-training.md).

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

The JSON does not duplicate network layers, weights, normalization, or deterministic output transforms already represented in ONNX. Loading checks observation-plan width against network input and network output against action-plan policy width.

The controller compiles plans, binds the robot adapter, then executes state collection → observation plan → inference → action plan → physical commands. Host gameplay chooses the artifact, map, and commands. Actuator/observation definitions come from the artifact. Gameplay uses a completed-solver clock and a deployment-specific synchronous-substep gate; see [deployment](in-game-deployment-guide.md).

## Reproducibility and fault boundaries

A Slot fault affects one robot's physical state. Its state is marked invalid, task math avoids it, and explicit sparse reset can attempt recovery. A Session-fatal fault violates protocol, assets, plans, fixed-frame relationships, or isolation and stops the whole Session. It is not converted into ordinary task termination or silently retried.

Keep a checkpoint with its resolved configuration, manifest, commit/local diff, random seed, build/layout identities, and evaluation conditions. A weight file alone is insufficient evidence. Deterministic streams and fixed timing support reproducibility, but do not promise identical numerical trajectories across hardware or engine versions.

Python tests cover configuration and task math; protocol tests use an independent wire oracle; shared parity cases check language equivalence; UE Automation tests cover engine behavior; E2E tests start a real Editor. Required missing external dependencies cause the full runtime gate to fail. See [tests](../tests/README.md).

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

These values come from the base task YAML, not every registered task. Continuous terrain uses eight levels (noise range ±0.015 to ±0.12 m), discrete terrain six, and pursuit an authored map. The terrain tasks default to 64 Slots. Deployment parity cases use `atol=rtol=2e-5`; this numerical comparison is not a physical trajectory guarantee.

The normal workflow is configuration resolution → UE startup or attach → handshake/initialization/Ready → initial reset → Step/Reset loop → checkpoints and manifest → evaluation → export → asset import → in-game validation. Recording uses UE viewport frames without changing the configured physics frequency. For long training sessions, an already-installed environment can avoid dependency synchronization while starting a run.

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
