# Task configuration

Use `uerl config` to inspect the resolved configuration for a registered task. Base YAML alone is insufficient: terrain and pursuit factories override parts of the base PhantomX configuration.

```powershell
uv run uerl tasks --filter PhantomX
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0
uv run uerl check task UERL-PhantomX-ContinuousTerrain-v0
```

These commands do not start UE. A successful task check validates declarations, not physical asset contents or runtime performance.

## Sources of truth

| Source | Owns |
|---|---|
| UE Skeletal Mesh / Skeleton / PhysicsAsset | Bodies, joints, hierarchy, mass, inertia, geometry, and limits |
| `src/uerl/assets/robots/*.py` | Asset references, actuator semantics, gains, reference poses, observation selection, and reset distributions |
| `src/uerl/configs/tasks/*/training.yaml` | Base task, Worker, and runner settings |
| `src/uerl/tasks/*/registration.py` | Task-specific factories and defaults |
| `src/uerl/configs/environments/terrains/phantomx/*.yaml` | Terrain tiers and generation parameters |
| Run `resolved_config.yaml` and `manifest.yaml` | The configuration and identities recorded for an actual run |

Robot declarations support derivation and regular-expression joint selection. Runtime Worker projections are generated from those declarations and reflected topology; they are not a second configuration to edit.

## Overrides

Everyday training flags map to typed configuration fields:

| Flag | Field |
|---|---|
| `--num-envs` | `worker.slot_count` |
| `--seed` | `worker.run_seed` |
| `--device` | `runner.device` |
| `--max-iterations` | `runner.max_iterations` |

`play` exposes `--seed` and `--device`; `export` exposes `--device`. Other configurable values use repeated dotted-path arguments. For example, inspect a shorter training configuration before running it:

```powershell
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0 --runner.max_iterations 100
```

Unknown or non-configurable paths are rejected. Do not combine an everyday flag with a dotted override for the same field. `--run-dir` conflicts with `--logging.run_directory`, and `--resume` conflicts with `--runner.checkpoint`.

Changing dimensions, timing, robot semantics, or the training objective can make an existing checkpoint incompatible. `play` and `export` restore recorded settings from the Run. Prefer Run directories or checkpoints kept inside their Run over detached weight files when reproducing results.

## Saved-run configuration

`play --run` and `export --run` use the shared application resolver in
[`application/run_config.py`](../src/uerl/application/run_config.py). They read
the Task ID from `resolved_config.yaml`, the checkpoint's embedded config, or
the Run manifest, then restore the saved Worker, Task, runner, map, and protocol
settings and retain the saved training hash. `--task` is
optional for an explicit Run directory; when supplied, it must match the saved
Task. `--run latest` still requires `--task` because latest Runs are selected
within a Task. A `--checkpoint` located under a directory containing saved Run
config or manifest also identifies its Run automatically. A checkpoint copied
outside its Run can restore Task identity and settings from its embedded
versioned YAML config. When checkpoint and Run metadata both exist, their typed
config values must agree. Saved files are not edited.

Play and export require a complete saved configuration and the same registered
Task version that trained the checkpoint. A missing snapshot, missing required
field, or Task version change fails before Session startup; current Task defaults
are never used to fill those gaps. Evaluation and export preserve the saved Task,
reward, timing, Robot, plan, and PPO settings. Play allows an explicit `--seed`
and both commands allow `--device`, one Slot, map, Session connection/launch,
and presentation settings as evaluation or local-machine choices. Dotted
overrides that change the saved training semantics are rejected.

Map selection is `--map`, then `--session.map_path`, then the recorded map. The
selected map is used both in Worker launch arguments and the Session
configuration. Python callers of the former `uerl.cli.run_config` module should import
`resolve_run_config` from `uerl.application.run_config` instead; map restoration
is included in the returned configuration.

### Recovering historical Runs and detached checkpoints

