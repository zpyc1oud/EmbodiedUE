# Validate a static-ground policy deployment

Use this procedure to separate policy computation, actuator/physics response,
and closed-loop behavior when evaluating a saved Run in a game scene. It covers
the supported static-ground path; it does not establish behavior on moving
platforms or with dynamic obstacles.

## Prepare a reproducible target

Choose a target map with a fixed start area and ground that blocks a downward
`WorldStatic` query. Record the map package, its source revision, Robot mesh and
PhysicsAsset, policy asset, spawn transform, seed, and command sequence. The
repository's `/Game/Maps/UERLPolicyDemo` is a candidate authored map; inspect it
in the Editor after retrieving LFS content before using it as the target.

From the checkout root in PowerShell:

```powershell
git lfs install
git lfs pull
uv sync --locked

$ueRoot = $env:UE_ROOT
if (-not $ueRoot) { throw 'Set UE_ROOT to the UE 5.8 installation root.' }
$ueCmd = "$ueRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
$project = (Resolve-Path 'engine/UERLHost.uproject').Path
$runDir = (Resolve-Path 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>').Path
$map = '/Game/Maps/UERLPolicyDemo'
& "$ueRoot\Engine\Build\BatchFiles\Build.bat" UERLHostEditor Win64 Development $project -WaitMutex
```

Set `UE_ROOT` to the local UE 5.8 install directory and replace
`<run-directory>` with the saved Run folder selected for evaluation.

LFS pointers are not loadable map, mesh, PhysicsAsset, or policy assets. The
Egypt Fab scene is optional external content and is not required for this
static-ground procedure.

Run a read-only deployment preflight against the selected Run:

```powershell
uv run uerl deploy --project $project --check `
  --task UERL-PhantomX-ContinuousTerrain-v0 --artifact $runDir
```

For an external game project, follow [in-game deployment](../in-game-deployment-guide.md)
to install the plugin and import the same artifact. Build the target Editor,
run `UERL.CheckProject`, and ensure the imported policy asset and its RobotMesh
are referenced by the map's policy actor so they are included in cook.

## Phase 1: compare computation with fixed inputs

Run the independent Python expected-value corpus:

```powershell
uv run pytest tests/parity/test_deploy_parity.py tests/parity/test_deploy_variable_dt.py -q
```

It checks reviewed observation, action, and actuator-target values. The
variable-dt fixture includes history reset, explicit previous action, the
5–35 ms interval endpoints, and an off-grid interval. These numerical
tolerances apply to those fixtures only; they do not set trajectory or learning
acceptance.

On the UE 5.8 host, run the corresponding C++ expected-value tests:

```powershell
& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Unit.Policy.DeployParity+UERL.Unit.Policy.DeployVariableDtParity;Quit' `
  -unattended -nullrhi -nosound -NoSplash
```

Expect both Automation cases to complete successfully and the log to contain
`**** TEST COMPLETE. EXIT CODE: 0 ****`. A passing Python-only run does not
replace the C++ result.

## Phase 2: compare fixed-action physical response

The native `AC_UE_E2E_ROBOT_CONTENT_005.DeployedEffortActuatorMatchesTrainingPerSolverStep`
case applies the same 20 N CartPole effort for nine 5 ms solver steps in the
training lockstep and deployment substep paths, then compares joint position
and velocity. Run it with:

```powershell
& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_005;Quit' `
  -unattended -nullrhi -nosound -NoSplash
```

This is a fixed-action physics regression in a transient Chaos scene. It
isolates the training/game actuator path; it is not a result for a target map
or a learned policy.

The policy controller also has a five-step static-ground smoke case. It checks
that the artifact initializes and the closed loop produces finite state; it
does not assert chase quality or no-fall behavior:

```powershell
& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Integration.Policy.Controller.AC_UE_INT_POLICY_003;Quit' `
  -unattended -nullrhi -nosound -NoSplash
