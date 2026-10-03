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
| `configs/tasks/*/training.yaml` | Base task, Worker, and runner settings |
| `src/uerl/tasks/*/registration.py` | Task-specific factories and defaults |
| `configs/environments/terrains/phantomx/*.yaml` | Terrain tiers and generation parameters |
| Run `resolved_config.json` and manifest | The configuration and identities recorded for an actual run |

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

Changing dimensions, timing, robot semantics, or the training objective can make an existing checkpoint incompatible. `play --run` and `export --run` load recorded settings from the Run. Prefer Run directories over detached weight files when reproducing results.

## Saved-run configuration

`play --run` and `export --run` use the shared application resolver in
[`application/run_config.py`](../src/uerl/application/run_config.py). It restores
Worker, Task, and runner settings from `resolved_config.json`, retains the saved
training hash, and validates explicit overrides through the typed configuration
resolver. Session endpoints and logging start from the current registration.
The saved file is not edited.

Map selection is `--map`, then `--session.map_path`, then the recorded map, then
the registered Task map. The selected map is used both in Worker launch arguments
and the Session configuration. Existing CLI commands and flags are unchanged.
Python callers of the former `uerl.cli.run_config` module should import
`resolve_run_config` from `uerl.application.run_config` instead; map restoration
is included in the returned configuration.

## Continue training

`train --resume` restores the source Run's Worker, Task, runner, map and protocol
settings, then applies permitted machine, output and budget changes. A Run
reference, `latest`, or a checkpoint within the Run root, `rsl_rl/`, or
`checkpoints/` is supported. A detached checkpoint needs its original
`resolved_config.json`; weights alone cannot establish the training semantics.
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
The destination must be empty or new, outside the source Run.

Changes to Slot count, seed, map, rewards, timing, Robot/terrain semantics,
network structure or PPO settings are rejected even when tensor dimensions
would still match. Repeating a saved value is accepted. Machine overrides do
not establish that a different UE installation or project has equivalent assets.

### Checkpoint state and old Runs

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

Complete, compatible old Run snapshots remain supported. Missing semantic
fields (including required runner parameter keys), a different Task/version,
or missing required checkpoint state fail before Session startup. Continuation
never fills those gaps with current Task defaults. For legacy runtime options:

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

`play --run` and `export --run` retain their existing compatibility rules: a missing
snapshot uses current defaults, and missing dataclass fields use the current
template. Unknown fields, invalid field types and a different Task identity are
rejected. These fallbacks do not establish faithful recovery of incomplete runs.
Broader run intent, warm start and deployment validation remain tracked in
[Issue #7](https://github.com/zpyc1oud/EmbodiedUE/issues/7).

## Timing and parallelism

CartPole uses `physics_dt=1/120` and `decimation=[2,2]`. PhantomX uses `physics_dt=0.005` and `decimation=[1,7]`. The range is immutable within a Session; each Step specifies one value in that range. This varies how long an action is held, not the physics solver timestep.

Flat walking defaults to 512 Slots, continuous and discrete terrain to 64, and pursuit to one. Increasing the count may require changes to placement and terrain coverage. See [PhantomX training](how-to/phantomx-robust-training.md) for objective scaling and curriculum behavior.
