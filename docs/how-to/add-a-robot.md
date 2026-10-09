# Adding a robot

A robot integration contains UE assets, a Python robot declaration, and a Python Task.
Supported robots share the existing UE runtime, action schema, and reset path.
CartPole and PhantomX are the reference integrations.
See [architecture](../architecture.md#robot-integration) for the supported boundaries.

## Before you start

Complete the [host setup](../../README.md#getting-started).
Prepare a robot asset with body and joint names that you can match to its Python declaration.
For a game-deployable Task, choose an exportable observation and action path before training.
See [external Tasks](external-tasks.md) if you want to keep your Task in a separate package.

The result of this guide is a registered integration ready for training and physical validation.
Generating files alone does not validate the robot's dynamics.

## Create starter files

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
See the [PhantomX guide](phantomx-robust-training.md).

## 1. UE assets

1. Add a Skeletal Mesh, Skeleton, and PhysicsAsset to the host project.
2. Make sure that their topology meets the runtime requirements.
3. Obtain redistribution permission before you contribute meshes, textures, or physics assets.

The runtime reflects structure from these assets.
Python does not duplicate mass, inertia, geometry, or joint limits.
The existing PhantomX mesh is `/Game/Robots/PhantomX/SK_PhantomX`.

## 2. Python robot declaration

Add a declaration under `src/uerl/assets/robots/`.
Include the asset path, joint and body names, reference pose, actuator groups, observations, and reset distributions.
Register it in `ROBOT_ASSETS` in `src/uerl/assets/robots/__init__.py`.

Use [cartpole.py](../../src/uerl/assets/robots/cartpole.py) and [phantomx.py](../../src/uerl/assets/robots/phantomx.py) as examples.

## 3. Python Task

1. Add the Task configuration and builder under `src/uerl/tasks/`.
2. Register the Task in `src/uerl/tasks/registry/defaults.py`.
3. Run `uerl tasks`.
4. Run `uerl check task <TaskID>`.
5. Do a real UE smoke test.

The registered ID is available to `uerl train`, `uerl play`, and `uerl export`.
Use the existing [CartPole](../../src/uerl/tasks/cartpole/) and [PhantomX](../../src/uerl/tasks/phantomx/) implementations as references.
Python preflight does not establish correct asset reflection or Chaos behavior.

## 4. Train and deploy

Train through the same CLI.
Export a `.uerlpol2` artifact.
Compare its observation and action dimensions, topology, and timing with the intended deployment mesh.
Use the [deployment guide](../in-game-deployment-guide.md) and applicable [test layers](../../tests/README.md).
