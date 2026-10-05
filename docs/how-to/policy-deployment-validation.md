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
repeat. First collect the Task-side evaluation on the target map:

```powershell
uv run uerl play --project $project --ue-executable $ueCmd --run $runDir --map $map --seed 0 --steps 4000 --presentation none
```

Then open that exact map in the Editor, place the imported policy actor on the
same marked start, set its `Artifact`, and bind its command/events before
calling `StartPolicy`. For PhantomX the host command channel is `velocity`,
ordered as body-frame forward speed (m/s), lateral speed (m/s), and yaw rate
(rad/s). The default training command sampler uses 0.4–0.5 m/s linear speed,
clamps yaw rate to ±1 rad/s, and includes zero-speed standing episodes. Keep
comparison commands within the selected Run's training distribution; the
component checks channel width and finite values, not task-specific units or
limits. The Egypt chase example uses `[0.45, 0, yaw_rate]`.

Bind `OnControlStepCompleted` to the host's run recorder. Each event carries a
single post-physics frame with solver frame/time, elapsed game/physics time,
observation dt, last solver-step dt, named raw-state fields and widths, exact
network observation, previous action input, raw policy action, actuator
targets, and named command values with ages. Append these frames to the same
run log as Task-side results so comparisons share a sequence number and solver
boundary; this is opt-in and only copies diagnostic arrays when a listener is
bound.

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
