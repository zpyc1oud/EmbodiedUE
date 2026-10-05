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
package, copy `examples/external-cartpole` outside the repository, change its
package name, entry-point name, and Task ID, then install that directory. Use
`uv pip install -e <directory> --no-deps` for editable development in the prepared
environment. Edit `src/example_cartpole/reward.yaml` to change
`pole_position_weight`. Reinstall after editing a non-editable installation.

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

To create a new package from the CLI instead, generate a ready-to-install
project outside the repository:

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
Task and changes only `pole_position_weight` in
`src/balance_demo/reward.yaml`. Run its tests with `python -m pytest`; edit the
YAML resource and reinstall after changing a non-editable installation.

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

Built-in training and terrain YAML files now live under `src/uerl/configs/` and
ship in wheels and source distributions. Existing loaders read that installed
location. Logical resource identifiers such as
`environments/terrains/cartpole/flat.yaml` stay unchanged. Code using the previous
checkout path `configs/tasks/...` should use the Task loaders instead. The
historically named `repository_config_root()` now returns the package resource
root for standard wheel and editable installations; importing directly from a
zipped wheel is not supported by the path-based loaders.

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
