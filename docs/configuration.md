# Task configuration

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

## Sources of truth

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

## Overrides

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

## Saved-run configuration

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

### Recovering historical Runs and detached checkpoints

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

## Continue training

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

### Checkpoint state and old Runs

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

### Continuation, evaluation, export, and deployment boundary

The command `train --resume` continues a compatible saved Run with its configuration and compatible training state.
The CLI has no warm-start mode for a new experiment initialized from selected earlier weights.

Play and export require complete current-schema saved configuration.
They reject missing fields, unsupported schemas, and Task identity or version mismatches before Session startup.
Current defaults do not fill missing saved settings.
These restoration rules do not establish learning quality or target-scene deployment compatibility.
[Issue #8](https://github.com/zpyc1oud/EmbodiedUE/issues/8) tracks full target-map and in-game deployment validation.

## Timing and parallelism

CartPole uses `physics_dt=1/120` and `decimation=[2,2]`.
PhantomX uses `physics_dt=0.005` and `decimation=[1,7]`.
The range is fixed within a Session.
Each Step selects one value within that range.
This changes action duration, not the physics solver timestep.

Flat walking defaults to 512 Slots.
Continuous and discrete terrain default to 64, and Pursuit defaults to one.
A larger count can require different placement or terrain coverage.
See [PhantomX training](how-to/phantomx-robust-training.md) for objective scaling and curriculum behavior.
