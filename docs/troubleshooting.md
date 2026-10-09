# Troubleshooting

Start with these Python checks before UE startup:

```powershell
uv run uerl tasks
uv run uerl config --task <TaskID>
uv run uerl check task <TaskID>
```

Replace `<TaskID>` with a listed Task ID.
These commands help separate configuration failures from UE startup failures.

## Choose the failing stage

| Symptom | Start here |
|---|---|
| Task lookup or configuration fails | [Task ID and overrides](#unknown-task-id-or-override) |
| UE does not start | [Executable path](#ue-executable-not-found), [host plugins](#missing-host-plugin) |
| Content cannot load | [UE assets](#missing-or-invalid-ue-assets) |
| Policy device cannot initialize | [CUDA](#cuda-unavailable) |
| An old Run cannot be restored | [Checkpoint compatibility](#old-phantomx-checkpoint-cannot-resume), [saved configuration](#missing-or-conflicting-saved-configuration) |
| Evaluation reports success but the robot barely moves | [Motion and success metrics](#success-result-with-little-robot-motion) |
| The game rejects a policy at startup | [Physics gate](#deployment-physics-gate-failure), [RobotMesh](#imported-artifact-has-no-robotmesh) |
| Packaging fails | [Game Target](#packaged-game-cannot-find-uerlinterface) |

## Unknown Task ID or override

Copy the ID from `uerl tasks`.
Examine the resolved configuration and command help.
The CLI also suggests similar Task names.

## Missing or invalid UE assets

Make sure that `git lfs pull` completed.
The files must contain actual assets, not LFS pointers.
If retrieval fails, examine access to the repository's LFS storage.

## UE executable not found

Supply `--ue-executable` to `train`, `play`, or `export`.
The default path is the Windows Epic `UE_5.8` installation.
A host profile can supply another path.
Setting `UE_ROOT` alone does not change these product commands.

## Missing host plugin

Make sure that the bundled `UERLEngine` source is present.
Make sure that the selected UE installation supplies `ProceduralMeshComponent` and `NNERuntimeORT`.
Rebuild the host with the matching UE version.

## CUDA unavailable

Examine `nvidia-smi` and `torch.cuda.is_available()` in the active Python environment.
A CUDA wheel does not supply an NVIDIA device or driver.
Where appropriate, use `--device cpu`.
CPU policy execution still requires UE simulation.

## Old PhantomX checkpoint cannot resume

Read the [objective and curriculum compatibility rules](how-to/phantomx-robust-training.md#start-and-resume-a-run).
Select a compatible Run or start a new training Run.

## Incompatible terrain curriculum during playback

Playback starts with registered Task curriculum settings.
Use `--terrain-level N` for a fixed procedural tier.

## Missing or conflicting saved configuration

Compare the checkpoint's embedded YAML with `resolved_config.yaml` and `manifest.yaml`.
To preview a separate recovery copy, run:

```powershell
python -m uerl.application.run_migration --source <run> --output <recovered-run>
```

Runtime loaders do not read historical JSON or earlier YAML schemas.
Migration stops on missing fields or Task/version mismatch.
It never fills missing values from current defaults.

The default checkpoint copy depends on the Run sidecar.
Add `--embed-checkpoint-config` only if the dry-run accepts weights-only loading and you need a detached checkpoint.
Keep the source Run and checkpoint unchanged.

## Success result with little robot motion

Compare forward velocity, speed error, and completed episode counts.
Success counts completed episodes without base contact.
It does not establish command tracking.

## Worker initialization timeout

Examine UE build errors, content errors, and Worker logs first.
Session connection and request timeouts default to 120 seconds.
A timeout invalidates the Session.
Do not replay a Step without a valid recovery decision.

## Deployment physics gate failure

Run `UERL.CheckProject` in the Editor.
Use the [synchronous substep settings](in-game-deployment-guide.md#game-physics-settings).
The runtime gate reads the actual solver.

## Imported artifact has no RobotMesh

Assign or migrate the matching Skeletal Mesh to the referenced game path.
The `robot_id` value does not replace a mesh reference.

## Packaged game cannot find UERLInterface

Build through a project with a C++ Game Target.
Editor-only precompiled plugin binaries do not replace that target.

## Slow or unusable video

Make sure that viewport rendering works and the MP4 encoder dependency is installed.
Recording requires viewport presentation and a newly launched Worker.
It is slower than headless training.

## Report a failure

Include the command, Task, commit, OS, Python/UE versions, and applicable redacted logs.
State whether the failure occurred before or after Worker initialization.
For CUDA failures, include GPU and driver information.
Keep credentials, private paths, and proprietary assets out of public reports.
