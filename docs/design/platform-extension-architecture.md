# Platform extension architecture

Status: proposed design for [Issue #52](https://github.com/zpyc1oud/EmbodiedUE/issues/52).
Reviewed repository baseline: `0aeea37f8cc0da0cd71a43d0ea5bb61f6a290841`.
This document specifies code changes before implementation. It does not claim
that the proposed interfaces exist. Public input and artifact changes require
[RFC 0002](../rfcs/0002-shared-input-providers.md) acceptance first.

## Goal and boundaries

Keep one runtime for supported Robot and Task integrations. Let an external
package add a Task and let an input integration serve both training and game
inference. Different maps can supply different geometry while preserving the
input contract used by the policy.

Preserve Session timing, sparse reset, the generic Robot runtime, shared plans,
and the current Direct/Manager entry paths. Do not redesign PPO, Chaos, transport,
or the whole scene system to add a sensor. Application behavior is outside this
design. Follow the [release roadmap](../roadmap/next-release.md).

## Reference and deliberate differences

Pin Isaac Lab at `b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8`.
The following references describe mechanisms, not performance guarantees for UE.

| Isaac Lab source | Mechanism to reuse | EmbodiedUE decision |
|---|---|---|
| [InteractiveSceneCfg](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/isaaclab/scene/interactive_scene_cfg.py) | Compose assets and sensors by name; separate scene creation from Task terms | Keep UE Environment ownership and Robot reflection. Add logical input bindings rather than a second scene owner or USD path system |
| [SceneEntityCfg](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/isaaclab/managers/scene_entity_cfg.py) | Resolve names to ordered indices once | Resolve body/joint/component references during binding, not each observation |
| [SensorBaseCfg](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/isaaclab/sensors/sensor_base_cfg.py) and [SensorBase](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/isaaclab/sensors/sensor_base.py) | Explicit update periods, per-environment reset and cached data | Use the completed solver clock and per-Slot state. Repeated reads cannot advance time or shift sampling phase |
| [RayCasterCfg](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/isaaclab/sensors/ray_caster/ray_caster_cfg.py) | Separate pattern, parent offset, ray alignment and queried geometry | Use UE collision queries and explicit logical ground bindings. Do not copy the pinned implementation's single-static-mesh limitation or assume dynamic scene support |
| [ManagerBasedRLEnv](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/isaaclab/envs/manager_based_rl_env.py) | Separate action, physics, termination/reward, reset, command and observation phases | Retain the repository's tested completed-window order and terminal-state capture; map the concepts without changing the Session clock |
| [DirectRLEnv](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/isaaclab/envs/direct_rl_env.py) | A direct hook-based Task authoring path | Retain Direct and Manager Tasks on the same core. Python-only observation/action functions remain non-exportable unless a native plan path exists |

Isaac Lab configuration selects Python classes inside its simulator. EmbodiedUE
also needs a stable native provider identifier and an artifact contract because
the game runs without the Python process. This is a required deployment adaptation.
The first implementation keeps the existing float32 numeric data path. A general
provider interface does not make image transport or asynchronous rendering ready.

## Current code and confirmed gaps

| Current source | Verified behavior | Consequence |
|---|---|---|
| [Python Robot config](../../src/uerl/core/config/robot.py) | `ObsType` is a closed list; `ObservationConfig` binds a type to a body or joint | Adding a sensor requires changes beyond an external Task declaration |
| [UE field descriptors](../../engine/Plugins/UERLEngine/Source/UERLInterface/Public/UERLInterfaceTypes.h) | Fields already carry shape, float32 type, unit, frame, semantic and source; observation binding uses a closed enum | Reuse these descriptors; add provider identity rather than replace all metadata |
| [Provider registry](../../engine/Plugins/UERLEngine/Source/UERLProvider/Public/UERLRegistry.h) | Environment and Robot factories register by stable IDs | Reuse this registry pattern for input factories |
| [Robot observation plan](../../engine/Plugins/UERLEngine/Source/UERLRobot/Public/UERLRobotObservationPlan.h) | Selected fields compile to topology indices | Keep precompiled bindings and avoid hot-path name parsing |
| [Observation sampler](../../engine/Plugins/UERLEngine/Source/UERLRobot/Private/UERLGenericRobotObservationSampler.cpp) | Ground clearance and terrain scans share a trace helper, but use built-in patterns, query-purpose branches and cached hit fallback | Extract reusable query and sampling components; make their semantics explicit |
| [Slot context](../../engine/Plugins/UERLEngine/Source/UERLProvider/Public/UERLProvider.h) | Training supplies owned terrain/collision context; deployment can query WorldStatic | Preserve isolation while making scene binding a separate input to the same sampler |
| [Game runtime](../../engine/Plugins/UERLEngine/Source/UERLRobot/Private/UERLSkeletalMeshRobotRuntime.cpp) | Deployment reuses the Robot provider, but resolves requests from the fixed observation set | Replace this selection bottleneck with compiled provider requirements |
| [Policy controller](../../engine/Plugins/UERLEngine/Source/UERLPolicy/Private/UERLPolicyController.cpp) | State requirements parse `robot.<joint\|body>.<part>.<type>` selectors | Dispatch through field/provider bindings rather than add one selector case per sensor |
| [Artifact](../../src/uerl/policy/artifact.py) | Format version 1 stores plans, Robot runtime, timing and model; strict metadata keys | A provider contract needs explicit versioned artifact changes in both languages |
| [Capabilities](../../src/uerl/core/direct/capabilities.py) | Train/evaluate/export report supported, unsupported or unknown | Extend capability evidence to provider availability without inventing automatic export |

The present ground query already uses rays. The gap is reusable configuration,
filtering, lifecycle and deployment binding, not the absence of ray casting.
Current training and deployment can use different ray start heights and target
filters. Shared code therefore does not by itself prove identical input meaning.

## Target modules and dependency direction

Keep `UERLInterface` for plain contract types. Put input factory interfaces and
registry beside existing providers in `UERLProvider`. Put the first concrete
robot-state and ray-ground providers in `UERLRobot`, where current access exists.
Add a new module only if a later provider has dependencies that require isolation.

`UERLWorker` and the in-game Robot runtime build the same compiled input set. The
Policy Controller consumes its fields and executes the existing observation plan.
It does not choose ray geometry or inspect a Task name. External native plugins
register factories during module startup and unregister at shutdown. Active
instances must be destroyed before their module unloads.

Python owns typed declarations, Task terms, configuration validation and export
requirements. UE owns native sampling and physical scene bindings. The existing
protocol carries the negotiated numeric fields; no sensor-specific socket or
parallel simulation loop is added.

## Proposed contracts

Names in this section are proposed API names, not existing symbols.

### Input specification

`InputSpec` contains a stable instance name, provider ID, provider contract
version, typed provider parameters, logical attachment target, and logical scene
binding names. An immutable `InputDescriptor` contains output fields and their
existing type/shape/unit/frame/semantic metadata. The provider validates the
parameters and produces the descriptor before buffers are allocated.

Use metres, seconds and radians at the Python/plan boundary. Native providers
perform UE unit conversion once at the boundary. Do not mix body-relative sample
positions with world-relative results without declaring the transform.

The input specification is policy-semantic configuration. Put it in resolved Run
YAML and the artifact requirements. Concrete actor/component references belong
to a `SceneInputBindings` object supplied by the host or game. Do not serialize
UObject pointers, absolute checkout paths, or a training map's actor identities.
Bindings resolve stable logical names to permitted runtime geometry and entities.

### Factory and runtime interfaces

| Proposed operation | Input | Output and responsibility |
|---|---|---|
| Factory `Describe` | Provider identity/version | Capabilities and accepted parameter schema |
| Factory `Validate` | Input specification | Effective specification and output descriptor, or a field-specific diagnostic |
| Factory `Create` | Validated specification | One batch-owned provider instance, with per-Slot state |
| Instance `Bind` | World/Robot read context, Slot contexts, scene bindings, selected fields | Compiled indices, verified query targets and allocated buffers |
| Instance `Sample` | Host-owned completed-window context and selected Slot IDs | Published numeric fields, validity and sample stamp |
| Instance `Reset` | Selected Slot IDs and reset generation | Invalidate only selected caches/history; establish the next bootstrap state |
| Instance `Release` | No policy data | Remove delegates and release only owned resources |

The compiled input set owns instances and field offsets. A field has exactly one
producer. Reject duplicate output names and conflicting descriptors during bind.
Factories may share immutable pattern data; mutable sample/cache state remains
per instance and Slot. Robot physics objects remain owned by the Robot runtime.
Input providers cannot advance simulation, apply actions, or modify unrelated
world objects. They read through the existing host phase or immutable snapshots.

### Sampling and reset

The host is the clock owner. The first implementation supports synchronous numeric
providers sampled at the completed control boundary. Samples carry the completed
solver sequence/time and reset generation in runtime diagnostics. Repeated reads
of the same boundary reuse that sample. Observation history advances once per
policy decision, not once per consumer read.

Do not insert an extra physics step to populate an input. Bind/reset uses the
existing bootstrap phase. A provider that cannot produce its declared initial
sample fails the bootstrap with a diagnostic. On sparse reset, invalidate only
selected Slot caches and clear corresponding policy history at the established
reset boundary. Unreset Slots keep their state and order.

A slower or asynchronous provider needs an explicit scheduling and staleness
contract before admission. It is not silently emulated by returning old values.
The interface can later carry periods and age limits, but the first numeric
implementation must reject unsupported timing modes.

### Missing data and faults

The proposed default for required data is a recoverable sampling fault, propagated
through the existing Slot/runtime fault path. No zero tensor may masquerade as a
valid observation. A provider may support a declared bounded hold-last policy;
it must publish age/validity diagnostics and fail when the bound is exceeded.
A validity mask becomes an actor input only when declared in the trained plan.

Distinguish missing geometry, unavailable plugin, unsupported provider version,
shape mismatch, stale data and reset-generation mismatch. Include provider,
instance, field, Slot and corrective action in the error. Validation must finish
before enabling a policy. On unload, release providers before their World/Robot
references become invalid. Apply the existing safe policy-stop behavior on faults.

## Ground query as the first extraction

Separate three responsibilities:

1. Geometry binding supplies the permitted ground set and Slot isolation rules.
2. Ray query executes declared origins/directions/range against that set.
3. Output conversion produces clearance, hit position or height samples with
   declared units, ordering and missing-hit behavior.

Ground clearance is a single-pattern query. A terrain scan is a multi-ray pattern.
They can share the query backend without collapsing their different output
semantics. Keep geometric contact and solver contact force separate; neither is
a height query or an interchangeable contact-history measurement.

Put attachment transform, alignment, ray pattern, distance and result transform
in provider parameters. Resolve the same parameters in training and deployment.
An Environment binds its owned ground; the game binds the declared logical ground
set. Both use the same sampler. A game-wide WorldStatic selection is an explicit
binding choice, not a fallback that bypasses an intended whitelist.

Use matching analytic geometry to compare both paths: a plane, a translated plane,
a slope and a step. Add an overhead surface and excluded neighbouring geometry
to expose filter/start-height errors. Different real maps need contract-compatible
queries, not numerically identical observations for different geometry.

## End-to-end data flow

Training initialization:

1. Resolve external Task/Robot declarations and typed YAML.
2. Resolve factories, validate input descriptors, and report early capabilities.
3. Create the Environment and Robot; bind logical geometry and topology names.
4. Compile selected input fields and existing observation/action plans.
5. Establish bootstrap/reset state; reject unresolved export/runtime requirements.
6. Save the effective input requirements with the Run.

Training control window:

1. Apply the existing action plan and complete the requested physics steps.
2. Sample selected providers at the existing completed-window boundary.
3. Publish raw fields through the existing batch layout.
4. Compute Task terms, capture terminal data, and reset selected Slots in the
   established DirectEnv order. Build the next policy observation with its command.

Game initialization reads the artifact requirements, validates installed native
providers, resolves scene bindings, and compiles the same input/observation/action
path. Game control consumes completed physics samples without a training Session.
A mismatch rejects loading rather than changing the policy's input shape or meaning.

## Serialization and compatibility decision

Extend selected-state descriptors and artifact input requirements in Python and
C++ together. Reuse existing units/frame fields. Store provider version, effective
parameters, output requirements, sampling/missing-data semantics, and logical
binding requirements. Native code does not execute arbitrary Python callbacks.

The current `.uerlpol2` container has format version 1. The proposal requires a new
format version for mandatory provider metadata. Do not repurpose fields or have
an old runtime ignore new semantics. Preserve original Runs/artifacts; use their
matching runtime for historical replay. No general compatibility framework is
required for this Early-Stage change.

Re-export is valid only when the source Run can supply the exact input contract
and behavior is unchanged. A changed ray pattern, frame, filter, cache policy or
history requires explicit reevaluation and may require fresh training. Do not
invent missing historical parameters. Freeze the exact version/negotiation change
in RFC 0002 before implementation.

## Implementation slices and affected code

| Slice | Main surfaces | Deliverable and exit evidence |
|---|---|---|
| A Contract and schema | Python config/Robot declarations; UE field types; RFC 0002 | Typed specs, independent descriptor fixtures, reject cases, reviewed schema/version decision |
| B Registry and compiled set | `UERLProvider`, Robot binding, Worker lifecycle | Factory registration, immutable bindings, per-Slot reset/ownership tests, no sampling changes yet |
| C Built-in adapters | Robot observation plan and sampler | Existing state fields served through providers; unchanged numeric results on golden fixtures and UE traces |
| D Ground extraction | Ray sampler, Slot scene bindings, deployment runtime | One query contract across training/game, explicit parameters and two-map/analytic tests |
| E Artifact and deployment | `policy/artifact.py`, native artifact reader and Policy Controller | Versioned requirements, exact round trip, load rejection, no closed-selector dependency for new providers |
| F External example and docs | External packages/plugin registration, guides, tests | A separately installed provider example without core dispatch edits; packaged runtime evidence |

Do not mix a sampling-semantic change with a behavior-preserving extraction.
Keep PRs reviewable and record which exact slices an integration batch contains.
The existing PR #50 training experiment does not depend on this proposal and
continues unchanged. Slice E cannot ship alone with an incompatible native reader.

## Verification matrix

- Python unit tests: typed parsing, shape/unit/frame contracts, deterministic
  field order, saved-Run round trips, capability decisions, duplicate producers.
- Native unit tests: provider registration, bind/release failures, resource
  ownership, reset generation, immutable compiled offsets, repeated sample reads.
- Independent mathematics: asymmetric translated/rotated geometry, different Slot
  origins and analytic expected ray heights. Do not compare two calls to the same
  implementation as the only oracle.
- Integration tests: installed external package outside the checkout, plugin
  missing/version mismatch, selected fields, sparse reset with unchanged rows,
  terminal observation ownership and actionable bootstrap failures.
- Real UE tests: selected native suites plus actual training/game paths, ground
  filtering, two scene bindings, lifecycle failure/retry, static-ground scope.
- Artifact/parity tests: write/read in both languages, use the exported contract,
  preserve the established tolerance where applicable, separate physical drift.
- Performance tests: bind cost, steady-state sampling cost, allocation count,
  per-Slot cache memory and packaged inference. Avoid World-wide searches and
  descriptor parsing in the hot path. Set regression budgets before measuring.

Document selected/completed tests, source, host, commands and missing gates under
[the repository workflow](../contribution-workflow.md). Mock-only results cannot
establish native sampling or game deployment.

## Design decisions still requiring review

Approve provider identity/version placement, the artifact/wire schema change,
and the first release's synchronous numeric provider scope. Confirm whether the
existing fault codes carry sufficient detail or need a narrow extension. Select
and document the ground-cache policy using measured existing behavior.

These decisions do not require a whole-platform rewrite. Implementation starts
with the reviewed schema and behavior-preserving extraction, then adds only the
extensions demonstrated by the acceptance cases.
