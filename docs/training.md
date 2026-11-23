# Training and task configuration

[Project home](../README.md)

## Contents

- [Reuse a host profile](#ue-host-profile)
- [Task configuration](#configuration)
- [PhantomX terrain training and evaluation](#phantomx-robust-training)
- [Recording video after training](#record-video)

Related guides: [Extending EmbodiedUE](extending.md) (adding robots, external Tasks, numeric inputs) and [Troubleshooting and diagnostics](troubleshooting.md).


<a id="ue-host-profile"></a>

<a id="ue-host-profile-reuse-a-host-profile"></a>
## Reuse a host profile

Save the UE executable and host project paths in one profile.
The commands `train`, `play`, and `export` use this profile when they launch a Worker.
Run `uerl check host` from the installed Python environment for static file checks.

This preflight applies to the bundled Windows x64 / UE 5.8 CartPole host.
First complete the [installation and build instructions](../README.md#install-and-build).

<a id="ue-host-profile-create-the-local-profile"></a>
### Create the local profile

Create `%USERPROFILE%\.uerl\host.toml` outside the repository.
The profile is host-specific configuration.
Both fields are optional:

```toml
ue_executable = 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
project = 'D:\work\EmbodiedUE\engine\UERLHost.uproject'
```

Replace the paths with your host paths.
TOML single-quoted strings preserve Windows backslashes.
For `project`, select a `.uproject` file, not a directory.

Run the static checks:

```powershell
uv run uerl check host
uv run uerl check host --json
```

To select another profile, use `--host-profile` or the environment variable:

```powershell
uv run uerl check host --host-profile 'D:\profiles\lab.toml'
$env:UERL_HOST_PROFILE = 'D:\profiles\lab.toml'
uv run uerl check host
uv run uerl check host --project 'D:\other\engine\UERLHost.uproject'
```

The command reads files only.
It does not create the profile or change project, plugin, driver, environment, or system settings.

<a id="ue-host-profile-resolution-rules"></a>
### Resolution rules

| Selection | Priority, highest first |
|---|---|
| Profile file | `--host-profile`, `UERL_HOST_PROFILE`, `~/.uerl/host.toml` |
| Executable | `--ue-executable`, profile `ue_executable`, Epic UE 5.8 default |
| Project | `--project`, profile `project`, checkout `engine/UERLHost.uproject` |

The Epic default is `C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/UnrealEditor-Cmd.exe`.
The checkout default is relative to the installed Python source, as in the existing runtime commands.
For another installation, set `project` explicitly.

If the implicit default profile is absent, the command uses default values.
An explicitly selected profile must exist.
This also applies to a profile selected through the environment variable.

Invalid TOML, unknown fields, empty values, and non-string values cause failure.
A command-line override does not hide an invalid profile field.
Profiles do not merge.

A relative profile value is relative to the profile directory.
A relative command-line path is relative to the current directory.
The resolver expands `~`.
It does not expand environment variables inside field values or read `UE_ROOT`.
The host that executes the command interprets the paths.

<a id="ue-host-profile-read-and-reuse-the-result"></a>
### Read and reuse the result

| Exit status | Meaning |
|---|---|
| `0` | All static checks passed |
| `1` | Configuration or static check failure |
| `2` | Invalid CLI use |

Text output gives the selected paths, their sources, check codes, and corrective actions.
Source values are `explicit`, `profile`, or `default`.
JSON output contains `ok`, `scope`, `platform`, `selected`, and `checks`.
A profile read or parse failure returns `ok: false` and an `error` with `code` and `message`.

In launch mode, `train`, `play`, and online `export` (`--online` or legacy Runs without `export_contract.yaml`) use these same resolution rules; Runs containing `export_contract.yaml` export offline without launching UE.
After static preflight passes, run:

```powershell
uv run uerl train --task UERL-CartPole-Direct-v0 --num-envs 2 --max-iterations 1 --device cpu
$runDir = 'runs/UERL-CartPole-Direct-v0/<run-directory>'
uv run uerl play --task UERL-CartPole-Direct-v0 --run $runDir
uv run uerl export --task UERL-CartPole-Direct-v0 --run $runDir
```

All three commands accept `--host-profile`, `--ue-executable`, and `--project`.
(`uerl deploy` also accepts `--host-profile` and `--ue-executable` to resolve the Editor binary when importing `--artifact`.)
An explicit path replaces only its own field.
An executable override therefore retains the profile's project path.

Advanced overrides have priority over computed launch values, including explicit path flags.
These overrides are `--session.worker_executable` and `--session.worker_args`.
The latter replaces the complete argument list.
Map and port precedence remain unchanged.

With `--session.mode attach`, the commands ignore host profiles and local launch flags.
External-Worker ownership rules remain unchanged.
An absent or malformed local profile cannot prevent attachment.

Host paths supply Session launch settings only.
They do not replace saved Worker, Task, or runner settings.
They do not edit the source Run or change continuation rules.
The resolved-configuration hash includes Session settings, so host path changes can change that hash.
The hash is not a semantic compatibility test.

The profile does not select a deployment target or change `deploy --check` or the E2E runner.
Runtime commands launch UE, but `check host` does not.
Runtime commands resolve the profile without automatically doing the static file checks.

Python callers can use `uerl.host.resolve_host_profile(profile_path=...)` and `uerl.host.check_host(profile)`.
Resolution returns selected `Path` values, `profile_path`, and field `sources`.
It does not require the selected files to exist.
The report contains `checks` and an `ok` property.
These APIs do not import the training runner or saved-run resolver.

<a id="ue-host-profile-checked-files-and-repairs"></a>
### Checked files and repairs

| Check | Corrective action |
|---|---|
| Executable and Windows engine directory layout | Select UE 5.8 `Engine/Binaries/Win64/UnrealEditor-Cmd.exe`. |
| `.uproject` JSON and explicit plugin enablement | Use the bundled host structure. Enable UERLEngine, UERLTraining, ProceduralMeshComponent, and NNERuntimeORT. |
| Plugin descriptor JSON | Restore the bundled UERLEngine and UERLTraining plugins or install the missing engine dependency. |
| Host and Worker DLL presence | Build `UERLHostEditor Win64 Development` with the selected engine. |
| Engine Entry map and four CartPole files | Repair engine content or run `git lfs pull` in the host checkout. |

Missing, unreadable, empty files and Git LFS pointers cause failure.
The default CartPole map is `/Engine/Maps/Entry`.
The robot files are `SK_CartPole`, `SKM_CartPole`, `PA_CartPole`, and `CartPole` under `Content/Robots/CartPole`.
This preflight does not include custom maps or PhantomX resources.

<a id="ue-host-profile-limits"></a>
### Limits

Static success does not establish runtime readiness.
It does not validate UE version, DLL compatibility or age, transitive asset references, asset loading, GPU availability, physics, or training.
After preflight, do the documented Windows build and smoke test.

Linux cases with dummy files establish resolution and diagnostic behavior only.
Startup probes, process cleanup, and independent Windows first-use validation remain in [Issue #5](https://github.com/zpyc1oud/EmbodiedUE/issues/5).

<a id="configuration"></a>

<a id="configuration-task-configuration"></a>
## Task configuration

Use `uerl config` to examine resolved configuration for a registered Task.
Base YAML alone is insufficient.
Terrain and Pursuit factories replace parts of the base PhantomX configuration.

```powershell
uv run uerl tasks --filter PhantomX
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0
uv run uerl check task UERL-PhantomX-ContinuousTerrain-v0
```

These commands do not start UE.
A successful Task check validates declarations, not physical asset contents or runtime performance.

<a id="configuration-choose-what-to-change"></a>
### Choose what to change

- Change mesh geometry, body mass, or joint limits in UE assets.
- Change actuator selection, gains, or reference pose in the robot declaration.
- Change commands, observations, rewards, or termination rules in the Task.
- Change parallelism and training duration with supported CLI overrides.
- Use the saved Run configuration when evaluating or exporting an existing policy.

For a new behavior, follow [external Tasks](extending.md#external-tasks).
For a new physical robot, follow [add a robot](extending.md#add-a-robot).

<a id="configuration-sources-of-truth"></a>
### Sources of truth

| Source | Owns |
|---|---|
| UE Skeletal Mesh / Skeleton / PhysicsAsset | Bodies, joints, hierarchy, mass, inertia, geometry, and limits |
| `src/uerl/assets/robots/*.py` | Asset references, actuator semantics, gains, reference poses, observation selection, and reset distributions |
| `src/uerl/configs/tasks/*/training.yaml` | Base task, Worker, and runner settings |
| `src/uerl/tasks/*/registration.py` | Task-specific factories and defaults |
| `src/uerl/configs/environments/terrains/phantomx/*.yaml` | Terrain tiers and generation parameters |
| Run `resolved_config.yaml` and `manifest.yaml` | The configuration and identities recorded for an actual run |

Robot declarations support derivation and regular-expression joint selection.
The runtime generates Worker projections from those declarations and reflected topology.
They are not a second configuration for manual changes.

<a id="configuration-overrides"></a>
### Overrides

Common training flags select typed configuration fields:

| Flag | Field |
|---|---|
| `--num-envs` | `worker.slot_count` |
| `--seed` | `worker.run_seed` |
| `--device` | `runner.device` |
| `--max-iterations` | `runner.max_iterations` |

The command `play` supplies `--seed` and `--device`.
The command `export` supplies `--device`.
Use repeated dotted-path arguments for other configurable values.
For example, examine a shorter training configuration:

```powershell
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0 --runner.max_iterations 100
```

Unknown or non-configurable paths cause rejection.
Do not combine a common flag with a dotted override for the same field.
The option `--run-dir` conflicts with `--logging.run_directory`.
The option `--resume` conflicts with `--runner.checkpoint`.

Dimension, timing, Robot-semantic, or objective changes can make a checkpoint incompatible.
The commands `play` and `export` restore saved Run settings.
For repeatable results, prefer a Run directory or a checkpoint inside its Run over detached weights.

<a id="configuration-saved-run-configuration"></a>
### Saved-run configuration

The commands `play --run` and `export --run` use [application/run_config.py](../src/uerl/application/run_config.py).
The resolver reads Task identity from `resolved_config.yaml`, embedded checkpoint configuration, or the Run manifest.
It restores saved Worker, Task, runner, map, and protocol settings and keeps the training hash.

For an explicit Run directory, `--task` is optional.
If supplied, the Task must match the saved identity.
The selection `--run latest` requires `--task`, because latest selection occurs within a Task.
A `--checkpoint` inside a directory with saved Run configuration or a manifest identifies its Run automatically.

A detached checkpoint can restore settings from embedded versioned YAML.
When checkpoint and Run metadata both exist, their typed configuration values must agree.
The resolver does not edit saved files.

Play and export require complete saved configuration and the registered Task version used for training.
A missing snapshot, missing required field, or changed Task version fails before Session startup.
Current defaults never fill those gaps.
The saved Task, reward, timing, Robot, plan, and PPO settings remain unchanged.

Play permits an explicit `--seed`.
Both commands permit `--device`, one Slot, map, Session connection/launch settings, and presentation settings for evaluation or the host.
Dotted overrides that change saved training semantics cause rejection.

Map priority is `--map`, then `--session.map_path`, then the saved map.
The selected map appears in Worker launch arguments and Session configuration.
Python callers must import `resolve_run_config` from `uerl.application.run_config` instead of the former `uerl.cli.run_config`.
The returned configuration includes map restoration.

<a id="configuration-recovering-historical-runs-and-detached-checkpoints"></a>
#### Recovering historical Runs and detached checkpoints

Current readers accept schema version 1 YAML in `resolved_config.yaml` and new RSL-RL checkpoint metadata.
They also accept the current YAML Run manifest.
Metadata inspection uses PyTorch's weights-only loader.
Training, play, and export do not read historical JSON or earlier YAML sidecar schemas.
For historical Runs in older JSON or unversioned YAML formats, train a new Run to produce schema-version-1 metadata.

<a id="configuration-continue-training"></a>
### Continue training

The command `train --resume` restores saved Worker, Task, runner, map, and protocol settings.
It then applies permitted host, output, and budget changes.
Supported inputs include a Run reference, `latest`, or a checkpoint inside the Run root, `rsl_rl/`, or `checkpoints/`.
The dotted option `--runner.checkpoint` uses the same rules.

A new detached checkpoint includes its configuration snapshot.
A historical detached checkpoint needs complete historical configuration or a manifest in a small source directory.
Migrate that source into a separate recovery directory.
Weights alone cannot establish training semantics.

```powershell
uv run uerl train --task UERL-CartPole-Direct-v0 --resume runs/cartpole_source --run-dir runs/cartpole_continued --max-iterations 100
```

The option `--max-iterations` is an **additional** budget, not an absolute iteration target.
The source snapshot is captured once for launch arguments and final configuration.
The `[CONFIG]` lines show changed field paths, saved values, effective values, and change sources.
Sources include explicit overrides, launch settings, local defaults, and generated output.
The new snapshot and `command.txt` keep the effective configuration and selected checkpoint.

Permitted changes include:

- Session launch/attach mode, executable, host, port, timeouts, and presentation
- Runner device, iteration budget, `run_name`, `experiment_name`, and `save_interval`
- Output directory

Supply the project and window size through ordinary launch flags.
Arbitrary `--session.worker_args` changes cause rejection.
Use the launch flags instead.
When supplied as a dotted override, `session.worker_args` is a YAML sequence of strings.
Earlier JSON string arrays are valid YAML input.
The destination must be empty or new and outside the source Run.

Changes to Slot count, seed, map, rewards, timing, Robot/terrain semantics, network structure, or PPO settings cause rejection.
This applies even when tensor dimensions match.
An explicit value equal to the saved value is accepted.
Host overrides do not establish equivalent assets in another UE installation or project.

<a id="configuration-checkpoint-state-and-old-runs"></a>
#### Checkpoint state and old Runs

Only load checkpoints from trusted sources.
Training-state recovery uses `torch.load(..., weights_only=False)`.
Pickle can execute code during deserialization.
Empty, truncated, invalid, or Git LFS pointer files cause a checkpoint-path error before Session startup.
Restore a complete trusted checkpoint, including actual LFS content where applicable, before retry.
These error checks do not make untrusted checkpoints safe.

The runner restores actor and critic state, enabled normalization statistics, and actor distribution parameters.
It also restores PPO optimizer state, iteration, named curriculum state, the decimation RNG, and enabled RND state.
Episodes start fresh.
Checkpoints do not save UE physical state, rollout buffers, event timers, or all process RNG states.
Continuation does not guarantee the same trajectory as uninterrupted execution.
The PhantomX physical-time objective check remains required.

New training checkpoints store `terrain_level` and `freeze_observation_normalization` in `infos.uerl_training_options`.
Continuation recovers them automatically.
The `[RESTORE]` output gives the selected restoration policy.

A complete current-schema snapshot can resume with compatible checkpoint state.
Historical JSON and earlier YAML schemas require migration first.
Missing semantic fields, required runner parameters, a Task/version mismatch, or missing checkpoint state fail before Session startup.
Continuation never fills those gaps with current defaults.

For saved runtime options:

- A saved terrain-curriculum term identifies adaptive terrain.
  Without it, a terrain Run requires its original explicit `--terrain-level N`.
- A Run with normalization but no saved update policy requires `--resume-normalization update` or `--resume-normalization frozen`.
  Supply the policy actually used.
  Statistics alone do not show whether updates were frozen.
- Explicit recovery inputs must match recorded options when those exist.
  The next checkpoint saves recovered options.
- Variable decimation requires its saved generator state.
  Fixed decimation accepts an absent unused sampler state and reports that condition.

The source files and checkpoints remain unchanged.
Recover missing configuration from original experiment evidence before retry.
There is no automatic migration.

The strict contract applies to the training CLI.
Programmatic callers can use `Continuation` and `inspect_resume_checkpoint` before `run_training`.
Supply the resolved configuration and recovered options to that training call.

<a id="configuration-continuation-evaluation-export-and-deployment-boundary"></a>
#### Continuation, evaluation, export, and deployment boundary

The command `train --resume` continues a compatible saved Run with its configuration and compatible training state.
The CLI has no warm-start mode for a new experiment initialized from selected earlier weights.

Play and export require complete current-schema saved configuration.
They reject missing fields, unsupported schemas, and Task identity or version mismatches before Session startup.
Current defaults do not fill missing saved settings.
These restoration rules do not establish learning quality or target-scene deployment compatibility.
[Issue #8](https://github.com/zpyc1oud/EmbodiedUE/issues/8) tracks full target-map and in-game deployment validation.

<a id="configuration-timing-and-parallelism"></a>
### Timing and parallelism

CartPole uses `physics_dt=1/120` and `decimation=[2,2]`.
PhantomX uses `physics_dt=0.005` and `decimation=[1,7]`.
The range is fixed within a Session.
Each Step selects one value within that range.
This changes action duration, not the physics solver timestep.

Flat walking defaults to 512 Slots.
Continuous and discrete terrain default to 64, and Pursuit defaults to one.
A larger count can require different placement or terrain coverage.
See [PhantomX training](training.md#phantomx-robust-training) for objective scaling and curriculum behavior.

<a id="phantomx-robust-training"></a>

<a id="phantomx-robust-training-phantomx-terrain-training-and-evaluation"></a>
## PhantomX terrain training and evaluation

This guide describes the current objective for `UERL-PhantomX-ContinuousTerrain-v0`.
Flat-ground, discrete-terrain, and Pursuit Tasks use the same physical-time reward and episode rules.
The terrain-level curriculum applies to Tasks with procedural terrain.

<a id="phantomx-robust-training-before-you-train"></a>
### Before you train

Complete [installation and the small smoke test](../README.md#getting-started).
Save the UE paths in a [host profile](training.md#ue-host-profile).
Choose the commands and terrain conditions that your game needs, then inspect the resolved Task configuration.

Use [Start and resume a Run](training.md#phantomx-robust-training-start-and-resume-a-run) for the commands.
Keep the printed Run directory for evaluation and export.
Use [Per-level evaluation](training.md#phantomx-robust-training-per-level-evaluation) to measure behavior before deployment.
After evaluation, follow [in-game deployment](deployment.md#in-game-deployment-guide).

### Velocity command sampling

Set `task.command.sampling` to `staged` for the reference command curriculum or
`uniform_velocity` for independent signed forward and lateral speeds.
The uniform mode uses `initial_speed_min/max` for body-frame X and
`lateral_speed_min/max` for body-frame Y. It samples both components at reset and
at each configured `resampling_time_min_s/max_s` interval.

Set `heading_command: false` to sample yaw rate directly from `yaw_rate_min/max`.
With heading control enabled, uniform sampling uses an absolute world heading
from `heading_delta_min/max`; the heading controller computes the yaw rate.
`standing_probability` zeros all command components. Uniform sampling requires
`turn_in_place_probability: 0`. The default remains the staged reference.

Inspect the resolved configuration before training. Measure each required
command separately after training. A supported sampling mode does not establish
that a policy has learned the selected command range.

<a id="phantomx-robust-training-terrain-configuration-and-parallel-slots"></a>
### Terrain configuration and parallel Slots

[continuous.yaml](../src/uerl/configs/environments/terrains/phantomx/continuous.yaml) defines eight difficulty levels and their generation parameters.
The Task factory expands patch dimensions for episode travel and scan reach.
The file `resolved_config.yaml` records the terrain definition and `worker.slot_count`.
A count of 64 Slots does not mean 64 independently maintained terrain configurations.

Continuous terrain defaults to Slot-isolated geometry and an 8×8 placement grid for 64 Slots.
Each Slot has terrain geometry, a level, and episode state.
The option `--num-envs` changes parallelism.
Before an increase, examine collision isolation, placement, and terrain coverage.
Discrete terrain defaults to a shared terrain atlas.
These defaults come from the Task factories, not from every PhantomX Task.

The Session generates all difficulty regions during initialization.
Each control step converts body-frame commands to the world frame.
It then projects horizontal displacement onto the command direction.
Geometry does not regenerate during an episode.

At reset, promotion occurs when progress exceeds the smaller of 80% of commanded distance or half the terrain width.
Progress below half the commanded distance causes demotion by one level.
Turning paths count toward distance.
Excessive speed gives no extra promotion credit.
Promotion above the highest level causes random selection across all levels.
Termination and timeout use the same rule.

Checkpoints keep the per-Slot curriculum state and its random stream.

<a id="phantomx-robust-training-physical-time-training-objective"></a>
### Physical-time training objective

The value of `physics_dt` is **0.005 seconds**.
Each control step samples one `step_decimation` from the inclusive range `[1, 7]`.
All synchronized Slots share this sample.
Thus, control intervals are **5–35 ms**, with a fixed physics timestep.

The setting `reference_dt_s: 0.02` calibrates reward weights and PPO parameters.
It does not require a 20 ms control interval.

| Quantity | Current rule |
|---|---|
| Continuous rewards and penalties | Multiply the 20 ms reference value by `actual_step_seconds / 0.02` |
| Falls and Worker Slot faults | Charge once per event, without duration scaling |
| PPO discount and GAE | Use `gamma^(actual_step_seconds / 0.02)` and `lambda^(actual_step_seconds / 0.02)`; timeout bootstrap uses the transition discount |
| Episode limit | 20 seconds of simulation time, accumulated from completed solver steps; sparse reset clears only the selected Slots |
| Initial timeout phase | At training startup, including resume, initialize each Slot's first episode clock at a random time in `[0, 20)` seconds to stagger timeouts |
| Action-change penalty | Sum squared differences between consecutive normalized policy actions; do not divide by the control interval |
| Evaluation vibration frequency | Use the actual sampling times of completed control windows |

CartPole retains fixed-step PPO, reward, and timeout behavior.
The design uses Isaac Lab's time-integrated reward and terrain resampling concepts without an Isaac Lab runtime dependency.
Equal objective timing does not establish equal Chaos and PhysX trajectories.

<a id="phantomx-robust-training-start-and-resume-a-run"></a>
### Start and resume a Run

Run:

```powershell
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --run-name robust-time
```

Record the printed `[RUN] directory=...`.
Select that directory explicitly for evaluation.
To resume the Run, execute:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --resume $runDir
```

The command `uerl runs` lists Runs and resumable checkpoints.
A Run selects `model_final.pt` when that file exists.
Otherwise, it selects the highest-numbered `rsl_rl/model_<iteration>.pt`.
The value `latest` selects by time, not objective compatibility.

Earlier checkpoints without `phantomx_physical_time_v1` cannot resume under the physical-time objective.
The rejection occurs before UE startup.
Terrain checkpoints from before command-direction progress also lack the required adaptive terrain state.
Some earlier terrain checkpoints lack the curriculum random stream.

The command `uerl train --terrain-level N --resume ...` omits terrain-curriculum restoration and retains command-curriculum restoration.
It does not bypass the objective compatibility requirement.

Single-robot playback uses registered Task curriculum settings instead of per-Slot checkpoint state.
Use `--terrain-level N` for a fixed procedural tier.
Historical policy behavior does not establish the effectiveness of the current objective.

<a id="phantomx-robust-training-per-level-evaluation"></a>
### Per-level evaluation

Continuous-terrain levels are **0–7**.
The option `--steps` counts control steps.
With variable decimation, equal step counts can represent different physical durations.
Select the actual Run directory:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
0..7 | ForEach-Object {
  uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 `
    --run $runDir --terrain-level $_ --steps 4000 --presentation none
}
```

Compare completed episodes, survival and fall rates, velocity RMSE, and body vibration.
The metric `survival_rate` counts completed timeout episodes without base contact.
It replaces the misleading `success_rate` output name.
It does not establish command tracking.
A stationary robot can have a high survival rate.
The mean curriculum level or a video alone is also insufficient evidence.

Start with the [flat-ground baseline](https://github.com/zpyc1oud/EmbodiedUE/issues/41) before terrain training.

See [recording](training.md#record-video) and [test boundaries](../tests/README.md#test-suites).

<a id="record-video"></a>

<a id="record-video-recording-video-after-training"></a>
## Recording video after training

`uerl play --record` captures UE viewport frames and encodes an MP4 at the selected path.
Playback uses one robot.
It requires a working Unreal Editor and a trained checkpoint.

Recording requires `viewport` presentation and a newly launched Worker.
The CLI rejects other presentation modes and attached Sessions.
The default playback presentation is `viewport`.
Viewport rendering reduces throughput.
Use recordings for inspection and presentation.

<a id="record-video-record-a-policy"></a>
### Record a policy

Replace the placeholder with the Run directory printed during training.
Then run:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl play `
  --task UERL-PhantomX-ContinuousTerrain-v0 `
  --run $runDir `
  --terrain-level 0 `
  --record artifacts/walk.mp4
```

| Option | Meaning |
|---|---|
| `--task` | Optional for an explicit Run directory. If supplied, the ID must match the saved Task. Required for `--run latest`. |
| `--run` | Run directory or `latest`. Use `--checkpoint` to select a file instead. |
| `--record` | Output MP4 path |
| `--record-seconds` | Maximum duration. Default: 20 seconds. |
| `--record-fps` | Encoded frame rate. Default: 30 fps. |
| `--controller` | `task` for Task commands, or `player` for keyboard control. Default: `task`. |
| `--terrain-level` | Fixed procedural terrain tier. Without this option, playback uses registered Task curriculum settings. |

For an objective comparison, select an explicit Run directory.
The value `latest` selects by time, not by training objective.
Use [per-level evaluation](training.md#phantomx-robust-training-per-level-evaluation) to measure performance.
The recorder also writes a JSON evidence file beside the video.

<a id="record-video-keyboard-control"></a>
### Keyboard control

Run:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl play `
  --task UERL-PhantomX-ContinuousTerrain-v0 `
  --run $runDir `
  --terrain-level 0 `
  --controller player `
  --presentation viewport `
  --steps 20000
```

1. Give keyboard focus to the viewport.
2. Hold **W** for forward motion.
3. During forward motion, use **A/D** or **Q/E** to turn.
4. Release W or press **S** for the default standing joint targets.

The supplied Task commands teach forward motion and turns during motion.
Historical policies do not establish reverse motion, lateral motion, or turns without forward motion.
Those behaviors require suitable training and evaluation.

Without `--controller player`, the Task publishes its own commands.

<a id="record-video-historical-examples"></a>
### Historical examples

- [Walking](media/phantomx-walk.mp4): one robot on continuous terrain, 20 seconds.
- [Terrain](media/phantomx-terrain.mp4): discrete box terrain, 20 seconds.

