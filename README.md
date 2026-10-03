# EmbodiedUE · UE RL Engine

Train robot control policies in **Unreal Engine 5.8 / Chaos**, then run the exported policies on Skeletal Mesh robots in a UE game. Python owns the task and PPO training; the UE plugin owns physics, observations, and in-game inference.

The project includes CartPole and PhantomX hexapod tasks. It uses **rsl-rl**, with observation and action plans shared between Python training and C++ deployment. Isaac Sim and Isaac Lab are not runtime dependencies.

<video src="docs/media/phantomx-walk.mp4" controls muted playsinline width="720"></video>

[Watch the walking demo](docs/media/phantomx-walk.mp4) · [Terrain demo](docs/media/phantomx-terrain.mp4)

These videos illustrate sample policies, not a performance guarantee for newly trained models.

## Project status: Early-Stage

EmbodiedUE is an early-stage project. APIs, artifact formats, and workflows may change;
backward compatibility and production readiness are not guaranteed. Python checks do
not establish UE runtime compatibility. Clean-host Windows/UE build, training, import,
and packaging validation remain outstanding in the current release review.

The Egypt gameplay scene is an [optional Fab installation](docs/how-to/optional-egypt-demo.md).
Acquire [Stylized Egypt](https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd)
yourself; the listing displayed Free on 2026-10-03. Its source assets are excluded from
this tree. The default game map is the empty Engine Entry map; open the installed
Egypt map explicitly for gameplay. CartPole and procedural-terrain task maps are unchanged.

## What is included

- Request-driven physics stepping and batched robot instances in a UE world.
- CartPole balancing and PhantomX walking, terrain, and pursuit tasks.
- Fixed physics steps with variable control intervals for PhantomX: 5 ms physics steps and 1–7 steps per action.
- Training, checkpoint inspection, playback, recording, export, and deployment through `uerl`.
- `.uerlpol2` artifacts containing observation/action plans, timing, robot metadata, and an ONNX policy for UE NNE inference.
- Python, protocol, cross-language parity, UE Automation, and end-to-end tests.

## Getting started

### Prerequisites

The integrated training and deployment workflow targets **Windows x64 and UE 5.8**. Other UE versions and Linux/macOS UE execution are not validated by this repository's documented workflow. Python-only inspection and tests can run separately from UE.

Install these tools before using the commands below:

| Requirement | Purpose |
|---|---|
| Git and Git LFS | Check out source and retrieve `.uasset` / `.umap` content |
| Unreal Engine 5.8 and its Windows C++ build toolchain | Build `UERLHostEditor Win64 Development` and run the Chaos Worker |
| Python 3.11 and `uv` | Install the Python package and locked dependencies |
| NVIDIA GPU with a driver compatible with the pinned CUDA 12.8 PyTorch build | Default PhantomX policy training on `cuda:0`; `--device cpu` selects CPU policy execution |

Chaos simulation runs in UE; selecting CPU policy execution does not remove the UE requirement. Use the C++ compiler and Windows SDK accepted by your UE 5.8 installation; this repository does not pin their versions. The project pins `torch==2.11.0+cu128`, `torchvision==0.26.0+cu128`, and `rsl-rl-lib==5.4.2`. No minimum RAM, GPU memory, or throughput guarantee has been established here.

The host enables the bundled `UERLEngine` plugin and its engine dependencies, `ProceduralMeshComponent` and `NNERuntimeORT`. Confirm those engine plugins are available before building; see [troubleshooting](docs/troubleshooting.md).

### Install and build

Run these commands in **PowerShell**, from your checkout root. Replace placeholder paths with your own paths.

```powershell
git lfs install
git lfs pull
uv sync --locked

$ueRoot = 'C:\Program Files\Epic Games\UE_5.8'
$project = (Resolve-Path 'engine/UERLHost.uproject').Path
& "$ueRoot\Engine\Build\BatchFiles\Build.bat" UERLHostEditor Win64 Development $project -WaitMutex
```