The current reader accepts schema version 1 YAML in `resolved_config.yaml`, the
same versioned YAML embedded in new RSL-RL checkpoints, and the current YAML Run
manifest. Embedded checkpoint metadata is read through PyTorch's weights-only
loader. Historical JSON files and earlier YAML sidecar schemas are not read by
training, play, or export.

The one-time migration command previews a new recovery copy by default. It
accepts a historical JSON or unversioned YAML config, or a JSON/YAML manifest
that contains the resolved config. It requires every saved field and the exact
registered Task version; it never fills gaps from current defaults. On apply,
it writes current `resolved_config.yaml`, converts a manifest when present,
copies the source config/manifest into `legacy/`, and copies the selected
checkpoint byte-for-byte without loading or changing it. The source Run stays
untouched. This default creates a recovery directory whose sidecar supplies
the config; it does not make the checkpoint self-contained or certify that it
is loadable for detached play/export.

```powershell
uv run python -m uerl.application.run_migration `
  --source runs/old-run --output runs/recovered-run

# Review the [PLAN] entries, then run the same command with --apply.
uv run python -m uerl.application.run_migration `
  --source runs/old-run --output runs/recovered-run --apply

uv run uerl play --run runs/recovered-run
```

The output must not already contain different files. A repeated apply to an
identical output is a no-op. If configuration is incomplete, conflicting, or
does not match an installed Task version, the command stops without writing.
Recover missing values from the original command, exact Task package and source
revision, archived configuration, and experiment records, then retry with a
complete snapshot. If those values cannot be established, retrain and evaluate
a new Run. A detached checkpoint can be handled by making a small recovery Run
directory containing the checkpoint and the complete historical metadata, then
running the same migration command.

For a weights-only-compatible checkpoint, the explicit
`--embed-checkpoint-config` option creates a checkpoint copy with the migrated
YAML embedded. Preview this separately, then apply only after reviewing the
plan:

```powershell
uv run python -m uerl.application.run_migration `
  --source runs/old-run --output runs/recovered-self-contained `
  --embed-checkpoint-config

uv run python -m uerl.application.run_migration `
  --source runs/old-run --output runs/recovered-self-contained `
  --embed-checkpoint-config --apply
```

This mode only uses PyTorch's `weights_only=True` loader; there is no pickle
fallback. It rejects unsupported state and verifies that model, optimizer,
iteration, and all other loaded checkpoint data remain unchanged after saving.
The original checkpoint bytes are retained under `legacy_checkpoint/` in the
recovery copy, and the source Run is left untouched. If the checkpoint cannot
be safely inspected or verified, sidecar migration can preserve the config but
cannot make that checkpoint readable by the current safe loader. Keep the Run
metadata with it and recover a weights-only-compatible checkpoint, or retrain;
do not treat the copied checkpoint as self-contained.

## Continue training

`train --resume` restores the source Run's Worker, Task, runner, map and protocol
settings, then applies permitted machine, output and budget changes. A Run
reference, `latest`, or a checkpoint within the Run root, `rsl_rl/`, or
`checkpoints/` is supported. A new detached checkpoint carries its config
snapshot; a historical detached checkpoint needs a complete historical config
or manifest copied into a small source directory and migrated into a separate
recovery directory because weights alone cannot establish the training
semantics.
The dotted `--runner.checkpoint` option follows the same continuation rules.

```powershell
uv run uerl train --task UERL-CartPole-Direct-v0 --resume runs/cartpole_source --run-dir runs/cartpole_continued --max-iterations 100
```

`--max-iterations` is an **additional** iteration budget, not an absolute target.
The source is captured once and reused when launch arguments and the final
configuration are assembled. `[CONFIG]` lines show changed field paths, saved
and effective values, and whether each change came from explicit overrides,
launch settings, local defaults, or generated output. The new snapshot and
`command.txt` retain the effective configuration and selected checkpoint.

