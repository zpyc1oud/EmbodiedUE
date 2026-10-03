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

### Current limitations

A missing snapshot still uses current Task defaults. Missing dataclass fields in
an existing snapshot still use the current template; unknown fields, invalid
field types, and a different Task identity are rejected. These legacy fallback
rules do not establish faithful continuation for incomplete runs.

`train --resume` still selects a checkpoint while building configuration from
current Task defaults and explicit overrides, through the same application
resolver's defaults path. It does **not** restore the source run snapshot.
Objective checks and checkpoint curriculum/random-state restoration remain in
the training runner. Strict continuation metadata requirements, allowed semantic
overrides, warm-start intent, and configuration differences remain tracked in
[Issue #7](https://github.com/zpyc1oud/EmbodiedUE/issues/7).

## Timing and parallelism

CartPole uses `physics_dt=1/120` and `decimation=[2,2]`. PhantomX uses `physics_dt=0.005` and `decimation=[1,7]`. The range is immutable within a Session; each Step specifies one value in that range. This varies how long an action is held, not the physics solver timestep.

Flat walking defaults to 512 Slots, continuous and discrete terrain to 64, and pursuit to one. Increasing the count may require changes to placement and terrain coverage. See [PhantomX training](how-to/phantomx-robust-training.md) for objective scaling and curriculum behavior.
