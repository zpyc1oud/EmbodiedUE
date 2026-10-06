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

## Built-in resources

Built-in training and terrain YAML files are under `src/uerl/configs/`.
Wheels and source distributions include these files.
The existing loaders read this installed location.
Logical resource IDs, such as `environments/terrains/cartpole/flat.yaml`, remain unchanged.

Use Task loaders instead of the old checkout path `configs/tasks/...`.
The function `repository_config_root()` now returns the package resource root for standard wheel and editable installations.
Path-based loaders do not support direct import from a zipped wheel.

## Validation boundary

The package test builds framework, example, and generated-package wheels.
It installs them into an isolated target and uses an unrelated current directory.
It examines discovery, built-in defaults, both YAML resources, Task construction, and CLI preflight.
See [Write tests](write-tests.md#write-package-and-generator-tests) for the test procedure.

These results do not establish UE execution, training quality, or correct export.
[Issue #6](https://github.com/zpyc1oud/EmbodiedUE/issues/6) tracks the generated minimal Task interface, minimal/composed equivalence, and early export capability reporting.