Allowed changes are Session launch/attach mode, executable, host, port, timeouts,
presentation, runner device, output directory, iteration budget, and runner
`run_name`, `experiment_name` and `save_interval`. The project and window size
are supplied through the ordinary launch flags. Arbitrary
`--session.worker_args` changes are rejected; use the launch flags instead.
When supplied directly as a dotted override, `session.worker_args` is a YAML
sequence of strings; legacy JSON string arrays remain valid YAML input.
The destination must be empty or new, outside the source Run.

Changes to Slot count, seed, map, rewards, timing, Robot/terrain semantics,
network structure or PPO settings are rejected even when tensor dimensions
would still match. Repeating a saved value is accepted. Machine overrides do
not establish that a different UE installation or project has equivalent assets.

### Checkpoint state and old Runs

Only load checkpoints from trusted sources. Checkpoint loading uses
`torch.load(..., weights_only=False)` to recover Python training state; pickle
can execute code during deserialization. Empty, truncated, invalid or Git LFS
pointer files produce a checkpoint-path error before Session startup. Restore a
complete trusted checkpoint, including the actual LFS content when applicable,
and retry. This error handling does not make untrusted checkpoints safe.

The existing runner loader restores actor and critic state (including enabled
observation-normalization statistics and actor distribution parameters), PPO
optimizer state and iteration, named curriculum state, the saved decimation RNG,
and RND state when enabled. Episodes start fresh. UE physical state, rollout
buffers, event timers and the complete set of process RNG states are not saved;
continuation is not a promise of an uninterrupted identical trajectory.
The PhantomX physical-time objective check remains required.

New training checkpoints record `terrain_level` and
`freeze_observation_normalization` in `infos.uerl_training_options`; continuation
recovers them automatically. `[RESTORE]` prints the selected state policy.

Complete current-schema Run snapshots can resume when their checkpoint state is
compatible. Historical JSON and earlier YAML schemas must first be migrated.
Missing semantic fields (including required runner parameter keys), a different
Task/version, or missing required checkpoint state fail before Session startup.
Continuation never fills those gaps with current Task defaults. For saved
runtime options:

- A saved terrain curriculum term identifies adaptive terrain. Without that
  term, a Run with terrain requires its original explicit `--terrain-level N`.
- A Run with observation normalization enabled but no recorded update policy
  requires `--resume-normalization update` or `--resume-normalization frozen`.
  Supply the policy actually used in that Run; the checkpoint's statistics alone
  do not reveal whether updates were frozen.
- Explicit recovery inputs must match recorded options when those are present.
  The next checkpoint persists the recovered options.
- Variable decimation requires its saved generator state. With fixed decimation,
  a missing unused sampler state is reported and accepted.

Neither source files nor checkpoints are rewritten. Recover missing configuration
from original experiment evidence before retrying; there is no automatic migration
or warm-start mode. The strict contract applies to the training CLI. Programmatic
callers can use `Continuation` and `inspect_resume_checkpoint` before passing the
resolved configuration and recovered options to `run_training`.

### Evaluation and export limitations

`play` and `export` require a complete current-schema saved configuration. They
reject missing fields, unsupported schema versions, and a Task identity or
version mismatch before Session startup. Current defaults never fill missing
saved settings. Broader run intent, warm start and deployment validation remain
tracked in [Issue #7](https://github.com/zpyc1oud/EmbodiedUE/issues/7).

## Timing and parallelism

CartPole uses `physics_dt=1/120` and `decimation=[2,2]`. PhantomX uses `physics_dt=0.005` and `decimation=[1,7]`. The range is immutable within a Session; each Step specifies one value in that range. This varies how long an action is held, not the physics solver timestep.

Flat walking defaults to 512 Slots, continuous and discrete terrain to 64, and pursuit to one. Increasing the count may require changes to placement and terrain coverage. See [PhantomX training](how-to/phantomx-robust-training.md) for objective scaling and curriculum behavior.
