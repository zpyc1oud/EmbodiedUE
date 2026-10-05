# External CartPole

This independent Python package registers `Example-CartPole-v0` and changes one
CartPole reward weight. It reuses EmbodiedUE's CartPole Task, Robot declaration,
actions, observations, and runtime.

## Install from this directory

Copy this entire directory anywhere on your machine. Use Python 3.11 and `uv`,
and activate an environment with the matching EmbodiedUE package and its runtime
dependencies already installed. See the upstream
[installation instructions](https://github.com/zpyc1oud/EmbodiedUE/blob/main/README.md#install-and-build)
to prepare the framework environment. The framework must include external Task
registration support (`uerl.tasks` entry points).

Run these commands in PowerShell from the copied directory:

```powershell
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'example-cartpole'
uerl tasks --filter Example-CartPole-v0
uerl check task Example-CartPole-v0
uerl config --task Example-CartPole-v0 --json
```

`--no-deps` reuses the prepared framework environment. The example does not
install UE or its host assets. The three inspection commands do not start UE;
they work from any directory after installation. In Bash, use
`export UERL_TASK_PLUGINS=example-cartpole` instead of the PowerShell assignment.

## Change one reward

Edit `pole_position_weight` in `src/example_cartpole/reward.yaml` (initially
`-2.0`). The editable installation reads this file when it creates each fresh
Task configuration. Run `uerl config --task Example-CartPole-v0 --json` again and
inspect `task.rew_scale_pole_pos`. Built-in CartPole defaults stay unchanged.
For a non-editable installation, reinstall after editing the source files.

For your own variant, change the distribution name and entry-point name in
`pyproject.toml`, and the Task ID in `src/example_cartpole/__init__.py`. Enable
the new entry-point name in `UERL_TASK_PLUGINS`. Each enabled name must resolve
to a zero-argument factory returning one `TaskRegistration`; Task IDs must be
unique across built-ins and enabled packages.

## Runtime validation

On an already configured Windows/UE host, the same registration can be selected
with `uerl train --task Example-CartPole-v0`. Keep the package installed and its
entry point enabled when using saved runs. Python preflight and packaging tests
do not establish UE execution, training quality, or export correctness.
