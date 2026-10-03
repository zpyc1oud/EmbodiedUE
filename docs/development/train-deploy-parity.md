# Training and deployment compatibility plan

Status: proposed development work, based on `42fae6dd404b8a5a74110113cdde251c7ec069b5`.

The objective is reliable policy behavior in the target game. Training and deployment can use different maps and scheduling mechanisms while preserving the policy's input, action, and timing semantics. Sharing Chaos removes one backend difference; it does not prove closed-loop equivalence.

This plan extends DEV-08 and DEV-16 to DEV-21 in the [roadmap](roadmap.md). Existing behavior is specified in [CONTEXT](../../CONTEXT.md), [architecture](../architecture.md), and the [deployment guide](../in-game-deployment-guide.md).

## Existing behavior to preserve

- Training uses request-driven fixed stepping. One Step holds a batched action for its requested decimation.
- The training host baseline disables substepping. Game deployment uses synchronous physics with substeps and checks the live solver before starting.
- Deployment advances control using the completed Chaos solver clock, not game-frame duration.
- An artifact binds observation/action plans to ONNX and Robot Runtime/timing metadata. It carries the trained plans rather than reconstructing them from current Task defaults.
- PhantomX uses a 5 ms physics step and control intervals of 5-35 ms. The first observation after Initialize or Reset uses `DtMin` by convention.
- Contact and contact force represent the last completed solver step of a control window, not an aggregate over the whole window.
- Terrain scans use an Environment-owner whitelist or Slot channel during training. Deployment queries blocking WorldStatic geometry and ignores the owning Actor.
- `StopPolicy` stops inference while physics continues. Stale-command diagnostics do not automatically change the latched action or command.

These are intentional contracts or concrete differences to evaluate. Do not copy the training host's substepping setting into gameplay to make the configuration files look identical.

## Separate three kinds of difference

### Fixed policy semantics

Keep observation order, dimensions, units, frames, quaternion convention, normalization, clipping, action mapping, actuator selection, history updates, and command channels consistent.

Compare canonical policy inputs, not raw engine storage. An adapter may convert units or frames, but its output must match the artifact's definition. Joint names and indices must resolve before inference.

Use the saved artifact plans and network. A changed Task default is not a deployment update for an existing policy.

### Declared compatible operating conditions

Check physical timestep, supported control interval, Topology, actuator behavior, sensor freshness, and sampling conventions. Make an actual supported conversion explicit. Reject incompatible startup conditions through the existing physics/binding checks.

Extend existing metadata only where a field drives a real compatibility decision. Record the asset revision and actual relevant physical properties when needed. A path or `robot_id` alone does not prove identical asset contents. Avoid a second identity system or new checksums that no consumer uses.

### Environment variation learned during training

Cover the intended terrain, friction, commands, reset distribution, loads, and disturbances. Select ranges from the target game and freeze them for an experiment. Keep validation and final test maps separate from reward tuning.

Randomization addresses variation within a correct interface. It does not repair wrong units, reversed joints, stale observations, or a different action interpretation.

## Minimal Task entry point

A default minimal template should reuse deployable observation/action components and expose Task mathematics through the existing `DirectTask` contract. The advanced Manager entry point must use the same environment, Session, valid-Slot masks, and reset core.

A user can implement custom Python observations or actions for research. Initial checks must state whether that Task can also export. The entry style does not decide capability: using supported plans does. Do not add a second runtime or promise automatic translation of arbitrary Python.

See DEV-08 in the [roadmap](roadmap.md) for the external-template acceptance conditions.

## Clock and action alignment

Record enough timestamps to distinguish:

1. Game/render time.
2. Completed solver frame, elapsed solver time, and last solver step.
3. Observation sample time and age.
4. Policy invocation and completion.
5. Action application and hold duration.

Extend existing profiling and diagnostic events. High-detail traces should be opt-in and their overhead measured separately.

Use completed physics advancement to schedule control. Do not invoke the policy repeatedly on one old observation to simulate missed control steps. Very short windows should retain the existing accumulation semantics.

Test stable intervals, jitter, pause/resume, and controlled stalls. A deployment interval inside the artifact range still needs behavioral evaluation; the range alone does not establish competence for every sequence. Clipping the time input cannot undo a longer physical interval.

For overruns or faults, the host must choose an explicit response, such as a tested controlled pose, pausing/removing the Robot, or a reset. Stopping inference alone is not a guarantee of a benign physical state. The response must specify whether old commands remain active, what state is cleared, and how restart is authorized by gameplay.

## Observation and actuator alignment

For a fixed input fixture, compare observation-plan output, normalized network input, network output, action-plan output, and physical command fields. Include history-dependent cases across multiple steps and after reset.

For actuators, check joint mapping, sign, SI units, reference pose, gains, effort limit, clipping, and target reapplication. Run a single-joint test before a complex locomotion test.

