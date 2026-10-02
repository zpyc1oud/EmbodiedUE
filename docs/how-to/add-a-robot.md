# Adding a robot

A new robot consists of **UE assets, a Python robot declaration, and a Python Task**. Robots within the supported topology and operator contracts share the existing UE runtime, action schema, and reset path. CartPole and PhantomX are the reference integrations; see [architecture](../architecture.md#robot-integration).

Generate starter files with:

```powershell
uv run uerl new robot phantomx2 --asset /Game/Robots/PhantomX2/SK_PhantomX2
uv run uerl new task walk2 --robot phantomx2 --template phantomx-walk
```

The scaffolder refuses to overwrite existing files and does not modify the explicit registry. Complete the generated `TODO` topology and task mathematics before registration. `max_episode_steps: 1000` is a placeholder. For variable-decimation locomotion, explicitly design rewards, discounts, and episode duration around physical time as described in the [PhantomX guide](phantomx-robust-training.md).

## 1. UE assets

Add a Skeletal Mesh, Skeleton, and PhysicsAsset to the host project. Structure is reflected from those assets; Python does not duplicate mass, inertia, geometry, or joint limits. The existing PhantomX mesh is `/Game/Robots/PhantomX/SK_PhantomX`.

Verify that the asset meets the runtime's supported topology before writing task code. Confirm permission to distribute any new meshes, textures, and physics assets before contributing them.

## 2. Python robot declaration

Add a declaration under `src/uerl/assets/robots/`: asset path, joint/body names, reference pose, actuator groups, observation selection, and reset distributions. Register it in `ROBOT_ASSETS` in `src/uerl/assets/robots/__init__.py`.

Use [cartpole.py](../../src/uerl/assets/robots/cartpole.py) and [phantomx.py](../../src/uerl/assets/robots/phantomx.py) as examples.

## 3. Python Task

Add task configuration, a builder, and registration under `src/uerl/tasks/`. Register the task in `src/uerl/tasks/registry/defaults.py`. The registered ID then becomes available to `uerl train`, `uerl play`, and `uerl export`.

Inspect the existing [CartPole](../../src/uerl/tasks/cartpole/) and [PhantomX](../../src/uerl/tasks/phantomx/) implementations. Run `uerl tasks` and `uerl check task <TaskID>` before a real UE smoke test. A Python preflight cannot validate asset reflection or Chaos behavior.

## 4. Train and deploy

Train through the same CLI and export a `.uerlpol2` artifact. Verify observation/action dimensions, robot topology, and timing against the intended deployment mesh. Follow [in-game deployment](../in-game-deployment-guide.md) and the relevant [test layers](../../tests/README.md).
