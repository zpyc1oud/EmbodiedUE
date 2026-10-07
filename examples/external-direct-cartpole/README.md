# Direct CartPole package

This installable package registers `UERL-DirectCartPole-v0` with the `uerl.tasks`
entry-point group. Its Task implements the public `DirectTask` hooks for
actions, observations, rewards, and termination. It uses the existing
DirectEnv, Session, reset, and valid-Slot lifecycle; it does not add a
second runtime loop.

## Install and inspect

Use Python 3.11 with the matching EmbodiedUE package installed. Package
checks do not start UE:

```powershell
uv pip install -e . --no-deps
$env:UERL_TASK_PLUGINS = 'example-direct-cartpole'
uerl tasks --filter UERL-DirectCartPole-v0
uerl check task UERL-DirectCartPole-v0 --json
```

The check output is a machine-readable preflight report, not a Task config.
It reports train and evaluate as supported and export as unsupported,
because this Python Task has no Manager-generated action/observation plans.
Do not infer ONNX or cross-language exportability from its metadata.

## Run on Windows/UE 5.8

After configuring the host project and CartPole assets, train a short
smoke Run with:

```powershell
uerl train --task UERL-DirectCartPole-v0 --num-envs 2 --max-iterations 1 --device cpu
```

The training smoke verifies Task integration, not learning quality. The
reward weight is loaded from `src/example_direct_cartpole/reward.yaml` for each fresh Task
configuration. Editing this YAML does not change the built-in CartPole
configuration.
