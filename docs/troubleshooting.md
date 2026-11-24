# Troubleshooting and diagnostics

[Project home](../README.md) · [Training guide](training.md)

## Contents

- [Troubleshooting](#troubleshooting)
- [Diagnose a recorded training result](#diagnose-a-recorded-training-result)
- [Inspect every Slot during training](#inspect-every-slot-during-training)


<a id="troubleshooting"></a>

<a id="troubleshooting-troubleshooting"></a>
## Troubleshooting

Start with these Python checks before UE startup:

```powershell
uv run uerl tasks
uv run uerl config --task <TaskID>
uv run uerl check task <TaskID>
```

Replace `<TaskID>` with a listed Task ID.
These commands help separate configuration failures from UE startup failures.

<a id="troubleshooting-choose-the-failing-stage"></a>
### Choose the failing stage

| Symptom | Start here |
|---|---|
| Task lookup or configuration fails | [Task ID and overrides](troubleshooting.md#troubleshooting-unknown-task-id-or-override) |
| UE does not start | [Executable path](troubleshooting.md#troubleshooting-ue-executable-not-found), [host plugins](troubleshooting.md#troubleshooting-missing-host-plugin) |
| Content cannot load | [UE assets](troubleshooting.md#troubleshooting-missing-or-invalid-ue-assets) |
| Policy device cannot initialize | [CUDA](troubleshooting.md#troubleshooting-cuda-unavailable) |
| An old Run cannot be restored | [Checkpoint compatibility](troubleshooting.md#troubleshooting-old-phantomx-checkpoint-cannot-resume), [saved configuration](troubleshooting.md#troubleshooting-missing-or-conflicting-saved-configuration) |
| Evaluation reports success but the robot barely moves | [Motion and success metrics](troubleshooting.md#troubleshooting-success-result-with-little-robot-motion) |
| The game rejects a policy at startup | [Physics gate](troubleshooting.md#troubleshooting-deployment-physics-gate-failure), [RobotMesh](troubleshooting.md#troubleshooting-imported-artifact-has-no-robotmesh) |
| Packaging fails | [Game Target](troubleshooting.md#troubleshooting-packaged-game-cannot-find-uerlinterface) |

<a id="troubleshooting-unknown-task-id-or-override"></a>
### Unknown Task ID or override

Copy the ID from `uerl tasks`.
Examine the resolved configuration and command help.
The CLI also suggests similar Task names.

<a id="troubleshooting-missing-or-invalid-ue-assets"></a>
### Missing or invalid UE assets

Make sure that `git lfs pull` completed.
The files must contain actual assets, not LFS pointers.
If retrieval fails, examine access to the repository's LFS storage.

<a id="troubleshooting-ue-executable-not-found"></a>
### UE executable not found

Supply `--ue-executable` to `train`, `play`, or `export`.
The default path is the Windows Epic `UE_5.8` installation.
A host profile can supply another path.
Setting `UE_ROOT` alone does not change these product commands.

<a id="troubleshooting-missing-host-plugin"></a>
### Missing host plugin

Make sure that the bundled `UERLEngine` and `UERLTraining` sources are present.
Make sure that the selected UE installation supplies `ProceduralMeshComponent` and `NNERuntimeORT`.
Rebuild the host with the matching UE version.

<a id="troubleshooting-cuda-unavailable"></a>
### CUDA unavailable

Examine `nvidia-smi` and `torch.cuda.is_available()` in the active Python environment.
A CUDA wheel does not supply an NVIDIA device or driver.
Where appropriate, use `--device cpu`.
CPU policy execution still requires UE simulation.

<a id="troubleshooting-old-phantomx-checkpoint-cannot-resume"></a>
### Old PhantomX checkpoint cannot resume

Read the [objective and curriculum compatibility rules](training.md#phantomx-robust-training-start-and-resume-a-run).
Select a compatible Run or start a new training Run.

<a id="troubleshooting-incompatible-terrain-curriculum-during-playback"></a>
### Incompatible terrain curriculum during playback

Playback starts with registered Task curriculum settings.
Use `--terrain-level N` for a fixed procedural tier.

<a id="troubleshooting-missing-or-conflicting-saved-configuration"></a>
### Missing or conflicting saved configuration

Compare the checkpoint's embedded YAML with `resolved_config.yaml` and `manifest.yaml`.
Runtime loaders require schema-version-1 Run configuration and do not read historical JSON or earlier YAML schemas.
If a Run uses an unsupported legacy metadata format, start a new training Run.

<a id="troubleshooting-success-result-with-little-robot-motion"></a>
### Success result with little robot motion

Compare forward velocity, speed error, and completed episode counts.
Success counts completed episodes without base contact.
It does not establish command tracking.

<a id="troubleshooting-worker-initialization-timeout"></a>
### Worker initialization timeout

Examine UE build errors, content errors, and Worker logs first.
Session connection and request timeouts default to 120 seconds.
A timeout invalidates the Session.
Do not replay a Step without a valid recovery decision.

<a id="troubleshooting-deployment-physics-gate-failure"></a>
### Deployment physics gate failure

Run `UERL.CheckProject` in the Editor.
Use the [synchronous substep settings](deployment.md#in-game-deployment-guide-game-physics-settings).
The runtime gate reads the actual solver.

<a id="troubleshooting-imported-artifact-has-no-robotmesh"></a>
### Imported artifact has no RobotMesh

Assign or migrate the matching Skeletal Mesh to the referenced game path.
The `robot_id` value does not replace a mesh reference.

<a id="troubleshooting-packaged-game-cannot-find-uerlinterface"></a>
### Packaged game cannot find UERLInterface

Build through a project with a C++ Game Target.
Editor-only precompiled plugin binaries do not replace that target.

<a id="troubleshooting-slow-or-unusable-video"></a>
### Slow or unusable video

Make sure that viewport rendering works and the MP4 encoder dependency is installed.
Recording requires viewport presentation and a newly launched Worker.
It is slower than headless training.

<a id="troubleshooting-report-a-failure"></a>
### Report a failure

Include the command, Task, commit, OS, Python/UE versions, and applicable redacted logs.
State whether the failure occurred before or after Worker initialization.
For CUDA failures, include GPU and driver information.
Keep credentials, private paths, and proprietary assets out of public reports.


## Diagnose a recorded training result

Use this procedure when a policy survives but does not follow its commands.
The report reads existing files. It does not start UE or modify a Run.

### Record and inspect a trace

Use the existing `uerl play --trace` option to record a single-Slot evaluation.
Keep the original Run, checkpoint and resolved configuration. Then run:

```powershell
uv run uerl diagnose training-trace runs/example/traces/task.yaml
uv run uerl diagnose training-trace runs/example/traces/task.yaml --events runs/example/logs --last-iterations 100
```

Pass the actual TensorBoard directory to `--events`. Omit it if no events exist.
Use `--body` and `--command` for a different root body or velocity-command channel.
The body must have pose, linear velocity and angular velocity fields in the trace.
The command must contain body-frame forward speed, lateral speed and yaw rate.

The output is YAML. It reports:

- Physical-time-weighted planar velocity and yaw-rate RMSE
- Command-group coverage, tracking RMSE, actual planar speed and absolute yaw rate
- Planar path length and measured displacement for each episode
- Action RMS and successive-action change within an episode
- Recorded force sample counts and maximum force for each available body
- Completed termination counts and the sum of transition rewards
- Optional TensorBoard scalar windows, including recorded reward components

The position calculation uses the decision input and pre-reset transition.
It excludes reset teleports. A report can include a partial episode.
Action changes do not cross reset boundaries or gaps in recorded policy steps.
The report preserves the source trace's completion status.

`command_groups` partitions each executed interval into `standing`, `forward`,
`backward`, `lateral`, `moving_turn`, or `yaw_only`. Classification uses the
decision input command: all three components zero means standing; a nonzero yaw
command selects moving turn or yaw only according to planar motion. Translation
without yaw follows the sign of forward command; pure lateral motion has zero
forward command. Diagonal translation follows forward sign. Exact zeros preserve
low-speed commands without a classification threshold.

Each group reports its samples and measured seconds. Errors and speeds use
physical-time weighting within that group. Unobserved groups have null metrics,
so missing command coverage cannot appear as perfect tracking. Standing planar
speed and absolute yaw rate measure drift. Grouped termination and timeout-only
counts belong to the last executed command, not whole-episode success. Inspect
groups separately so long standing intervals cannot hide failed moving commands.

`low_motion_speed_threshold_m_s` defaults to 0.01. The report flags a planar command
above this threshold when measured path speed is below it. Change the threshold
with `--low-motion-speed`. A finding is an inspection prompt, not a success criterion.
Zero recorded force does not prove that a foot missed the ground.

### Keep metric meanings separate

A trace covers one recorded Slot. It is not a summary of every training environment.
No completed episode means that an episode success rate is unavailable.
The report does not invent a success criterion or reward components absent from the trace.

TensorBoard results retain the original tag names. Each tag reports its latest
value and mean over the requested final iteration range. Missing iterations do
not become zeros. These means can summarize values that were already averaged
by the training logger. Do not sum them as though they were one episode reward,
or compare them directly with physical-time-weighted evaluation RMSE.

### Check configuration before UE startup

```powershell
uv run uerl check task UERL-PhantomX-Walk-v0 --yaml --worker.physics_dt 0.005 --worker.decimation "[4, 4]"
uv run uerl check task UERL-PhantomX-Walk-v0 --run runs/example --yaml
uv run uerl check task UERL-PhantomX-Walk-v0 --run runs/example --artifact runs/example/exported/policy.uerlpol2 --yaml
```

The first command checks resolved defaults plus typed overrides. A saved-Run check
uses its recorded configuration and rejects overrides. Artifact checks compare
Task and Robot identity, physics timing and actuator configuration. They also
check ONNX input/output dimensions against the artifact's own plans.

Read `pending_host_checks` and the artifact's `pending` fields. Actual mesh topology,
bound Task plan equality, scene binding and physical response still need the UE
host. An offline pass does not establish those properties.

## Inspect every Slot during training

Use a short debug Run to check the training data path before changing rewards or PPO parameters.
Set the Run length and capture length explicitly:

```powershell
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --num-envs 2 --max-iterations 2 --debug-trace runs/debug/steps.yaml --debug-rollouts 2
uv run uerl diagnose training-debug runs/debug/steps.yaml
```

The trace contains every Slot at each captured control step:

- Raw state, commands, observations, and previous actions before the decision takes effect.
- Actor and critic inputs after normalization, sampled actions, values, and action likelihoods.
- Physical targets, completed transition state, elapsed simulated time, and decimation.
- Weighted reward terms, rewards, termination, timeout, and sparse reset results.
- PPO storage, timeout bootstrap, returns, advantages, and optimizer gradient statistics.

The recorder streams YAML documents to disk. It saves model and optimizer snapshots in adjacent `.pt` files.
It refuses to overwrite an existing trace or snapshot.
`--debug-rollouts` limits the capture; it does not stop training.
Use `--max-iterations` to limit the Run itself.
Start with a few Slots, then repeat with the required batch size.
Disk writes and CPU copies slow a debug Run. Do not use it as a throughput benchmark.

The offline checker independently reconstructs normalized inputs, Gaussian likelihoods,
action clipping and scaling, timeout bootstrap, and GAE targets.
It checks Slot order, reward sums, reset mappings, physical-time discounts, and optimizer update counts.
A mismatch fails the command. An interrupted or incomplete capture also fails.
Custom Task methods can omit compiled plans or reward decomposition. Inspect those fields separately.

### Debug limits

This mode supports the pinned MLP actor and critic with PPO or TimeAwarePPO.
A recorded step is one completed policy control window, not each internal Chaos solver substep.
Physical targets are requested actuator commands; they are not measurements of solver torque.
Passing the checker establishes the recorded data mappings and arithmetic.
Use real UE contact, joint-response, and coordinate checks to verify the physical model.
Use training and evaluation results to assess whether the policy learns the task.

