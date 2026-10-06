# EmbodiedUE · UE RL Engine

Train robot control policies in **Unreal Engine 5.8 / Chaos**.
Then run the exported policies on Skeletal Mesh robots in a UE game.
Python owns the Task and PPO training.
The UE plugin owns physics, observations, and in-game inference.

The project includes CartPole and PhantomX hexapod Tasks.
It uses **rsl-rl**.
Python training and C++ deployment share observation and action plans.
Isaac Sim and Isaac Lab are not runtime dependencies.

<video src="docs/media/phantomx-walk.mp4" controls muted playsinline width="720"></video>

[Watch the walking demo](docs/media/phantomx-walk.mp4) · [Terrain demo](docs/media/phantomx-terrain.mp4)

These videos show sample policies.
They do not guarantee the performance of a new model.

## Project status: Early-Stage

EmbodiedUE is an early-stage project.
APIs, artifact formats, and procedures can change.
The project does not guarantee backward compatibility or readiness for production.
Python checks do not establish UE runtime compatibility.
The release review still requires build, training, import, and packaging validation on a clean Windows/UE host.

The Egypt gameplay scene is an [optional Fab installation](docs/how-to/optional-egypt-demo.md).
Acquire [Stylized Egypt](https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd)
through your own account.
The listing displayed Free on 2026-10-03.
Its source assets are not in this tree.
The default game map is the empty Engine Entry map.
For gameplay, open the installed Egypt map explicitly.
CartPole and procedural-terrain Task maps are unchanged.

## What is included

- Request-driven physics stepping and batched robot instances in a UE world.
- CartPole balancing and PhantomX walking, terrain, and pursuit tasks.
- Fixed physics steps with variable control intervals for PhantomX: 5 ms physics steps and 1–7 steps per action.
- Training, checkpoint inspection, playback, recording, export, and deployment through `uerl`.
- `.uerlpol2` artifacts containing observation/action plans, timing, robot metadata, and an ONNX policy for UE NNE inference.
- Python, protocol, cross-language parity, UE Automation, and end-to-end tests.

## Getting started

### Prerequisites

The integrated training and deployment procedure targets **Windows x64 and UE 5.8**.
The documented procedure does not validate other UE versions or Linux/macOS UE execution.
Python-only inspection and tests can run without UE.

Before you use these commands, install the required tools:

| Requirement | Purpose |
|---|---|
| Git and Git LFS | Check out source and retrieve `.uasset` / `.umap` content |
| Unreal Engine 5.8 and its Windows C++ build toolchain | Build `UERLHostEditor Win64 Development` and run the Chaos Worker |
| Python 3.11 and `uv` | Install the Python package and locked dependencies |
| NVIDIA GPU with a driver compatible with the pinned CUDA 12.8 PyTorch build | Default PhantomX policy training on `cuda:0`; `--device cpu` selects CPU policy execution |

Chaos simulation runs in UE.
CPU policy execution still requires UE.
Use the C++ compiler and Windows SDK accepted by your UE 5.8 installation.
This repository does not pin their versions.
The project pins `torch==2.11.0+cu128`, `torchvision==0.26.0+cu128`, and `rsl-rl-lib==5.4.2`.
The project has no established minimum RAM, GPU memory, or throughput guarantee.

The host enables the bundled `UERLEngine` plugin and its engine dependencies, `ProceduralMeshComponent` and `NNERuntimeORT`.
Before the build, make sure that these engine plugins are available.
See [troubleshooting](docs/troubleshooting.md).

### Install and build

Replace the placeholder paths with your paths.
From the checkout root, run these commands in **PowerShell**.

```powershell
git lfs install
git lfs pull
uv sync --locked

$ueRoot = 'C:\Program Files\Epic Games\UE_5.8'
$project = (Resolve-Path 'engine/UERLHost.uproject').Path
& "$ueRoot\Engine\Build\BatchFiles\Build.bat" UERLHostEditor Win64 Development $project -WaitMutex
```

An LFS pointer is not a usable UE asset.
Before training, make sure that LFS access works.
Complete the Editor build.
Obtain Unreal Engine separately.
This repository does not grant redistribution rights for Unreal Engine or third-party content.

### Inspect tasks without starting UE

```powershell
uv run uerl tasks
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0
uv run uerl check task UERL-PhantomX-ContinuousTerrain-v0
```

