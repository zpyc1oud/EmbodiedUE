# How-to guides

Complete [Getting started](../../README.md#getting-started) before using a runtime guide.
For the full sequence, follow [Build your first game robot](../README.md#build-your-first-game-robot).

## Prepare a robot and Task

| I want to… | Guide | Result |
|---|---|---|
| Save my UE installation and project paths | [Host profile](ue-host-profile.md) | Reusable launch settings |
| Use my own robot assets | [Add a robot](add-a-robot.md) | A robot declaration and Task integration |
| Keep my Task outside the framework repository | [External Tasks](external-tasks.md) | An installed, registered Python package |
| Inspect or change Task settings | [Configuration](../configuration.md) | Resolved settings and supported overrides |

Choose the Task path before writing custom behavior.
The Manager-based path supplies exportable observation and action plans.
The current DirectTask Python-hooks example supports training and evaluation but not policy export.
See [external Tasks](external-tasks.md#create-a-directtask-python-hooks-package) for that boundary.

## Train and measure behavior

| I want to… | Guide | Result |
|---|---|---|
| Establish a bounded flat-ground reference | [Flat-ground baseline](phantomx-flat-baseline.md) | Separate tracking and survival measurements |
| Train, resume, and evaluate PhantomX | [Training and evaluation](phantomx-robust-training.md) | A saved Run and per-condition measurements |
| Record playback or use keyboard commands | [Record video](record-video.md) | An MP4 of the selected Run |
| Measure simulation cost separately from learning | [Runtime benchmark](runtime-benchmark.md) | Runtime measurements for a declared setup |

## Deploy into a game

| I want to… | Guide | Result |
|---|---|---|
| Install the runtime and import a policy | [In-game deployment](../in-game-deployment-guide.md) | A controller connected to game commands |
| Check the controller in my target scene | [Deployment validation](policy-deployment-validation.md) | Parity, physical-response, and behavior evidence |
| Use the optional Egypt scene | [Optional Egypt demo](optional-egypt-demo.md) | Locally installed, authorized Fab content |

## Contribute or diagnose a problem

- [Troubleshooting](../troubleshooting.md): choose a check from the failure symptom.
- [Write tests](write-tests.md): select the test layer and assert the intended behavior.
- [Validate a feature batch](batch-validation.md): reuse a Windows worktree for a frozen set of changes.