```

## Phase 3: evaluate a held-out static target scene

Use the same saved Run, seed, spawn transform, and command sequence on every
repeat. For a fixed-command diagnostic, pin the PhantomX sampler to a constant
forward command and record the Task-side trace:

```powershell
$seed = 0
$taskTrace = Join-Path $runDir 'traces/task.yaml'
uv run uerl play --project $project --ue-executable $ueCmd --run $runDir --map $map --seed $seed --steps 4000 --terrain-level 0 --presentation none --trace $taskTrace --controller fixed --fixed-velocity '0.45,0,0'
```

The fixed controller keeps `velocity` at `[0.45, 0, 0]` across episode resets. In the
Editor, open that same map, place the imported policy actor on the recorded
start, set its `Artifact` to `/UERLEngine/Policies/PhantomXContinuousTerrain116`,
and set `bAutoStart=false`. Add a `UERLPolicyTraceRecorder` component to the
same host (or set its `PolicyComponent` reference). In the host BeginPlay path,
call `SetCommand(velocity, [0.45, 0, 0])`, then
`StartTrace("target-scene-ue.yaml", 0)`, then `StartPolicy`. Check the boolean
return from each call and `GetLastError` on failure. StartTrace writes to
`engine/Saved/UERLPolicyTraces/target-scene-ue.yaml` and refuses to overwrite an
existing file. Stop the trace explicitly after the final sample.

`--trace` also exports the loaded policy to a temporary UERLPOL2 artifact and
records the SHA-1 of its ONNX payload. The UE recorder hashes the imported
artifact's ONNX bytes the same way. The comparator requires these fingerprints
to match before it reports a frame pair as comparable. The temporary artifact
is removed after the fingerprint is recorded.

For every host reset/restart, first call `ResetToReferencePose` or `SoftReset`,
then call `MarkEpisodeBoundary` with the episode index from the Task trace's
post-reset row and a reason. The call must occur before the next policy
bootstrap frame. This marks the new phase and resets the trace's episode clock.
The trace recorder defers applying that boundary until it receives a bootstrap
frame whose sequence is greater than the component sequence at the reset call.
This keeps the callback frame in the old episode whether the host callback runs
before or after the recorder callback. A boundary record uses the sequence of
the first new-episode bootstrap frame.
The first Task input is `post_reset_input`; UE's first-ever control callback is
`bootstrap_input`, so the comparator will mark that initial pair not comparable.
Later Task/UE rows pair only when episode index, policy step, command, timing,
and reset boundaries agree. A reset schedule that differs between the two runs
will be reported as a mismatch or a missing row.

The PhantomX host channel is `velocity`, ordered as body-frame forward speed
(m/s), lateral speed (m/s), and yaw rate (rad/s). The component checks channel
width and finite values; it does not enforce task-specific units or limits.

Bind `OnControlStepCompleted` to the host's run recorder. Each event is one
**policy-decision sample**: the initial/reset input is consumed in PrePhysics,
with zero elapsed physics and no completed solver-step dt. Subsequent samples
are emitted at the safe post-physics boundary. The raw
state, observation, previous action, and command values are the inputs consumed
to produce that frame's action; actuator targets are that action's outputs.
The event is broadcast after inference and before the next physics window runs.
Its raw state is therefore not the successor of the action in the same event.
The successor is the next event's raw state if the host did not reset or
otherwise move the Robot between events. A restart/reset must be recorded as a
separate boundary, and the first sample after it starts a new comparison phase.

The event carries a component-local sequence number, solver frame/time, elapsed
game/physics time for the preceding window, observation dt used by this input,
last solver-step dt, named raw-state fields and widths, exact deployment-plan
observation, previous action input, raw policy action, actuator targets, and
named command values with ages. The event is opt-in: the component builds and
copies snapshot arrays only while a listener is bound. The component does not
persist these events. Start and stop the host recorder explicitly.

Trace capture requires an exportable Task because it records the exported
policy identity and deployment-plan fields. Known unsupported Tasks fail this
preflight before Worker startup; ordinary playback remains available.

For a paired record, `uerl play --trace <trace.yaml>` stores the Task-side
decision sample and the result of its Worker step in the selected Run's trace
file. The Task `input` record is captured before the Worker step and contains
the raw state and observation supplied to the inference policy, previous-action
history, and current Task command values. The `transition` record is the
Worker's post-step `TransitionState` **before Reset**, with the actual
`physics_dt`, sampled `step_decimation`, `transition_dt`, reward, done flags,
validity, and fault. When done, the `reset` record separately captures the
post-reset state and the observation returned for the next episode. It must not
be treated as the terminal transition or paired with that transition's action.
`field_layout.deployment_state_fields` records the state names selected by the
bound Task observation plan. The Task trace retains every raw state field,
including Task-only diagnostics; comparisons require and measure the plan fields
only. UE's recorded raw-state field set must exactly match those deployment
requirements, with matching widths. Task-only fields are listed as diagnostic
fields on each pair and do not block comparison. A required field missing on
either side, or a non-finite required value, makes the pair not comparable.
Trace capture is off by default and allocates no per-step copies when disabled.
The `UERLPolicyTraceRecorder` also records control overruns, stale command
channels, policy faults, and explicit episode-boundary records while active.
UE trace files are written as UTF-8 without a BOM, so host-provided Unicode
reset reasons round-trip through the Python reader. Artifacts with no command
channels write `commands: []` as an empty YAML sequence.

The UE host trace uses the same YAML field names for decision inputs and action
outputs; it also records the artifact Task/Robot identity, map package, supplied
seed (or `null` when the host has none), solver/game clocks, and raw-state field
layout. The Task trace records its Run/checkpoint identity, Task, map, seed,
physics timing configuration, and observed field layout. Keep both trace files
with the Run and host evidence.

Pair rows by episode and policy-step only as candidate decision samples. Before
comparing values, check the Task/Robot and map identities, seed, command values,
the exact deployment-required raw-state fields and observation layouts, and dt
**with the phase stated**. Task input
observation dt corresponds to the dt used by the UE event's observation plan;
Task action `transition_dt` corresponds to the following UE event's elapsed
physics window, not the current event's elapsed window. Game and solver clocks
may differ. Report the first command, layout, phase, or dt mismatch with both
actual values and mark affected rows not comparable; do not interpolate or
silently shift them. A Task transition state can be compared with the next UE
decision input only when no reset/fault boundary intervened and the elapsed
window aligns. A numerical difference in a closed-loop state/action/target is a
diagnostic, not a parity failure threshold. Existing `2e-5` tolerances apply
only to their fixed-input numerical fixtures.

Overrun, stale-command, and policy-fault events carry the episode/step and
sequence they affect. UE policy faults and nonzero Task Worker fault codes make
the comparison `faulted`; `EndPlay` without an explicit successful `StopTrace`
writes `status: incomplete`. The comparator includes fault events in its
report and exits nonzero for a faulted trace; it rejects incomplete traces. A
failed or faulted rollout therefore cannot be presented as a completed
comparison.

Run the snapshot alignment regression on the UE host:

AC_019 replays the artifact observation plan from each captured frame's raw
state, command values, previous action, and control dt, then re-evaluates the
artifact network from that observation and compares the results with the
recorded observation and action. AC_020 calls `StopPolicy` and `SoftReset`
inside the public frame callback and checks the restart/bootstrap boundary.

```powershell
& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_019;Quit' `
  -unattended -nullrhi -nosound -NoSplash

& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_020;Quit' `
  -unattended -nullrhi -nosound -NoSplash

& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_021;Quit' `
  -unattended -nullrhi -nosound -NoSplash

& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_022;Quit' `
  -unattended -nullrhi -nosound -NoSplash
```

After both traces are saved, run the pair report. A `comparable` status means
at least one pair passed task/robot/map/seed/model identity, field-layout,
phase, command, and clock checks; it is not a learned-behavior verdict. A
`not_comparable` report prints the actual mismatched values and exits with
status 2. A `faulted` report retains per-frame fault events and also exits 2;
`incomplete` traces are rejected. Numerical differences remain diagnostics
without a rollout-quality threshold.

```powershell
$ueTrace = Join-Path (Split-Path $project) 'Saved/UERLPolicyTraces/target-scene-ue.yaml'
uv run python scripts/compare_policy_traces.py $taskTrace $ueTrace
```

Use `GetRequiredCommandChannels` to obtain the channel names and widths rather
than assuming that another artifact uses PhantomX's `velocity` channel. With
`bClaimOwnerMesh=false`, the component owner's transform selects the spawn
placement and the measured ground clearance sets the initial root height. With
`bClaimOwnerMesh=true`, the component claims the matching authored SkeletalMesh
and detaches it for physics control. `GetRobotTransform` reports the live
controlled mesh transform; the Owner transform can remain at its authored
location while the mesh moves.

Deployment terrain scans accept blocking `WorldStatic` hits and ignore the
controlled Robot actor. Start-clearance and pose-reset traces ignore the
policy component's Owner actor. Other blocking `WorldStatic` geometry along a
downward probe can be selected, so ensure ceilings, platforms, and Owner
collision shells do not intersect the target map's probe paths. Training's
shared World query uses the Environment terrain-owner whitelist;
Slot-isolated training uses its terrain query channel. `StartPolicy` measures
current ground clearance; `ResetToReferencePose` uses current XY/yaw and traces
current ground again. `SoftReset` clears policy history and observation
caches while retaining pose.

The `AC_UE_INT_COMPONENT_017.SpawnedPoseResetIgnoresHostWorldStaticCollision`
Automation case checks that a spawned Robot reset selects ground below an
Owner collision shell:

```powershell
& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_017;Quit' `
  -unattended -nullrhi -nosound -NoSplash