The Task check validates Python declarations.
It does not validate the installed UE binary, content loading, or GPU readiness.
Use `uv run uerl check host` for static CartPole host file checks.
Use a [host profile](docs/how-to/ue-host-profile.md) to save reusable paths.

### Run a small training smoke test

After the Editor build succeeds:

```powershell
uv run uerl train --task UERL-CartPole-Direct-v0 --num-envs 2 --max-iterations 1 --device cpu --run-name smoke
```

Save custom engine and project paths in the [host profile](docs/how-to/ue-host-profile.md).
The commands `train`, `play`, and `export` use these paths in launch mode.
The options `--ue-executable` and `--project` replace individual profile fields.
These commands do not automatically read `UE_ROOT`.
This smoke test starts UE and trains a policy.

### Train PhantomX

```powershell
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --run-name terrain
```

The command prints `[RUN] directory=...`.
Keep that directory with its resolved configuration, logs, and checkpoints.
Generated `runs/` output is not distributed with the source.
Continuous-terrain training defaults to 64 Slots.
Flat-ground walking defaults to 512 Slots.
Before you change parallelism or resume an older checkpoint, read [training and evaluation](docs/how-to/phantomx-robust-training.md).

### Evaluate and record

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir --terrain-level 0 --steps 4000 --presentation none
uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir --terrain-level 0 --record artifacts/walk.mp4
```

Playback uses one robot and starts with the registered Task curriculum.
Use `--terrain-level` to select a procedural tier for evaluation.
With variable decimation, a control-step count does not specify a fixed duration.
See [recording and playback](docs/how-to/record-video.md) for keyboard control, recording options, and result interpretation.

### Export and deploy

```powershell
uv run uerl export --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir
uv run uerl deploy --project '<target-project-directory>' --demo phantomx --task UERL-PhantomX-ContinuousTerrain-v0 --artifact $runDir
```

Export initializes a UE Session.
It is not an offline checkpoint conversion.
Deployment copies the runtime, changes target project settings, and prints an import command.
After the target Editor build succeeds, add `--import` to execute that import.
Use `deploy --check` for read-only preflight.
Use the [deployment guide](docs/in-game-deployment-guide.md) for Blueprint setup, physics settings, and packaging.

## Tasks

| Task ID | Purpose | Default Slots |
|---|---|---:|
| `UERL-CartPole-Direct-v0` | CartPole balance and training smoke test | 64 |
| `UERL-PhantomX-Walk-v0` | Flat-ground locomotion | 512 |
| `UERL-PhantomX-ContinuousTerrain-v0` | Eight levels of continuous terrain | 64 |
| `UERL-PhantomX-DiscreteTerrain-v0` | Six levels of discrete obstacles | 64 |
| `UERL-PhantomX-Pursuit-v0` | Pursuit in an optional locally installed Egypt map | 1 |

Use `uerl config --task <TaskID>` for resolved defaults.
Robot declarations are in `src/uerl/assets/robots/`.
Task factories can replace base YAML settings.
See [configuration](docs/configuration.md).

## Documentation and development

[Documentation index](docs/README.md) · [Architecture](docs/architecture.md) · [Domain glossary](CONTEXT.md) · [Add a robot](docs/how-to/add-a-robot.md) · [Tests](tests/README.md) · [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Changelog](CHANGELOG.md)

For Python-only development after installation:

```powershell
uv run pytest -q
```

UE Automation, end-to-end training, recording, and packaging require the Windows/UE host.
This documentation update does not include new Windows/UE execution evidence.
Python-only validation does not establish runtime compatibility or policy performance.
Use [Write tests](docs/how-to/write-tests.md) for project-specific test implementation and review.

## License

Original EmbodiedUE project code, documentation, and configuration are licensed under
[Apache-2.0](LICENSE). Third-party code and content retain their respective rights;
this license does not cover Unreal Engine, Fab/Epic assets, or robot assets, policy
artifacts, and media without an explicit project licensing statement. See
[Third-party notices and asset inventory](THIRD_PARTY_NOTICES.md).

Unreal Engine is obtained separately under Epic's applicable agreements. The owner
has confirmed that `Stylized_Egypt` comes from Fab. Its raw assets are excluded from
the current tree; historical Git/LFS copies remain a publication blocker. Other
content provenance is still under review. See [release readiness](docs/release-readiness.md).
