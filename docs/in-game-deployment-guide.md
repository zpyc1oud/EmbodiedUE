# In-game policy deployment

Deploy a `.uerlpol2` policy on a Skeletal Mesh Actor in UE 5.8.
Inference uses the imported `UUERLPolicyArtifactAsset`, its mesh, and its PhysicsAsset.
Editor import and reimport require the source `.uerlpol2`.
The packaged game does not need that source as a loose file.

After [optional Fab installation and setup](how-to/optional-egypt-demo.md), `Stylized_Egypt_Demo` gameplay spawns PhantomX to pursue the player.
For another game, use `BP_UERLPolicyRobot` or `UUERLPolicyComponent`.
The host GameMode is demonstration code.

## Prepare a target project

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

## Import an artifact

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

## Artifact and mesh contract

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

## Game physics settings

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

## Editor project checks

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

## Place the demonstration Blueprint

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

## Commands, diagnostics, and reset

`SetCommand` copies and latches each channel. Query names and widths with `GetRequiredCommandChannels`. The component checks width and finite values; it does not know task-specific units or ranges. For the documented PhantomX task, `velocity` is body-frame forward/lateral speed in m/s followed by yaw rate in rad/s. Use the saved Task's command distribution when deploying its policy; the default sampler uses 0.4–0.5 m/s linear speed, clamps yaw rate to ±1 rad/s, and includes zero-speed standing episodes. `CommandStalenessSeconds` controls stale-command diagnostics; zero disables that diagnostic. Staleness is not a control tick and does not automatically change the action.

Connect these events to logs, HUD, or gameplay state:

| Event | Meaning |
|---|---|
| `OnControlStepOverrun(GameSeconds, PhysicsSeconds, ObservationSeconds)` | Reports three distinct clocks; inspect each separately |
| `OnControlStepCompleted(Frame)` | Optional post-physics snapshot aligns commands, raw state, network input, previous action, policy output, actuator targets, and solver/game clocks |
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

For the phase-separated numerical, fixed-action, target-scene, and fault-recovery procedure, see [static-ground deployment validation](how-to/policy-deployment-validation.md).

## Multiple instances and timing

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

## Cook, package, and validate

Build through a project with a C++ Game Target.
An installed monolithic engine without a Game Target can fail with `UERLInterface could not be found`.
The command `UERL.CheckProject` detects this setup before packaging.

Make sure that cook references include the map, Blueprint, game-side mesh, PhysicsAsset, imported policy, and ORT runtime.
Make sure that runtime does not read the original loose `.uerlpol2`.
Expected startup messages have this form:

```text
[UERLPolicyComponent] StartPolicy succeeded owner=BP_UERLPolicyRobot_C_1
[UERLPolicyComponent] first control step frame=7 observation_dt=0.005000 solver_dt=0.005000
```

The host defaults to the empty `/Engine/Maps/Entry`.
For Egypt Play, install the optional content and explicitly open `Stylized_Egypt_Demo`.
Do not change the Editor startup map to the large demo solely for this procedure.
Automation also loads the project.

## Limitations

These runtime and packaging instructions target the repository's UE 5.8 Windows setup.
Window-end contact samples cannot recover force peaks that ended earlier.
The demo uses static ground and cached terrain queries.
Moving ground, dynamic obstacles, and new command channels require Task/artifact design and validation.
Demo assets and third-party maps require redistribution review before release.
