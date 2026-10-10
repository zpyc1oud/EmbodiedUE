# Architecture and extension reference

[Project home](../README.md)

## Contents

- [Architecture and design rationale](#architecture)
- [EmbodiedUE Context](#context)
- [Operator admission (UERLPolicy)](#operators)


<a id="architecture"></a>

<a id="architecture-architecture-and-design-rationale"></a>
## Architecture and design rationale

EmbodiedUE is a robot training and deployment platform for Unreal Engine game developers.
Its [product direction](../README.md#from-training-to-gameplay) puts in-game physical behavior at the center of the design.
It trains policies in UE 5.8 Chaos and deploys observation and action plans with the exported network.
CartPole and PhantomX are the current reference integrations.
Python owns training semantics.
The UE plugin owns physical execution.
Gameplay code supplies task-level commands to the controller.

Use the [README](../README.md), [configuration guide](training.md#configuration), [training guide](training.md#phantomx-robust-training), [recording guide](training.md#record-video), and [deployment guide](deployment.md#in-game-deployment-guide) for executable procedures.
For scene comparisons, use a fixed map, start, command sequence, and seed.
Compare survival, Pursuit success, root height, and non-foot contact separately.
The CLI reports metrics but does not automatically compare a baseline report.

<a id="architecture-motivation-and-scope"></a>
### Motivation and scope

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

<a id="architecture-responsibility-boundaries"></a>
### Responsibility boundaries

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

<a id="architecture-training-and-deployment-paths"></a>
#### Training and deployment paths

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

<a id="architecture-python-modules"></a>
#### Python modules

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

<a id="architecture-ue-modules"></a>
#### UE modules

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

<a id="architecture-configuration-and-robot-integration"></a>
### Configuration and robot integration

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

<a id="architecture-robot-integration"></a>
#### Robot integration

A supported Robot integration adds UE content, a Python Robot declaration, a Task, and trained artifacts.
It uses existing generic runtime code.
See [Add a robot](training.md#add-a-robot).

The runtime rejects missing PhysicsAssets and unsupported structural contracts.
It does not guess a Robot configuration.
Reflection supplies bodies, joints, motion types, the supported unlocked coordinate, SI limits, and hierarchy.
Training selects names for actuation and observation.
A declaration/descriptor mismatch fails initialization with a field path.

CartPole has one actuator and supplies a minimal balance and training test.
PhantomX has 18 actuators and a floating base.
Mechanisms outside the supported topology or operator contracts require explicit runtime design and tests.
The existing integration path does not promise arbitrary Robot support.

<a id="architecture-fixed-physics-steps-and-variable-control-intervals"></a>
### Fixed physics steps and variable control intervals

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

<a id="architecture-parallel-slots-and-collision-isolation"></a>
### Parallel Slots and collision isolation

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

<a id="architecture-actuation-and-observations"></a>
### Actuation and observations

Actuators declare physical stiffness, damping, effort limit, target, position, and velocity.
The explicit actuator path calculates and clamps effort.
Revolute position actuators use the Chaos constraint drive.
The adapter compensates UE's angular stiffness and damping scale factors so the
simulator receives the declared SI gains. It does not change global engine settings.
The torque limit is converted from N·m to UE units without gain scaling.
Keep these engine scale factors positive and constant during a Robot's lifetime.

A constraint drive and an explicit PD calculation can have different discrete-time responses.
Body or joint inertia conditioning and other solver settings can change the physical response.
The Robot runtime disables joint position projection in training and deployment.
Projection can change pose without matching physical velocity and can change total momentum.
A claimed component regains its original projection settings when released.
After writing all reset body states, the runtime clears the older deferred skeletal
teleport. The next physics step must start from the completed reset pose.
Check measured trajectories and loads with the physical test fixtures.
A calculated PD effort is not measured solver torque.
The current actuator is an idealized simulation drive with an effort cap.
It does not model servo voltage, temperature, a torque-speed curve, or communication delay.
A physical servo's stall torque is not a continuous operating limit.
Keep the asset's declared simulation parameters separate from hardware specifications.
Position, effort, and passive behavior use shared runtime code instead of robot-specific control code.

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

<a id="architecture-protocol-and-reset"></a>
### Protocol and reset

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

<a id="architecture-declarative-mdp-and-cross-language-plans"></a>
### Declarative MDP and cross-language plans

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
See [operator admission](architecture.md#operators).

Manager-composed declarative Tasks supply the exportable Task path.
Export reads both plans directly from the Task.
Interfaces remain where alternative implementations exist, such as command sources.

<a id="architecture-ppo-curriculum-and-physical-time"></a>
### PPO, curriculum, and physical time

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
See the [training guide](training.md#phantomx-robust-training) for complete rules.

<a id="architecture-artifact-and-in-game-inference"></a>
### Artifact and in-game inference

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
See [deployment](deployment.md#in-game-deployment-guide).

<a id="architecture-reproducibility-and-fault-boundaries"></a>
### Reproducibility and fault boundaries

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
See [tests](../tests/README.md#test-suites) and [Write tests](../tests/README.md#write-tests).

<a id="architecture-reference-defaults-and-workflow"></a>
### Reference defaults and workflow

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

Use the [README](../README.md), [configuration guide](training.md#configuration), [training guide](training.md#phantomx-robust-training), [recording guide](training.md#record-video), and [deployment guide](deployment.md#in-game-deployment-guide) for executable procedures. Use a fixed map, start, command sequence, and seed when comparing scene generalization. Compare survival, pursuit success, root height, and non-foot contact individually; the CLI reports metrics but does not automatically compare a baseline report.

<a id="architecture-additional-terminology"></a>
### Additional terminology

| Term | Meaning |
|---|---|
| Robot asset declaration | Python asset reference and semantics, supporting derivation |
| Observation shape table | Shapes, units, frames, and semantics derived from simulation field descriptors |
| Plan | Compiled operator graph stored as data and evaluated by Python or C++ |
| Policy artifact | Observation/action plans bound to an exported network and runtime contract |
| Scene randomization | Training variation in ground friction and disturbances; terrain tiers are selected at reset |
| Authored-map baseline | Repeatable evaluation on a fixed map/start/seed, compared metric by metric |

<a id="context"></a>

<a id="context-embodiedue-context"></a>
## EmbodiedUE Context

This glossary defines Sessions, Environments, Robots, assets, semantics, and the boundary between training and simulation.
Use these terms consistently.
Implementation details belong in the architecture guide and source code.

<a id="context-robot-domain"></a>
### Robot domain

**Robot**:
A simulated entity that a training Task can control, observe, and reset.
_Avoid_: Actor, Model, Agent

**Robot asset**:
The assets defining a robot’s simulated structure, including the entity hierarchy and body/joint relationships.
_Avoid_: robot model, scene object

**Topology**:
The stable body and joint references, parent/child relationships, and structural joint constraints reflected from a Robot asset.
_Avoid_: semantics, configuration

**Robot semantics**:
Declarations specifying which structures participate in control, observation, and reset, and how those operations are interpreted.
_Avoid_: topology, asset metadata

**Robot Interface**:
The contract for exchanging robot structure, actions, observations, and reset information between training and simulation.
_Avoid_: CartPole-specific protocol

<a id="context-configuration-and-runtime-descriptions"></a>
### Configuration and runtime descriptions

**RobotConfig**:
Robot semantics configuration, including actuators, observation selection, and reset distributions.
_Avoid_: asset description

**RobotSpec**:
The executable robot description obtained by merging asset Topology with RobotConfig, with stable action and observation indices.
_Avoid_: raw config, raw topology

**SessionSpec**:
The immutable Session contract that Python compiles at initialization.
It includes the Robot, Environment, Slots, physics_dt, inclusive decimation range, and wire projection.
The [min,max] endpoints are positive int32 values.
The form [N,N] specifies fixed decimation.
_Avoid_: mutable runtime state, training source config

**Execution plan**:
The immutable plan for the simulation execution path, compiled from SessionSpec at Initialize commit.
It caches body, constraint, column, and unit-conversion indices.
_Avoid_: plugin framework, user configuration

**Canonical default state**:
The complete default physical state captured for each Slot at Initialize.
Reset restores this state before it applies the requested overrides.
_Avoid_: current state, partial reset fallback

**Initial episode state**:
The Post-Reset State from a normal reset of every Slot after Session Ready.
It uses the same reset distribution as later episodes.
It is the first policy-visible state.
The canonical initialization state establishes the runtime and validates the protocol.
It does not start the first episode.
_Avoid_: asset spawn pose, startup grace period

**Actuator**:
A robot component that converts an action dimension into a physical effect on a controllable joint.
_Avoid_: action, controller

**Action target**:
The numeric target received by an Actuator at the current control instant. It carries no joint name.
_Avoid_: policy output

**Observation**:
A value read by the training Task from current robot physics, with explicit type, units, and ordering.
_Avoid_: sensor plugin

**Terrain-height scan**:
A fixed 7×5 terrain-height primitive with a world-horizontal footprint relative to the Robot root.
Each sample is (hit.Z-root.Z)/100 in meters.
Robot Runtime calculates it during observation collection.
It does not copy terrain configuration or expose UE world coordinates.

Shared World training restricts ray hits to the Environment terrain-owner whitelist.
Slot-isolated training uses the Slot collision query channel.
Deployment queries blocking WorldStatic geometry.
Successful hits cache world-space hit points.
During a brief query loss, the runtime calculates new root-relative values from those points.
Missing initial or reset hits cause a diagnostic error.
_Avoid_: terrain seed, heightfield mesh, world-space elevation

**Reset distribution**:
Rules for sampling a robot’s initial state on each reset.
_Avoid_: terrain curriculum

**Contact observation**:
Binary geometric support (0/1) for a selected body at the last completed solver step of a control window.
The runtime queries it at the window end.
It does not accumulate earlier support or OnComponentHit events.
Slot-isolated contact counts only the owning Environment.
Shared World contact counts WorldStatic ground.
The terrain-scan owner whitelist does not restrict contact.
_Avoid_: contact force, collision manifold

**Contact-force observation**:
The net impulse magnitude from the same final completed solver step, converted to newtons as |I| / (100 * SolverStepSeconds).
It is not a window maximum.
The divisor is not game DeltaTime, the complete control window, or clamped observation dt.
Without a new physics result, the runtime does not reuse old impulses.
_Avoid_: window force maximum, control-frame impulse

<a id="context-runtime-boundaries"></a>
### Runtime boundaries

**Slot**:
One robot instance and its task state within a parallel simulation.
_Avoid_: world, environment process

**Session**:
An immutable training connection served by one Worker bridge in one UWorld.
Slot count, execution plans, physics_dt, and decimation range are fixed.
Each Step carries one step_decimation scalar within that range.
_Avoid_: episode, task

**Environment**:
The simulation context that supplies placement, ground, and terrain to Slots.
It does not own Robot semantics, robot control, or Task mathematics.
_Avoid_: robot, task

**Collision scope**:
The Environment declaration of whether collision geometry belongs to individual Slots or is shared at Session scope.
_Avoid_: collision mode, spacing policy

**Slot-isolated Environment**:
An Environment where each Slot owns collision geometry that interacts only with its own Robot.
_Avoid_: cloned world, private scene

**Shared World Environment**:
An Environment where several Slot Robots share terrain geometry and keep robot-to-robot Slot isolation.
Geometry can come from a loaded World Map or procedural generation in that World.
_Avoid_: Shared Map Environment, global environment, multi-agent environment

**Terrain source**:
The source of Environment geometry and Ground frames: procedural terrain or an authored World Map.
A change to the source does not change collision scope.
_Avoid_: collision mode, map mode

**Terrain curriculum**:
The Session generates all difficulty regions during initialization.
At episode reset, training changes difficulty from accumulated command-direction progress and commanded distance.
Normal promotion or demotion moves one level.
Promotion above the highest level causes random selection across all levels.
Terminations and timeouts use the same rule.
Geometry does not change or regenerate during walking.
_Avoid_: Reset distribution, runtime terrain mutation

**World Map identity**:
The long UE package name of the World loaded for a Session.
The resolved configuration, Worker projection, and Run manifest jointly establish this identity.
_Avoid_: launch argument, level filename

**Ground frame**:
A Slot-local ground reference frame used to express root/body poses and velocities.
_Avoid_: world transform, terrain random seed

**Slot isolation**:
The invariant that robot collisions and contacts in one Slot cannot affect another.
Slot-isolated Environments also isolate terrain and queries.
Distance between Slots alone does not establish isolation.
_Avoid_: spacing heuristic

**Task**:
The training-side definition of actions, observations, rewards, termination, and episode rules.
_Avoid_: Worker, Robot provider

**Worker**:
The simulation-side runtime coordinating Slot lifecycle, initialization, fixed stepping, state collection, and reset.
_Avoid_: trainer, task

**Control frame**:
A complete control cycle.
Training submits an action target and step_decimation.
Simulation holds that target for the specified physics steps and returns one observation.
The window is physics_dt × step_decimation.
ArtifactTiming stores physics_dt and the decimation range and derives the minimum and maximum control intervals.
_Avoid_: physics substep

**Control frame length**:
The actual duration of frame k is dt_k = physics_dt × d_k.
Here, d_k is that Step’s step_decimation.
The sequence is o_k → a_k → physics(d_k) → o_(k+1).
The returned observation interval belongs to the completed frame.
The first observation after Initialize or Reset uses DtMin by convention.
Game deployment consumes this reset input before its first physics window; completed-window inputs follow physics.
_Avoid_: nominal control period, wall-clock latency

**Slot fault**:
A runtime fault affecting only one Slot’s physical state, for which an explicit sparse reset may attempt recovery.
_Avoid_: terminated, truncated

**Session-fatal fault**:
A fault in protocol, assets, execution plans, fixed-frame relationships, or Slot isolation that requires stopping the entire Session.
_Avoid_: automatic retry, task termination

**Training side**:
The side owning Robot semantics, action mapping and resolution, and reset distributions.
_Avoid_: physics executor

**Simulation side**:
The side reflecting Topology from assets, executing numeric actions, collecting selected observations, and applying reset values.
_Avoid_: robot owner

<a id="context-configuration-ownership-and-training-host"></a>
### Configuration ownership and training host

**Training configuration**:
The Task, Worker, Environment, runner parameters, and Robot asset/semantics references for a training Run.
It does not duplicate the physical facts in Robot assets.
_Avoid_: robot definition

**Robot semantics configuration**:
The training use of a Robot: actuators, gains, reference pose, action scaling, observations, and reset distributions.
It excludes mass, inertia, geometry, and joint limits.
_Avoid_: asset metadata

**Worker projection**:
The training-side projection of a resolved RobotSpec into simulation runtime configuration.
It is an internal artifact, not a second Robot configuration maintained by the user.
_Avoid_: source configuration

**Training UE host**:
The dedicated UE project that contains the Worker and project-level Chaos baseline.
The host owns global physics configuration.
The plugin reads and validates it.
_Avoid_: plugin installer

**Chaos baseline**:
Global fixed-step-related physics settings that must be active before the training host creates its physics scene.
_Avoid_: per-robot semantics

**Deployment physics gate**:
A read-only pre-start check of the actual World, Chaos solver, synchronous substep settings, and artifact timing.
It requires valid synchronous substeps.
It does not reuse the training Worker’s substeps-disabled gate or change host settings.
_Avoid_: training physics gate, configured time as completed time

**Completed solver clock**:
Chaos GetSolverTime, current frame, and GetLastDt from a safe completion point in the same World.
Only actual frame/time advancement produces a new completed sample.
Pause or the absence of a solve does not create progress.
_Avoid_: game DeltaTime, configured physics window

<a id="operators"></a>

<a id="operators-operator-admission-uerlpolicy"></a>
## Operator admission (UERLPolicy)

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

<a id="operators-quaternion-convention-xyzw"></a>
### Quaternion convention (xyzw)

All plan-layer quaternions use **xyzw**:

- Robot `body_pose` units are `m,quat_xyzw` (position then quat).
- Python `rotate_inverse` / `projected_gravity` read `quat[:, 0:3]` as xyz and
  `quat[:, 3]` as w.
- C++ constructs `FQuat(Q[0], Q[1], Q[2], Q[3])`.
  Unreal's `(X,Y,Z,W)` matches the plan's xyzw order.
  Do not interpret plan storage as wxyz.

The case `rotate_inverse/asymmetric_rotation.json` detects an xyzw/wxyz swap.
Identity and axis-aligned rotations can hide that defect.

<a id="operators-built-in-source-ops"></a>
### Built-in source ops

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

<a id="operators-built-in-transform-ops-body-frame"></a>
### Built-in transform ops (body frame)

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

<a id="operators-nan-passthrough-shaping-ops"></a>
#### NaN passthrough (shaping ops)

The operators `scale`, `offset`, and `clip` pass NaN inputs through unchanged.
They do not treat NaN as a soft error.
Upper-layer finiteness checks must terminate execution after physical divergence.
The post-control-step state is one such check location.
There is no dedicated NaN parity corpus.