Window-end contact cannot recover a collision that ended earlier in the window. If a Task needs peak force, integrated impulse, or contact duration, introduce a separately defined field with matching implementations and tests. Retrain or explicitly assess the affected policy; do not silently change the meaning of an existing observation.

Expose terrain-scan age, cache use, and query failures in diagnostics. Retain existing initialization/reset errors and bounded cache behavior. The diagnostics should distinguish a fresh hit from a re-expressed cached hit.

## Authored-map contract

Extend the existing Environment/Provider boundary with a small, explicit description of:

- Spawn and reset regions.
- Objects eligible for ground queries and objects to ignore.
- Collision and query filtering.
- Scan footprint and supported ground behavior.
- Mapping from game commands to the artifact's units, frames, and ranges.

Build a diagnostic map with flat ground, a slope, a step, an overhead roof, and an object that should not count as ground. At known Robot poses, compare hit objects, root-relative heights, clearance, and Ground-frame values in training and deployment.

Training Slot isolation and game-world Robot interaction are different requirements. If the target game allows Robots to collide or be pushed by moving obstacles, include that behavior in the Task and validation, or exclude it from the initial support claim.

Moving platforms, dynamic ground, and new command channels need their own observation/timing design. A static-ground demo does not establish those capabilities.

Use calibration scenes to fix system differences. Use training maps to learn the Task. Use held-out maps to measure generalization. Record the map revision and collision settings with each result.

## Four-layer diagnostic procedure

### Layer 1: fixed inputs and deterministic computations

Use identical raw state, command, previous action, and completed interval. Compare Python and C++ intermediate values and final commands. Extend existing independent expected-value fixtures in [parity tests](../../tests/parity/).

Use each fixture's documented tolerances. Existing deployment inference cases use `atol=rtol=2e-5`; this is not a tolerance for full physical trajectories. New models/operators need reviewed appropriate criteria.

A failure here blocks physical comparisons until the computation mismatch is understood.

### Layer 2: fixed actions and physical response

Bypass the policy. Use equivalent initial conditions, the same asset and ground, and the same action time series through the training and game execution paths. Compare joints, root motion, contact, and completed solver time.

Start with single-joint and static-support cases, then short sequences. Report deviation over time. Do not require bit-identical long trajectories or use a single final pose as proof.

A failure here directs investigation to actuation, physics setup, state sampling, reset, or scheduling.

### Layer 3: same-scene closed-loop behavior

Run the same policy and command sequence on flat ground, then known slopes and steps. Use equivalent initial conditions and map settings. Record history state, actual control intervals, and faults.

If Layers 1 and 2 pass while this layer fails, inspect feedback timing, state history, and accumulated small differences. Change one attributable factor at a time.

### Layer 4: target maps and operating variation

Begin development diagnosis with 10 paired seeds. For release evidence, use independent runs and the evaluation protocol from the [benchmark design](training-benchmarks.md). Freeze behavioral tolerances after pilots and before final measurement.

Exercise render-load targets such as 30, 60, and 120 frames/s, plus controlled stalls. Record achieved render and solver timing; a render target is not a physics rate.

Report survival, tracking error, falls, non-foot contact, action saturation, control overruns, stale commands, and task-specific success/time. Pair corresponding scenarios where possible. Treat independent runs or seeds, not adjacent control steps, as statistical samples.

## Failure response and change control

Use this repair order:

1. Fix interface and implementation errors.
2. Align scheduling, action timing, and query semantics.
3. Calibrate supported physical parameters.
4. Expand the training distribution or retrain.
5. Re-run the affected layers and held-out evaluation.

A map, asset, collision setting, physics baseline, observation, or command change should identify which layers require rerunning. Keep the support statement narrower than the measured results.

## Development items

| Item | Change | Acceptance | Dependency |
|---|---|---|---|
| DEV-16 | Extend existing deployment compatibility data | A real incompatible field is named before startup; existing valid artifact behavior remains | Unified run/config resolution |
| DEV-17 | Add opt-in cross-boundary traces | A fixed fixture can be compared through plans, inference, commands, and time | DEV-16; existing profiling |
| DEV-18 | Automate Layers 1-3 | A deliberately introduced mapping/timing error is detected by the appropriate layer | DEV-17 |
| DEV-19 | Add map query/reset description and diagnostic scene | Training and deployment agree on defined ground inputs at known poses | Existing Environment/Provider |
| DEV-20 | Define host overrun/staleness/fault response | Fault, reset, and restart tests verify actual physical/command behavior | Time contract; existing events |
| DEV-21 | Add held-out map evaluation and tutorial | A supported policy is evaluated and integrated without undocumented steps | DEV-18 to DEV-20 |

## Limitations

This document proposes work and test criteria; it reports no new UE executions. The existing runtime already provides physics gates, completed-clock behavior, plan checks, and diagnostics. Extend those mechanisms. Do not assume an observed configuration difference is itself a defect, or introduce a generic compatibility framework without a supported case.
