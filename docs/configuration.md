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

## Timing and parallelism

CartPole uses `physics_dt=1/120` and `decimation=[2,2]`. PhantomX uses `physics_dt=0.005` and `decimation=[1,7]`. The range is immutable within a Session; each Step specifies one value in that range. This varies how long an action is held, not the physics solver timestep.

Flat walking defaults to 512 Slots, continuous and discrete terrain to 64, and pursuit to one. Increasing the count may require changes to placement and terrain coverage. See [PhantomX training](how-to/phantomx-robust-training.md) for objective scaling and curriculum behavior.
