# Game deployment

[Project home](../README.md)

## Contents

- [In-game policy deployment](#in-game-deployment-guide)
- [Validate a static-ground policy deployment](#policy-deployment-validation)
- [Optional Stylized Egypt demo](#optional-egypt-demo)


<a id="in-game-deployment-guide"></a>

<a id="in-game-deployment-guide-in-game-policy-deployment"></a>
## In-game policy deployment

Use a trained robot controller as part of your Unreal game.
Gameplay code supplies commands; the policy controls the robot through its physics setup.
Evaluate its response in the target map before using it as a gameplay feature.

Deploy a `.uerlpol2` policy on a Skeletal Mesh Actor in UE 5.8.
Inference uses the imported `UUERLPolicyArtifactAsset`, its mesh, and its PhysicsAsset.
Editor import and reimport require the source `.uerlpol2`.
The packaged game does not need that source as a loose file.

After [optional Fab installation and setup](deployment.md#optional-egypt-demo), `Stylized_Egypt_Demo` gameplay spawns PhantomX to pursue the player.
For another game, use `BP_UERLPolicyRobot` or `UUERLPolicyComponent`.
The host GameMode is demonstration code.

<a id="in-game-deployment-guide-before-you-deploy"></a>
### Before you deploy

Prepare a trained Run, matching robot assets, and a target UE project.
First [evaluate the policy](training.md#phantomx-robust-training-per-level-evaluation) against the commands and conditions you intend to use.
Keep a version-controlled backup of the target project because deployment changes its files and settings.

Follow this sequence:

1. Install the runtime in the target project and build its Editor.
2. Import the exported policy and resolve its robot mesh.
3. Set the supported game physics configuration.
4. Place the robot, supply every required command channel, and start the policy.
5. Validate physical response and behavior in the target scene.
6. Cook and test the packaged game.

The sections below give the commands and component details for each step.
Use [deployment validation](deployment.md#policy-deployment-validation) for the acceptance procedure.

<a id="in-game-deployment-guide-prepare-a-target-project"></a>
### Prepare a target project

Run from the repository root in PowerShell:

```powershell
uv run uerl deploy --project '<target-project-directory>' --demo phantomx
```

This command **changes the target project**.
It copies `UERLEngine` and enables `UERLEngine`, `ProceduralMeshComponent`, and `NNERuntimeORT`.
It writes the synchronous physics settings below.
With `--demo phantomx`, it copies the mesh, Skeleton, and PhysicsAsset to `/Game/Robots/PhantomX/`.
Without `--demo`, installation includes only the runtime.

Without a C++ module, deployment creates an empty Game Target with `IMPLEMENT_PRIMARY_GAME_MODULE`.
UAT can then link the plugin into a game built with an installed engine.
Existing plugins, content, and modules normally stay unchanged.
The option `--force` overwrites the plugin and selected demo assets.
Before an update, keep a version-controlled backup of the target.

Robot integration uses Blueprint and Content assets, without robot-specific policy C++.
After module generation, build the target Editor.
Then run `UERL.CheckProject` in its console.

Read-only preflight is available without launching UE:

```powershell
uv run uerl deploy --project '<target-project-directory>' --check
uv run uerl deploy --project '<target-project-directory>' --check --task UERL-PhantomX-ContinuousTerrain-v0 --artifact latest
```

The option `--artifact` accepts a `.uerlpol2` file, Run directory, or `latest` for the selected Task.
For objective comparisons, select an explicit Run.
Before copying assets into a distributed project, examine redistribution rights.
See [release readiness](release-readiness.md).

<a id="in-game-deployment-guide-import-an-artifact"></a>
### Import an artifact

First enable the plugin and build the target Editor.
Deployment supplies artifact, policy asset, and RobotMesh arguments and prints the import command.
The option `--import` executes that command:

```powershell
uv run uerl deploy --project '<target-project-directory>' --task UERL-PhantomX-ContinuousTerrain-v0 --artifact latest
uv run uerl deploy --project '<target-project-directory>' --task UERL-PhantomX-ContinuousTerrain-v0 --artifact latest --import
```

For a custom engine installation, supply `--ue-executable '<UE-root>\Engine\Binaries\Win64\UnrealEditor.exe'`.
The generated command has this form:

```powershell
& '<UE-root>\Engine\Binaries\Win64\UnrealEditor.exe' '<target-project-directory>\<Project>.uproject' `
  -run=UERLPolicyImport `
  '-artifact=<RunDir>\exported\<task>.uerlpol2' `
  -asset=/Game/UERLEngine/Policies/<PolicyName> `
  -robotmesh=/Game/Robots/PhantomX/SK_PhantomX `
  -unattended -nop4
```

The option `-asset` accepts `/Game/Path/Asset` or `/Game/Path/Asset.Asset`.
Replacement requires `--replace-existing`, or `-replaceexisting` at Commandlet level.
The importer parses the artifact and binds RobotMesh.
It applies the same asset validation as `StartPolicy` and saves a `.uasset`.
It does not place a Blueprint in a map.

Run `UERL.CheckProject`, then validate Play and cook.
The default asset name keeps only letters, digits, and underscores from the Task ID.
Use `--asset` for another package name.

To distribute **Editor precompiled binaries**:

```powershell
uv run python scripts/package_plugin.py --output '<output-directory>'
```

Copy the generated `UERLEngine` into the target's `Plugins/`, or use `uerl deploy --from-package`.
This package does not supply a game DLL for an installed `UnrealGame.exe`.
Game packaging still requires a C++ Game Target.
Deployment can generate it, or you can add it through **Tools → New C++ Class**.

<a id="in-game-deployment-guide-artifact-and-mesh-contract"></a>
### Artifact and mesh contract

The repository includes these demonstration plugin assets:

```text
/UERLEngine/Policies/PhantomXContinuousTerrain116
/UERLEngine/Blueprints/BP_UERLPolicyRobot
```

The policy references `/Game/Robots/PhantomX/SK_PhantomX` in game content.
The plugin does not include the mesh, Skeleton, or PhysicsAsset.
Keep that object path during migration.
Make sure that RobotMesh resolves in the artifact editor.
Save and cook validation also reject a missing mesh.
The identity value `robot_id` cannot infer or replace RobotMesh.

The demo artifact has **116 observations, 18 actions, `physics_dt=0.005 s`, and decimation `[1,7]`**.
Its `velocity` command channel contains `[forward, lateral, yaw]`.
Before another export is used, examine its actual Summary.
PhantomX requires the expected 19-body/18-joint PhysicsAsset topology.

<a id="in-game-deployment-guide-game-physics-settings"></a>
### Game physics settings

`uerl deploy` writes these keys in the target `Config/DefaultEngine.ini`:

```ini
[/Script/Engine.PhysicsSettings]
bTickPhysicsAsync=False
bSubstepping=True
bSubsteppingAsync=False
MaxSubstepDeltaTime=0.005
MaxSubsteps=7
MaxPhysicsDeltaTime=0.033333
```

The value `MaxSubstepDeltaTime` must not exceed the artifact physics timestep.
The value `MaxSubsteps` must cover maximum decimation.
The policy component reads these settings but does not change them.
Training uses a separate lockstep baseline with substepping disabled.
Do not apply that training setting to gameplay deployment.

Control uses the **completed Chaos solver clock**, not game-frame duration.
World time dilation affects game time and stale-command diagnostics.
For stable-timing evaluation, keep default time dilation.

<a id="in-game-deployment-guide-editor-project-checks"></a>
### Editor project checks

After loading, the Editor automatically does a project check.
To repeat it, run this console command:

```text
UERL.CheckProject
```

Results appear in the **UERL** Message Log tab.
Errors open the log and show a notification.

| Check | Meaning |
|---|---|
| Packaging | A Blueprint-only project without suitable game binaries needs a C++ Game Target. Editor-only plugin packaging is insufficient. |
| Artifact | Artifact bytes must parse and RobotMesh must resolve. Set the mesh in the asset editor or migrate content to its referenced path. Save/cook uses the same asset validation. |
| DeployPhysics | Project settings must satisfy each artifact's timing. The report names relevant PhysicsSettings keys. `StartPolicy` additionally validates the actual live solver. |

Startup project checks do not run in cook or command-line mode.

<a id="in-game-deployment-guide-place-the-demonstration-blueprint"></a>
### Place the demonstration Blueprint

If necessary, import an exported `.uerlpol2` through Content Browser.
Examine its Summary, dimensions, timing, command channels, and RobotMesh.
Make sure that a cook reference includes the imported asset.

Place `BP_UERLPolicyRobot` in a map with ground and a PlayerStart.
Use these documented settings:

- `Artifact`: `PhantomXContinuousTerrain116`.
- `bClaimOwnerMesh`: true.
  The `OwnerMesh` field identifies its Skeletal Mesh component.
  If the Owner has multiple matching meshes, select one explicitly.
  Class-default references resolve by component name on the live Owner.
- `bAutoStart`: false, because BeginPlay performs explicit startup.
- `AutoReceiveInput`: Player 0 for keyboard diagnostics.

BeginPlay binds the four diagnostic events and calls `GetRequiredCommandChannels`.
It sets `velocity` to `[0,0,0]`, then calls `StartPolicy`.
Tick uses `GetPlayerPawn(0)` and `GetRobotTransform` for Pursuit commands.
It commands zero inside 1.25 m.
The generic API is `SetCommand`.
There is no `SetCommandVelocity` node.

The operation `GetRobotTransform` returns the claimed or spawned mesh transform, not the Owner transform.
Claiming detaches the mesh from the Owner root.
For a floating base, the physical body reference is local to the mesh. Its authored mounting offset sets the initial placement and is not added again during reset.
Chaos moves the mesh while the Owner can remain at its original position.

Ground queries start near the Robot and point down through WorldStatic, ignoring the Owner.
This prevents an overhead roof from becoming the ground reference.
Deployment terrain scans, clearance, and pose reset use the same downward-ground convention.

With the game viewport focused, the sample Blueprint exposes:

```text
1  StopPolicy
2  SoftReset             (prints return value)
3  ResetToReferencePose  (prints return value)
4  StartPolicy           (prints return value)
```

Alternatively, `bAutoStart=true` starts the component on its first tick.
Set commands in Actor BeginPlay before that tick.
The Egypt host uses this path through `AUERLEgyptChaseGameMode`.
This is host gameplay, not a required plugin interface.

<a id="in-game-deployment-guide-commands-diagnostics-and-reset"></a>
### Commands, diagnostics, and reset

`SetCommand` copies and latches each channel. Query names and widths with `GetRequiredCommandChannels`. The component checks width and finite values; it does not know task-specific units or ranges. For the documented PhantomX task, `velocity` is body-frame forward/lateral speed in m/s followed by yaw rate in rad/s. Use the saved Task's command distribution when deploying its policy; the default sampler uses 0.4–0.5 m/s linear speed, clamps yaw rate to ±1 rad/s, and includes zero-speed standing episodes. `CommandStalenessSeconds` controls stale-command diagnostics; zero disables that diagnostic. Staleness is not a control tick and does not automatically change the action.

Connect these events to logs, HUD, or gameplay state:

| Event | Meaning |
|---|---|
| `OnControlStepOverrun(GameSeconds, PhysicsSeconds, ObservationSeconds)` | Reports three distinct clocks; inspect each separately |
| `OnControlStepCompleted(Frame)` | Optional reset-input or post-physics snapshot aligns commands, raw state, network input, previous action, policy output, actuator targets, and solver/game clocks |
| `OnCommandStale(Channel, StaleSeconds)` | A channel has not been refreshed |
| `OnPolicyFault(Reason)` | Policy stops while the robot remains; handle the cause before resetting |
| `OnPhysicsBaselineMismatch(Report)` | Startup physics validation failed |

Examine reset return values and `GetLastError`:

```text
StopPolicy()                  // Releases this controller's Robot resources; does not pause the World.
SoftReset() -> bResetOK       // Clears history, contact/terrain caches, and solver accumulation.
ResetToReferencePose() -> bPoseOK
                              // Restores the live robot pose without destroying its Actor.
StartPolicy()                 // Explicitly restart after reset.
```

No old control step continues after `StopPolicy`. The runtime destroys a Robot Actor it spawned; for a claimed mesh it restores the attachment and physics settings captured at claim time. `OnPolicyFault` disables inference/ticks but leaves the host to choose a fallback. A claimed mesh may keep simulating its prior drive; restoring an originally non-simulating mesh stops that mesh. `StopPolicy` does not globally pause Chaos or provide a universal emergency stop. Resolve the fault, set every required command channel again, and call `StartPolicy` explicitly to restart. Floating-base pose reset uses current ground and the Owner mounting transform; a fixed base retains its mounting transform. Start-clearance and pose-reset traces ignore the policy component's Owner actor; other blocking `WorldStatic` surfaces below the probe origin remain eligible ground.

For the phase-separated numerical, fixed-action, target-scene, and fault-recovery procedure, see [static-ground deployment validation](deployment.md#policy-deployment-validation).

<a id="in-game-deployment-guide-multiple-instances-and-timing"></a>
### Multiple instances and timing

Each component owns its controller, command latch, previous action, contact state, and terrain cache.
Deployed Robots can collide in one World without shared policy state.
They share the synchronous solver.
Its settings must satisfy every artifact's timing requirements.

The value `DtMin` and actual completed physics advancement determine control execution.
Short physics windows accumulate until the artifact minimum interval is reached.
There is no independent deployment frequency override.
Another supported control range requires suitable training and export.
Timing outside the trained interval can cause an overrun.
Clamping does not establish policy capability outside that interval.

<a id="in-game-deployment-guide-cook-package-and-validate"></a>
### Cook, package, and validate

Build through a project with a C++ Game Target.
An installed monolithic engine without a Game Target can fail with `UERLInterface could not be found`.
The command `UERL.CheckProject` detects this setup before packaging.

Make sure that cook references include the map, Blueprint, game-side mesh, PhysicsAsset, imported policy, and ORT runtime.
Make sure that runtime does not read the original loose `.uerlpol2`.
Expected startup messages have this form:

```text
[UERLPolicyComponent] StartPolicy succeeded owner=BP_UERLPolicyRobot_C_1
[UERLPolicyComponent] first control step frame=7 observation_dt=0.005000 solver_dt=0.000000
```

The host defaults to the empty `/Engine/Maps/Entry`.
For Egypt Play, install the optional content and explicitly open `Stylized_Egypt_Demo`.
Do not change the Editor startup map to the large demo solely for this procedure.
Automation also loads the project.

<a id="in-game-deployment-guide-limitations"></a>
### Limitations

These runtime and packaging instructions target the repository's UE 5.8 Windows setup.
Window-end contact samples cannot recover force peaks that ended earlier.
The demo uses static ground and cached terrain queries.
Moving ground, dynamic obstacles, and new command channels require Task/artifact design and validation.
Demo assets and third-party maps require redistribution review before release.

<a id="policy-deployment-validation"></a>

<a id="policy-deployment-validation-validate-a-static-ground-policy-deployment"></a>
## Validate a static-ground policy deployment

Use this procedure to separate policy computation, actuator/physics response,
and closed-loop behavior when evaluating a saved Run in a game scene. It covers
the supported static-ground path; it does not establish behavior on moving
platforms or with dynamic obstacles.

<a id="policy-deployment-validation-prepare-a-reproducible-target"></a>
### Prepare a reproducible target

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

For an external game project, follow [in-game deployment](deployment.md#in-game-deployment-guide)
to install the plugin and import the same artifact. Build the target Editor,
run `UERL.CheckProject`, and ensure the imported policy asset and its RobotMesh
are referenced by the map's policy actor so they are included in cook.

<a id="policy-deployment-validation-phase-1-compare-computation-with-fixed-inputs"></a>
### Phase 1: compare computation with fixed inputs

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

<a id="policy-deployment-validation-phase-2-compare-fixed-action-physical-response"></a>
### Phase 2: compare fixed-action physical response

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

<a id="policy-deployment-validation-phase-3-evaluate-a-held-out-static-target-scene"></a>
### Phase 3: evaluate a held-out static target scene

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

<a id="policy-deployment-validation-respond-to-timing-and-policy-faults"></a>
### Respond to timing and policy faults

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

<a id="policy-deployment-validation-evidence-boundary"></a>
### Evidence boundary

The UE Automation cases above exercise native plans, reset/query behavior,
fixed-action response, and component fault handling. Passing them does not
establish a held-out target-scene result. Record a target-scene run separately;
if the map, Robot assets, or Windows UE host is unavailable, mark that result
blocked and keep the behavioral release gate open. This procedure covers
static ground only.

<a id="optional-egypt-demo"></a>

<a id="optional-egypt-demo-optional-stylized-egypt-demo"></a>
## Optional Stylized Egypt demo

The Egypt integration is optional.
The current tree excludes Fab meshes, materials, textures, map, built data, and text map export.
Original host GameMode and Pursuit Task code remain available.

The default game map is the empty engine map `/Engine/Maps/Entry`.
Open the optional Egypt map explicitly in the Editor.
The setting `GameDefaultMap` does not configure Editor startup.

<a id="optional-egypt-demo-acquire-and-install-your-copy"></a>
### Acquire and install your copy

1. Open the official [Stylized Egypt listing by AleksandrIvanov](https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd).
   It displayed **Free** on 2026-10-03. Review and accept the applicable terms yourself;
   the displayed price is not public redistribution permission. The inspected page
   did not expose a specific license value and displayed “Allows usage with AI: No”.
   Confirm that the terms cover your intended use, including any RL experiment.
2. Acquire the content through your own Fab account.
   Add it to `engine/UERLHost.uproject` through the supported Fab/Epic procedure.
   If the package requires a supported-version staging project, migrate its assets through Unreal's asset migration procedure.
   Keep the content root name and all map dependencies.
3. Make sure that this file contains an actual map package:
   `engine/Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap`.
   Its Unreal package path must be `/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo`.
   An LFS pointer is not sufficient.
4. Examine materials, textures, and collisions in the Editor.
5. Open the map explicitly.
   Keep local vendor-map changes private.

The repository ignores `engine/Content/Stylized_Egypt/` to keep installed content private.
UE 5.8 compatibility is not established here.
This checkout does not supply a modified vendor map.
It does not guarantee that the current Fab package matches the historical revision.

<a id="optional-egypt-demo-restore-the-integration-settings-locally"></a>
### Restore the integration settings locally

The original integration remains in
[`UERLEgyptChaseGameMode.cpp`](../engine/Source/UERLHost/UERLEgyptChaseGameMode.cpp)
and [`registration.py`](../src/uerl/tasks/phantomx/registration.py).

<a id="optional-egypt-demo-gameplay-demo"></a>
#### Gameplay demo

1. If the map overrides the project GameMode, select `UERLEgyptChaseGameMode` for the local map.
2. Make sure that Player 0 possesses a Pawn and has a usable PlayerStart.
3. Make sure that the separate Robot and policy assets are available.

The project keeps this global GameMode.
Its chase logic activates only on map names that contain `Stylized_Egypt`.
It loads `/Game/UERLEngine/Policies/PhantomXContinuousTerrain` and spawns the policy Robot near the player.
See [deployment](deployment.md#in-game-deployment-guide).

<a id="optional-egypt-demo-worker-pursuit"></a>
#### Worker Pursuit

The optional `UERL-PhantomX-Pursuit-v0` Task selects the Egypt map.
It requires one Slot, Player 0, and an authored actor with the configured PhantomX Skeletal Mesh.
It also requires WorldStatic ground below **(-9.4, 1.6) metres**.
The ground trace extends from z=10 m to z=-10 m.
The original coordinates and `robot.claim_authored_actor=1` remain unchanged.

1. Select a local non-chase GameMode with a Player 0 Pawn.
2. Add one authored actor with the configured PhantomX Skeletal Mesh.
3. Disable the chase GameMode's separate policy Robot and any other automatic policy component.
4. Launch with `--presentation gameplay` to keep the map's player setup.

A fresh Fab package does not include this project's authored Robot integration.
Keep gameplay-demo and Worker-task GameMode settings separate.

For another pack revision, examine each integration assumption.
Do not silently change ground origin, Robot topology, policy contracts, or Task semantics to obtain a successful launch.
Native map loading, gameplay, and Pursuit validation remain outstanding in this guide.

<a id="optional-egypt-demo-missing-content-behavior-and-non-fab-workflows"></a>
### Missing-content behavior and non-Fab workflows

Before training, playback, or export starts UE, Python examines the selected Egypt map file.
An absent, empty, or LFS-pointer `.umap` causes rejection with the Fab link and this guide.
This is a presence check only.
It does not establish asset dependencies, map customization, license rights, or package compatibility.

Attach mode uses an existing host and cannot inspect its remote map locally.
The command `uerl check task` validates declarations only.

CartPole and continuous/discrete PhantomX terrain keep `/Engine/Maps/Entry`.
They do not substitute another map for Egypt.
Flat walking keeps `/Game/Maps/NewMap`.
Its binary dependency closure still requires an Editor inspection.
Robot asset provenance and LFS availability remain separate prerequisites.

The exclusion did not purge historical assets.
Before a repository visibility change, read [publication status and recovery](release-readiness.md#remaining-publication-decisions).