```

Repeat the Task-side playback and the in-game policy run without changing the
map, artifact, seed, start, or command schedule. Keep separate evidence for:

| Phase | Record | Interpretation |
|---|---|---|
| Computation | command values, named raw state, network observation, previous action, raw action, actuator targets, `control_frame_dt`, last solver dt, and solver frame | Plan/network mismatch before physics |
| Fixed action | the action sequence and resulting joint/root state at matching completed solver times | Actuator, unit, or solver response mismatch |
| Closed loop | command sequence, reset/start pose, completed solver times, fall/base-contact events, speed error, and root-height trace | Accumulated policy/scene behavior difference |

Include the exact Run, map package/revision, artifact, seed, software versions,
and raw results with each comparison. Report throughput separately from
learning quality and behavior. The repository has no frozen held-out-scene
behavior threshold yet; select it only after pilot results and record the
rationale before release measurements. Do not infer learning quality from a
short smoke run or from numerical parity.

## Respond to timing and policy faults

`OnControlStepOverrun` reports game, physics, and observation seconds. Inspect
all three values. `OnCommandStale` reports one command channel and its age; the
latched value otherwise remains in use. Neither event changes the host command
or automatically pauses physics. The host must choose and test its response
for the target game. Component regressions exercise a host `StopPolicy`
fallback for overrun and stale-command callbacks, followed by explicit command
re-latching and restart; a game may choose another response.

`OnPolicyFault` stops policy inference and component ticks. The host must
choose a fallback before relying on a stopped controller. For a
component-owned spawned Robot, a host may bind `OnPolicyFault` to `StopPolicy`;
that releases and destroys the spawned Robot while the rest of the World
continues. For a claimed authored mesh, `StopPolicy` restores its captured
attachment and physics settings, which may include restoring a non-simulating
state. It is not a universal physical emergency stop.

After correcting the cause, set every required command channel again and call
`StartPolicy`. The
`AC_UE_INT_COMPONENT_016.HostFaultFallbackStopsOwnedRobotAndRestarts` Automation
case exercises a missing-ground fault, this spawned-Robot fallback, continued
unrelated Chaos motion, and restart after the ground and commands are restored:

```powershell
& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_016;Quit' `
  -unattended -nullrhi -nosound -NoSplash
```

The adjacent cases cover overrun clock reports, stale-channel rearming,
stop/reset from an overrun callback, and reset failure reporting. The stale
fallback case verifies that the host can stop, re-latch commands, and restart:

```powershell
& $ueCmd $project '/Engine/Maps/Entry' `
  '-ExecCmds=Automation RunTests UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_009+UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_010+UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_011+UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_012+UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_018;Quit' `
  -unattended -nullrhi -nosound -NoSplash
```

The case uses a real UE preview physics scene and bundled PhantomX assets. It
does not validate the target map or establish a physical safety guarantee for
another host's fallback.

## Evidence boundary

The UE Automation cases above exercise native plans, reset/query behavior,
fixed-action response, and component fault handling. Passing them does not
establish a held-out target-scene result. Record a target-scene run separately;
if the map, Robot assets, or Windows UE host is unavailable, mark that result
blocked and keep the behavioral release gate open. This procedure covers
static ground only.
