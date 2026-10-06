# UE RL Engine Context

This glossary defines Sessions, Environments, Robots, assets, semantics, and the boundary between training and simulation.
Use these terms consistently.
Implementation details belong in the architecture guide and source code.

## Robot domain

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

## Configuration and runtime descriptions

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

## Runtime boundaries

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

## Configuration ownership and training host

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
