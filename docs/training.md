# Training and robot integration

[Project home](../README.md)

## Contents

- [Reuse a host profile](#ue-host-profile)
- [Task configuration](#configuration)
- [Adding a robot](#add-a-robot)
- [Install an external Task](#external-tasks)
- [PhantomX terrain training and evaluation](#phantomx-robust-training)
- [Recording video after training](#record-video)
- [Troubleshooting](#troubleshooting)


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

In launch mode, `train`, `play`, and `export` use these same resolution rules.
After static preflight passes, run:

```powershell
uv run uerl train --task UERL-CartPole-Direct-v0 --num-envs 2 --max-iterations 1 --device cpu
$runDir = 'runs/UERL-CartPole-Direct-v0/<run-directory>'
uv run uerl play --task UERL-CartPole-Direct-v0 --run $runDir
uv run uerl export --task UERL-CartPole-Direct-v0 --run $runDir
```

All three commands accept `--host-profile`, `--ue-executable`, and `--project`.
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
| `.uproject` JSON and explicit plugin enablement | Use the bundled host structure. Enable UERLEngine, ProceduralMeshComponent, and NNERuntimeORT. |
| Plugin descriptor JSON | Restore the bundled UERLEngine plugin or install the missing engine dependency. |
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

For a new behavior, follow [external Tasks](training.md#external-tasks).
For a new physical robot, follow [add a robot](training.md#add-a-robot).

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

The separate migration command previews a new recovery copy by default.
It accepts historical JSON or unversioned YAML configuration, or a JSON/YAML manifest with resolved configuration.
Every saved field and the exact registered Task version are required.
Current defaults never fill missing values.

On apply, migration writes current `resolved_config.yaml` and converts an existing manifest.
It copies source configuration and manifest files into `legacy/`.
It copies the selected checkpoint byte-for-byte without loading or changing it.
The source Run stays unchanged.

The default recovery directory uses a configuration sidecar.
Its checkpoint is not self-contained.
The copy operation does not establish that detached play or export can load it.

```powershell
uv run python -m uerl.application.run_migration `
  --source runs/old-run --output runs/recovered-run

# Review the [PLAN] entries, then run the same command with --apply.
uv run python -m uerl.application.run_migration `
  --source runs/old-run --output runs/recovered-run --apply

uv run uerl play --run runs/recovered-run
```

The output must not contain different files already.
Repeated apply to an identical output makes no changes.
Incomplete configuration, conflicts, or an installed Task-version mismatch stop migration before writes.

Recover missing values from the original command, exact Task package and revision, archived configuration, and experiment records.
Then retry with a complete snapshot.
If the values are unavailable, train and evaluate a new Run.
For a detached historical checkpoint, create a small source Run with its complete historical metadata.
Use the same migration command on that directory.

For a weights-only-compatible checkpoint, `--embed-checkpoint-config` creates a copy with migrated YAML inside it.
Preview this mode separately.
Examine the plan before apply:

```powershell
uv run python -m uerl.application.run_migration `
  --source runs/old-run --output runs/recovered-self-contained `
  --embed-checkpoint-config

uv run python -m uerl.application.run_migration `
  --source runs/old-run --output runs/recovered-self-contained `
  --embed-checkpoint-config --apply
```

This mode uses only `weights_only=True`, without a pickle fallback.
It rejects unsupported state.
After saving, it verifies that model, optimizer, iteration, and all other loaded data are unchanged.
The recovery copy keeps original checkpoint bytes under `legacy_checkpoint/`.
The source Run remains unchanged.

If safe inspection or verification fails, sidecar migration can keep the configuration but cannot make that checkpoint safely readable.
Keep its Run metadata.
Recover a weights-only-compatible checkpoint or train a new Run.
Do not treat the copied checkpoint as self-contained.

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

<a id="add-a-robot"></a>

<a id="add-a-robot-adding-a-robot"></a>
## Adding a robot

A robot integration contains UE assets, a Python robot declaration, and a Python Task.
Supported robots share the existing UE runtime, action schema, and reset path.
CartPole and PhantomX are the reference integrations.
See [architecture](architecture.md#architecture-robot-integration) for the supported boundaries.

<a id="add-a-robot-before-you-start"></a>
### Before you start

Complete the [host setup](../README.md#getting-started).
Prepare a robot asset with body and joint names that you can match to its Python declaration.
For a game-deployable Task, choose an exportable observation and action path before training.
See [external Tasks](training.md#external-tasks) if you want to keep your Task in a separate package.

The result of this guide is a registered integration ready for training and physical validation.
Generating files alone does not validate the robot's dynamics.

<a id="add-a-robot-create-starter-files"></a>
### Create starter files

To create starter files, run:

```powershell
uv run uerl new robot phantomx2 --asset /Game/Robots/PhantomX2/SK_PhantomX2
uv run uerl new task walk2 --robot phantomx2 --template phantomx-walk
```

The generator rejects existing output files.
It does not change the explicit registry.
Complete the generated `TODO` topology and Task mathematics before registration.
The value `max_episode_steps: 1000` is a placeholder.

For variable-decimation locomotion, use physical time for rewards, discounts, and episode duration.
See the [PhantomX guide](training.md#phantomx-robust-training).

<a id="add-a-robot-1-ue-assets"></a>
### 1. UE assets

1. Add a Skeletal Mesh, Skeleton, and PhysicsAsset to the host project.
2. Make sure that their topology meets the runtime requirements.
3. Obtain redistribution permission before you contribute meshes, textures, or physics assets.

The runtime reflects structure from these assets.
Python does not duplicate mass, inertia, geometry, or joint limits.
The existing PhantomX mesh is `/Game/Robots/PhantomX/SK_PhantomX`.

<a id="add-a-robot-2-python-robot-declaration"></a>
### 2. Python robot declaration

Add a declaration under `src/uerl/assets/robots/`.
Include the asset path, joint and body names, reference pose, actuator groups, observations, and reset distributions.
Register it in `ROBOT_ASSETS` in `src/uerl/assets/robots/__init__.py`.

Use [cartpole.py](../src/uerl/assets/robots/cartpole.py) and [phantomx.py](../src/uerl/assets/robots/phantomx.py) as examples.

<a id="add-a-robot-3-python-task"></a>
### 3. Python Task

1. Add the Task configuration and builder under `src/uerl/tasks/`.
2. Register the Task in `src/uerl/tasks/registry/defaults.py`.
3. Run `uerl tasks`.
4. Run `uerl check task <TaskID>`.
5. Do a real UE smoke test.

The registered ID is available to `uerl train`, `uerl play`, and `uerl export`.
Use the existing [CartPole](../src/uerl/tasks/cartpole) and [PhantomX](../src/uerl/tasks/phantomx) implementations as references.
Python preflight does not establish correct asset reflection or Chaos behavior.

<a id="add-a-robot-4-train-and-deploy"></a>
### 4. Train and deploy

Train through the same CLI.
Export a `.uerlpol2` artifact.
Compare its observation and action dimensions, topology, and timing with the intended deployment mesh.
Use the [deployment guide](deployment.md#in-game-deployment-guide) and applicable [test layers](../tests/README.md#test-suites).

<a id="external-tasks"></a>

<a id="external-tasks-install-an-external-task"></a>
## Install an external Task

An installed Python package can supply a Task without changes to framework registration.
The [CartPole example](../examples/external-cartpole) changes one reward weight.
It uses the existing CartPole Task factory, Robot declaration, observations, actions, and runtime.

<a id="external-tasks-install-and-enable-the-example"></a>
### Install and enable the example

First install the repository's Python dependencies.
Then run these PowerShell commands:

```powershell
uv pip install ./examples/external-cartpole --no-deps
$env:UERL_TASK_PLUGINS = 'example-cartpole'
uerl tasks
uerl check task Example-CartPole-v0
uerl config --task Example-CartPole-v0 --json
```

After installation, the inspection commands work outside the checkout.
The command `check task` validates Python configuration and Task construction without UE startup.

For your own package:

1. Copy `examples/external-cartpole` outside the repository.
2. Change the package name, entry-point name, and Task ID.
3. Install that directory into the prepared environment.

For editable development, use `uv pip install -e <directory> --no-deps`.
To change `pole_position_weight`, edit `src/example_cartpole/reward.yaml`.
For a non-editable installation, reinstall after a source change.

Alternatively, create a package with the CLI:

```powershell
uv run uerl new external-cartpole balance-demo --output-dir ..\balance-demo
Set-Location ..\balance-demo
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'balance-demo'
uerl tasks --filter UERL-BalanceDemo-v0
uerl check task UERL-BalanceDemo-v0
uerl config --task UERL-BalanceDemo-v0 --json
```

The generated project contains entry-point metadata, a reward resource, registration tests, and usage instructions.
It uses the framework CartPole Task.
Its only Task change is `pole_position_weight` in `src/balance_demo/reward.yaml`.
Run its tests with `python -m pytest`.
After a non-editable source change, reinstall the package.

On a configured Windows/UE host, use the same registration for training:

```powershell
uerl train --task Example-CartPole-v0 --num-envs 2 --max-iterations 1 --device cpu
```

Supply host paths as specified in [Getting started](../README.md#getting-started).
The Python wheel does not contain UE assets, the host project, or UE binaries.

<a id="external-tasks-create-a-directtask-python-hooks-package"></a>
### Create a DirectTask Python-hooks package

The [Direct CartPole example](../examples/external-direct-cartpole) owns its
action, observation, reward, and termination functions in a `DirectTask` subclass.
It still uses the framework's `DirectEnv`, Session, reset, and valid-Slot path.
Its package-owned reward override is YAML, as are Task and Run configurations.

To generate another package from the CLI:

```powershell
uv run uerl new direct-cartpole direct-balance-demo --output-dir ..\direct-balance-demo
Set-Location ..\direct-balance-demo
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'direct-balance-demo'
uerl tasks --filter UERL-DirectBalanceDemo-v0
uerl check task UERL-DirectBalanceDemo-v0 --json
```

`check task --json` writes a machine-readable preflight report to stdout; it is
not a configuration file or a configuration input. The report shows whether
train, evaluate, and export are supported, unsupported, or unknown, with reasons.
For a Python-hooks Task, train/evaluate are supported and export is unsupported
because Python action/observation methods have no Manager-generated plans. Do
not infer mathematical or cross-language exportability from Task metadata.

<a id="external-tasks-registration-contract"></a>
### Registration contract

Declare a named entry point in the package's `pyproject.toml`:

```toml
[project.entry-points."uerl.tasks"]
my-task = "my_package:create_registration"
```

The target must be a zero-argument callable that returns one `uerl.tasks.registry.TaskRegistration`.
Its factories supply fresh Task, Worker, and runner configurations and a `DirectTask`.
Optional curriculum, event, and evaluation factories remain available.

Each Task owns its Python resources.
Use `importlib.resources` to load them.
Do not locate them through the current directory.

Enable entry-point names with the comma-separated `UERL_TASK_PLUGINS` environment variable.
Installation alone does not enable a package.
All CLI commands use this selection.
Python callers can use `create_default_registry(external_tasks=("my-task",))`.
An explicit `()` selects built-in Tasks only and overrides the environment variable.
Only enable trusted packages, because package loading executes their Python code.

Task IDs must be unique across built-in Tasks and enabled packages, including different versions.
Entry-point names must be unique among installed packages.
Missing names, import failures, invalid callables, invalid registration objects, and invalid factories cause `RegistryError`.
The error identifies the entry-point source.
A failed construction does not return a partial registry.
Keep the same package and selection available for saved Runs.

Registration does not require Manager composition.
An external factory can return a `DirectTask` subclass or a composed Task.
Both use DirectEnv, Session, reset, and valid-Slot behavior.
This example uses the existing composed CartPole factory without a new simulation loop.

<a id="external-tasks-simple-and-manager-task-implementations"></a>
### Simple and Manager Task implementations

An external factory can return either a `DirectTask` subclass that implements
`preprocess_actions`, `build_observations`, `compute_terminations` (or
`termination_terms`), and `compute_rewards`, or a `DirectTask` assembled from
action, observation, termination, and reward Managers. `DirectTask`,
`DirectTaskCfg`, `UERLDirectEnv`, and the capability report types are public
Python APIs. Both Task styles use the same `DirectEnv` step/reset path; Task code
does not own Session calls, sparse reset, or invalid-Slot compaction.

Inspect a constructed Task with `task.capabilities`. Training and evaluation are
supported when the required DirectTask methods or Manager declarations exist.
Manager Tasks report `unknown` until a Worker RobotSpec has been bound and their
plans have been assembled. A Python-only Task can train and evaluate, but its
Python observation/action math has no exported plan and reports export as
`unsupported` with the reason. The export service stops such a Task before
calling the ONNX exporter.

Training and evaluation report capabilities after Worker schema binding and
before their long loop. A known unsupported capability fails before Worker
startup; Manager `unknown` is allowed to reach RobotSpec binding and then must
resolve before the loop starts. Export rejects known unsupported work before
Worker startup, then checks strictly after DirectEnv initialization and its
initial reset, before vector-runner construction or checkpoint loading. It
proceeds only when the resolved report says `supported`; both `unsupported` and
`unknown` stop first. If a Task adds Python action or
observation behavior around Manager plans, export reports `unknown`: the
framework does not infer mathematical equivalence from registration metadata.
Resolve the implementation into a supported plan path before export, then
complete the separate artifact, UE, and target-scene checks for the behavior you
intend to ship.

<a id="external-tasks-built-in-resources"></a>
### Built-in resources

Built-in training and terrain YAML files are under `src/uerl/configs/`.
Wheels and source distributions include these files.
The existing loaders read this installed location.
Logical resource IDs, such as `environments/terrains/cartpole/flat.yaml`, remain unchanged.

Use Task loaders instead of the old checkout path `configs/tasks/...`.
The function `repository_config_root()` now returns the package resource root for standard wheel and editable installations.
Path-based loaders do not support direct import from a zipped wheel.

<a id="external-tasks-validation-boundary"></a>
### Validation boundary

The packaging test builds and installs the framework, checked-in examples, and
generated Manager and Direct package wheels into an isolated target. It checks
entry-point discovery, YAML resource loading, Task construction, and CLI
capability reports outside the checkout, then executes one scripted DirectEnv
step for each installed Direct package outside the checkout. The Direct/Manager
behavior test uses the checked-in Direct Task implementation, a Manager
registration, a scripted Session, and independent numeric expectations for
actions, observations, rewards, termination, reset masks, and invalid Slots.
These are Python checks; they do not validate UE execution, Chaos behavior,
training quality, or game deployment. Those remain separate Issue #6 acceptance
gates.

See [Write tests](../tests/README.md#write-tests-write-package-and-generator-tests) for the test procedure.
[Issue #6](https://github.com/zpyc1oud/EmbodiedUE/issues/6) tracks the remaining external Task acceptance.

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
| Task lookup or configuration fails | [Task ID and overrides](training.md#troubleshooting-unknown-task-id-or-override) |
| UE does not start | [Executable path](training.md#troubleshooting-ue-executable-not-found), [host plugins](training.md#troubleshooting-missing-host-plugin) |
| Content cannot load | [UE assets](training.md#troubleshooting-missing-or-invalid-ue-assets) |
| Policy device cannot initialize | [CUDA](training.md#troubleshooting-cuda-unavailable) |
| An old Run cannot be restored | [Checkpoint compatibility](training.md#troubleshooting-old-phantomx-checkpoint-cannot-resume), [saved configuration](training.md#troubleshooting-missing-or-conflicting-saved-configuration) |
| Evaluation reports success but the robot barely moves | [Motion and success metrics](training.md#troubleshooting-success-result-with-little-robot-motion) |
| The game rejects a policy at startup | [Physics gate](training.md#troubleshooting-deployment-physics-gate-failure), [RobotMesh](training.md#troubleshooting-imported-artifact-has-no-robotmesh) |
| Packaging fails | [Game Target](training.md#troubleshooting-packaged-game-cannot-find-uerlinterface) |

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

Make sure that the bundled `UERLEngine` source is present.
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
uv run python scripts/diagnose_training_trace.py runs/example/traces/task.yaml
uv run python scripts/diagnose_training_trace.py runs/example/traces/task.yaml --events runs/example/logs --last-iterations 100
```

Pass the actual TensorBoard directory to `--events`. Omit it if no events exist.
Use `--body` and `--command` for a different root body or velocity-command channel.
The body must have pose, linear velocity and angular velocity fields in the trace.
The command must contain body-frame forward speed, lateral speed and yaw rate.

The output is YAML. It reports:

- Physical-time-weighted planar velocity and yaw-rate RMSE
- Planar path length and measured displacement for each episode
- Action RMS and successive-action change within an episode
- Recorded force sample counts and maximum force for each available body
- Completed termination counts and the sum of transition rewards
- Optional TensorBoard scalar windows, including recorded reward components

The position calculation uses the decision input and pre-reset transition.
It excludes reset teleports. A report can include a partial episode.
Action changes do not cross reset boundaries or gaps in recorded policy steps.
The report preserves the source trace's completion status.

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
uv run python scripts/check_training_debug.py runs/debug/steps.yaml
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
