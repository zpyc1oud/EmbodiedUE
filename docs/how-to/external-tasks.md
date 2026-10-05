# Install an external Task

An installed Python package can provide a Task without changing framework
registration. The [CartPole example](../../examples/external-cartpole/) is a
complete packaging example that changes one reward weight and reuses the existing
CartPole Task factory, Robot declaration, observations, actions, and runtime.

## Install and enable the example

With the repository's Python dependencies already installed, run in PowerShell:

```powershell
uv pip install ./examples/external-cartpole --no-deps
$env:UERL_TASK_PLUGINS = 'example-cartpole'
uerl tasks
uerl check task Example-CartPole-v0
uerl config --task Example-CartPole-v0 --json
```

These commands work from outside the checkout. `check task` validates Python
configuration and Task construction without starting UE. To develop your own
package, generate a ready-to-install project outside the repository:

```powershell
uv run uerl new external-cartpole balance-demo --output-dir ..\balance-demo
Set-Location ..\balance-demo
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'balance-demo'
uerl tasks --filter UERL-BalanceDemo-v0
uerl check task UERL-BalanceDemo-v0
uerl config --task UERL-BalanceDemo-v0 --json
```

The generated project includes entry-point metadata, its own reward resource,
registration tests, and usage instructions. It reuses the framework CartPole
Task and changes only `pole_position_weight` in `src/balance_demo/reward.json`.
Run its tests with `python -m pytest`. The generated package can be installed and
checked from any current directory; `check task` does not start UE. For editable
development, use `uv pip install -e <directory> --no-deps` in the prepared
environment. Reinstall after editing a non-editable installation.

On an already configured Windows/UE host, the same registration is used by:

```powershell
uerl train --task Example-CartPole-v0 --num-envs 2 --max-iterations 1 --device cpu
```

Supply host paths as described in [Getting started](../../README.md#getting-started).
The Python wheel does not contain UE assets, the host project, or UE binaries.

## Registration contract

Declare a named entry point in your package's `pyproject.toml`:

```toml
[project.entry-points."uerl.tasks"]
my-task = "my_package:create_registration"
```

The target is a zero-argument callable returning one
`uerl.tasks.registry.TaskRegistration`. Its factories supply fresh Task, Worker,
and runner configurations and a `DirectTask`. Existing optional curriculum,
event, and evaluation factories remain available. A Task owns its Python
resources and loads them with `importlib.resources`; do not use the current
working directory to locate them.

Enable entry-point names with the comma-separated `UERL_TASK_PLUGINS` environment
variable. Installation alone does not enable a package. All CLI commands use
this selection. Python callers can use
`create_default_registry(external_tasks=("my-task",))`; passing `()` selects only
built-ins and overrides the environment. Loading enabled packages executes their
Python code, so enable packages you trust.

Task IDs must be unique across built-ins and enabled packages, even across
versions. Entry-point names must be unique among installed packages. Missing
names, import failures, non-callable targets, invalid registration objects, and
invalid factories produce `RegistryError` with the entry-point source. Failed
registry construction does not return a partially populated registry. Keep the
same package and selection available when restoring a run.

Registration does not require Manager composition. External factories can return
existing `DirectTask` subclasses or composed Tasks; both use `DirectEnv`, Session,
reset, and valid-Slot handling. This example uses the existing composed CartPole
factory and introduces no new simulation loop.

## Built-in resources

Built-in training and terrain YAML files now live under `src/uerl/configs/` and
ship in wheels and source distributions. Existing loaders read that installed
location. Logical resource identifiers such as
`environments/terrains/cartpole/flat.yaml` stay unchanged. Code using the previous
checkout path `configs/tasks/...` should use the Task loaders instead. The
historically named `repository_config_root()` now returns the package resource
root for standard wheel and editable installations; importing directly from a
zipped wheel is not supported by the path-based loaders.

## Validation boundary

The packaging test builds the framework wheel and a freshly generated Task wheel,
installs them into an isolated target, changes to an unrelated directory, and
checks discovery, built-in defaults, external resource loading, Task construction,
and CLI preflight. This does not validate UE execution, training quality, or export.
Minimal/composed equivalence and early export capability reporting remain work
tracked by [Issue #6](https://github.com/zpyc1oud/EmbodiedUE/issues/6).
