# External CartPole

This independent Python package registers `Example-CartPole-v0`.
It changes one CartPole reward weight.
It uses the existing CartPole Task, Robot declaration, actions, observations, and runtime.

## Install from this directory

1. Copy this complete directory to your development location.
2. Use Python 3.11 and `uv`.
3. Activate an environment with the matching EmbodiedUE package and runtime dependencies.

Use the framework [installation instructions](https://github.com/zpyc1oud/EmbodiedUE/blob/main/README.md#install-and-build) to prepare that environment.
The framework must support external Task registration through `uerl.tasks` entry points.

From the copied directory, run these PowerShell commands:

```powershell
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'example-cartpole'
uerl tasks --filter Example-CartPole-v0
uerl check task Example-CartPole-v0
uerl config --task Example-CartPole-v0 --json
```

The option `--no-deps` uses the prepared framework environment.
The example does not install UE or host assets.
The three inspection commands do not start UE.
After installation, they work from any directory.

In Bash, replace the PowerShell environment assignment with `export UERL_TASK_PLUGINS=example-cartpole`.

## Change one reward

1. Edit `pole_position_weight` in `src/example_cartpole/reward.yaml`.
2. Run `uerl config --task Example-CartPole-v0 --json` again.
3. Examine `task.rew_scale_pole_pos`.

The initial weight is `-2.0`.
An editable installation reads the file for each new Task configuration.
Built-in CartPole defaults stay unchanged.
For a non-editable installation, reinstall the package after source changes.

For a separate variant, change these identifiers:

- Distribution name and entry-point name in `pyproject.toml`
- Task ID in `src/example_cartpole/__init__.py`

Enable the new entry-point name in `UERL_TASK_PLUGINS`.
Each enabled name must identify a zero-argument factory that returns one `TaskRegistration`.
Task IDs must be unique across built-in Tasks and enabled packages.

## Runtime validation

On a configured Windows/UE host, select this registration with `uerl train --task Example-CartPole-v0`.
Keep the package installed and its entry point enabled for saved Runs.
Python preflight and package tests do not establish UE execution, training quality, or correct export.
