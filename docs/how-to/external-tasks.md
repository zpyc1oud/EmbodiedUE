# Install an external Task

An installed Python package can supply a Task without changes to framework registration.
The [CartPole example](../../examples/external-cartpole/) changes one reward weight.
It uses the existing CartPole Task factory, Robot declaration, observations, actions, and runtime.

## Install and enable the example

First install the repository's Python dependencies.
Then run these PowerShell commands:

```powershell
uv pip install ./examples/external-cartpole --no-deps
$env:UERL_TASK_PLUGINS = 'example-cartpole'
uerl tasks
uerl check task Example-CartPole-v0
uerl config --task Example-CartPole-v0 --json
```

After installation, the inspection commands work outside the checkout.
The command `check task` validates Python configuration and Task construction without UE startup.

For your own package:

1. Copy `examples/external-cartpole` outside the repository.
2. Change the package name, entry-point name, and Task ID.
3. Install that directory into the prepared environment.

For editable development, use `uv pip install -e <directory> --no-deps`.
To change `pole_position_weight`, edit `src/example_cartpole/reward.yaml`.
For a non-editable installation, reinstall after a source change.

Alternatively, create a package with the CLI:

```powershell
uv run uerl new external-cartpole balance-demo --output-dir ..\balance-demo
Set-Location ..\balance-demo
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'balance-demo'
uerl tasks --filter UERL-BalanceDemo-v0
uerl check task UERL-BalanceDemo-v0
uerl config --task UERL-BalanceDemo-v0 --json
```

The generated project contains entry-point metadata, a reward resource, registration tests, and usage instructions.
It uses the framework CartPole Task.
Its only Task change is `pole_position_weight` in `src/balance_demo/reward.yaml`.
Run its tests with `python -m pytest`.
After a non-editable source change, reinstall the package.

On a configured Windows/UE host, use the same registration for training:

```powershell
uerl train --task Example-CartPole-v0 --num-envs 2 --max-iterations 1 --device cpu
```

Supply host paths as specified in [Getting started](../../README.md#getting-started).
The Python wheel does not contain UE assets, the host project, or UE binaries.

## Create a DirectTask Python-hooks package

The [Direct CartPole example](../../examples/external-direct-cartpole/) owns its
action, observation, reward, and termination functions in a `DirectTask` subclass.
It still uses the framework's `DirectEnv`, Session, reset, and valid-Slot path.
Its package-owned reward override is YAML, as are Task and Run configurations.

To generate another package from the CLI:

```powershell
uv run uerl new direct-cartpole direct-balance-demo --output-dir ..\direct-balance-demo
Set-Location ..\direct-balance-demo
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'direct-balance-demo'
uerl tasks --filter UERL-DirectBalanceDemo-v0
uerl check task UERL-DirectBalanceDemo-v0 --json
```

`check task --json` writes a machine-readable preflight report to stdout; it is
not a configuration file or a configuration input. The report shows whether
train, evaluate, and export are supported, unsupported, or unknown, with reasons.
For a Python-hooks Task, train/evaluate are supported and export is unsupported
because Python action/observation methods have no Manager-generated plans. Do
not infer mathematical or cross-language exportability from Task metadata.

## Registration contract

Declare a named entry point in the package's `pyproject.toml`:

```toml
[project.entry-points."uerl.tasks"]
my-task = "my_package:create_registration"
```

The target must be a zero-argument callable that returns one `uerl.tasks.registry.TaskRegistration`.
Its factories supply fresh Task, Worker, and runner configurations and a `DirectTask`.
Optional curriculum, event, and evaluation factories remain available.

Each Task owns its Python resources.
Use `importlib.resources` to load them.
Do not locate them through the current directory.

Enable entry-point names with the comma-separated `UERL_TASK_PLUGINS` environment variable.
Installation alone does not enable a package.
All CLI commands use this selection.
Python callers can use `create_default_registry(external_tasks=("my-task",))`.
An explicit `()` selects built-in Tasks only and overrides the environment variable.
Only enable trusted packages, because package loading executes their Python code.

Task IDs must be unique across built-in Tasks and enabled packages, including different versions.
Entry-point names must be unique among installed packages.
Missing names, import failures, invalid callables, invalid registration objects, and invalid factories cause `RegistryError`.
The error identifies the entry-point source.
A failed construction does not return a partial registry.
Keep the same package and selection available for saved Runs.

Registration does not require Manager composition.
An external factory can return a `DirectTask` subclass or a composed Task.
Both use DirectEnv, Session, reset, and valid-Slot behavior.
This example uses the existing composed CartPole factory without a new simulation loop.

## Simple and Manager Task implementations

An external factory can return either a `DirectTask` subclass that implements
`preprocess_actions`, `build_observations`, `compute_terminations` (or
`termination_terms`), and `compute_rewards`, or a `DirectTask` assembled from
action, observation, termination, and reward Managers. `DirectTask`,
`DirectTaskCfg`, `UERLDirectEnv`, and the capability report types are public
Python APIs. Both Task styles use the same `DirectEnv` step/reset path; Task code
does not own Session calls, sparse reset, or invalid-Slot compaction.

Inspect a constructed Task with `task.capabilities`. Training and evaluation are
supported when the required DirectTask methods or Manager declarations exist.
Manager Tasks report `unknown` until a Worker RobotSpec has been bound and their
plans have been assembled. A Python-only Task can train and evaluate, but its
Python observation/action math has no exported plan and reports export as
`unsupported` with the reason. The export service stops such a Task before
calling the ONNX exporter.

Training and evaluation report capabilities after Worker schema binding and
before their long loop. A known unsupported capability fails before Worker
startup; Manager `unknown` is allowed to reach RobotSpec binding and then must
resolve before the loop starts. Export rejects known unsupported work before
Worker startup, then checks strictly after DirectEnv initialization and its
initial reset, before vector-runner construction or checkpoint loading. It
proceeds only when the resolved report says `supported`; both `unsupported` and
`unknown` stop first. If a Task adds Python action or
observation behavior around Manager plans, export reports `unknown`: the
framework does not infer mathematical equivalence from registration metadata.
Resolve the implementation into a supported plan path before export, then
complete the separate artifact, UE, and target-scene checks for the behavior you
intend to ship.

## Built-in resources

Built-in training and terrain YAML files are under `src/uerl/configs/`.
Wheels and source distributions include these files.
The existing loaders read this installed location.
Logical resource IDs, such as `environments/terrains/cartpole/flat.yaml`, remain unchanged.

Use Task loaders instead of the old checkout path `configs/tasks/...`.
The function `repository_config_root()` now returns the package resource root for standard wheel and editable installations.
Path-based loaders do not support direct import from a zipped wheel.

## Validation boundary

The packaging test builds and installs the framework, checked-in examples, and
generated Manager and Direct package wheels into an isolated target. It checks
entry-point discovery, YAML resource loading, Task construction, and CLI
capability reports outside the checkout, then executes one scripted DirectEnv
step for each installed Direct package outside the checkout. The Direct/Manager
behavior test uses the checked-in Direct Task implementation, a Manager
registration, a scripted Session, and independent numeric expectations for
actions, observations, rewards, termination, reset masks, and invalid Slots.
These are Python checks; they do not validate UE execution, Chaos behavior,
training quality, or game deployment. Those remain separate Issue #6 acceptance
gates.

See [Write tests](write-tests.md#write-package-and-generator-tests) for the test procedure.
[Issue #6](https://github.com/zpyc1oud/EmbodiedUE/issues/6) tracks the remaining external Task acceptance.