An LFS pointer is not a usable UE asset. Confirm LFS access and complete the Editor build before training. Unreal Engine is obtained separately; this repository does not grant rights to redistribute it or third-party content.

### Inspect tasks without starting UE

```powershell
uv run uerl tasks
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0
uv run uerl check task UERL-PhantomX-ContinuousTerrain-v0
```

The task check validates Python declarations, not the installed UE binary, content loading, or GPU readiness.
Use `uv run uerl check host` for static CartPole host file checks and a reusable
[machine profile](docs/how-to/ue-host-profile.md).

### Run a small training smoke test

After the Editor build succeeds:

```powershell
uv run uerl train --task UERL-CartPole-Direct-v0 --num-envs 2 --max-iterations 1 --device cpu --run-name smoke
```

For a non-default engine path, pass `--ue-executable "$ueRoot\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"` to `train`, `play`, and `export`. These commands do not read `UE_ROOT` automatically. This smoke test starts UE and trains a policy; it is not a configuration-only check.

### Train PhantomX

```powershell
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --run-name terrain
```

The command prints `[RUN] directory=...`. Keep that directory: it contains the resolved configuration, logs, and checkpoints. `runs/` is local generated output and is not distributed with the source. Continuous-terrain training defaults to 64 Slots; flat-ground walking defaults to 512. See [training and evaluation](docs/how-to/phantomx-robust-training.md) before changing parallelism or resuming an older checkpoint.

### Evaluate and record

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir --terrain-level 0 --steps 4000 --presentation none
uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir --terrain-level 0 --record artifacts/walk.mp4
```

Playback uses one robot. Explicitly select a terrain level to avoid restoring a multi-Slot terrain curriculum into a single-Slot evaluation. A control-step count is not a fixed duration when decimation varies. For keyboard control, recording options, and interpretation of results, see [recording and playback](docs/how-to/record-video.md).

### Export and deploy

```powershell
uv run uerl export --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir
uv run uerl deploy --project '<target-project-directory>' --demo phantomx --task UERL-PhantomX-ContinuousTerrain-v0 --artifact $runDir
```

Export initializes a UE Session; it is not an offline checkpoint conversion. Deployment copies the runtime, updates target project settings, and prints an import command. Add `--import` to execute the Editor import after the target Editor build succeeds. Use `deploy --check` for a read-only preflight. Follow the [deployment guide](docs/in-game-deployment-guide.md) for Blueprint setup, physics settings, and packaging.

## Tasks

| Task ID | Purpose | Default Slots |
|---|---|---:|
| `UERL-CartPole-Direct-v0` | CartPole balance and training smoke test | 64 |
| `UERL-PhantomX-Walk-v0` | Flat-ground locomotion | 512 |
| `UERL-PhantomX-ContinuousTerrain-v0` | Eight levels of continuous terrain | 64 |
| `UERL-PhantomX-DiscreteTerrain-v0` | Six levels of discrete obstacles | 64 |
| `UERL-PhantomX-Pursuit-v0` | Pursuit in an optional locally installed Egypt map | 1 |

Use `uerl config --task <TaskID>` for the resolved defaults. Robot declarations live in `src/uerl/assets/robots/`; task factories can override base YAML settings. See [configuration](docs/configuration.md).

## Documentation and development

[Documentation index](docs/README.md) · [Architecture](docs/architecture.md) · [Domain glossary](CONTEXT.md) · [Add a robot](docs/how-to/add-a-robot.md) · [Tests](tests/README.md) · [Contributing](CONTRIBUTING.md) · [Security](SECURITY.md) · [Changelog](CHANGELOG.md)

For Python-only development after installation:

```powershell
uv run pytest -q
```

UE Automation, end-to-end training, recording, and packaging require the Windows/UE host. These Windows/UE workflows have not been rerun during this documentation update. Python-only validation does not establish runtime compatibility or policy performance.

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
