# PhantomX command-coverage experiment

Status: implementation for a controlled learning comparison.
Related: [Issue #41](https://github.com/zpyc1oud/EmbodiedUE/issues/41).
Prerequisite: [PR #47](https://github.com/zpyc1oud/EmbodiedUE/pull/47).

## Observed failure

The fixed-D4 baseline completed training and real deployment checks.
Its final policy survived all four 22-second evaluation groups.
It nevertheless moved forward at 0.488 m/s under a zero command.
For a -0.5 rad/s yaw command, mean yaw was +0.192 rad/s.
Forward tracking at 0.45 m/s had planar RMSE 0.142 m/s.
See PR #47 for the exact revision, interrupted-run recovery, budget, and reports.

The baseline samples only forward moving speeds of 0.4–0.5 m/s.
Standing occupies 10% of command intervals.
Yaw starts after curriculum promotion and comes from a changing heading error.
It does not train explicit turn-in-place intervals.
This coverage gap is a testable explanation, not proof of the failure's cause.

## Isaac Lab reference

Pin upstream at `b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8`.

- [Velocity command implementation](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/isaaclab/envs/mdp/commands/velocity_command.py): body-frame velocity commands, direct sampled yaw or heading-derived yaw, timed resampling, and zero commands for standing.
- [Locomotion configuration](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab_tasks/isaaclab_tasks/manager_based/locomotion/velocity/velocity_env_cfg.py): fixed 5 ms physics, D4, 20-second episodes, positive exponential tracking, and separately configured motion costs.
- [Go1 overrides](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab_tasks/isaaclab_tasks/manager_based/locomotion/velocity/config/go1/rough_env_cfg.py): disabled pushes, reduced action scale, zero reset velocities, and robot-specific tracking weights of 1.5 and 0.75.

These are official implementation references. They do not prove that the same
parameters work for a six-legged PhantomX in Chaos.

## First comparison: command distribution only

Keep the previous physics, action, reward, PPO, friction, and noise settings.
Use a new Run. Keep 64 Slots, D4, seed 0, and 1000 PPO loops.
The comparison changes the command distribution only:

| Setting | Candidate | Reason |
|---|---|---|
| `heading_command` | `false` | Train signed yaw rates directly, available from the first episode |
| `initial_speed_min/max` | 0.05 / 0.5 m/s | Cover low speed through the measured forward target |
| `standing_probability` | 0.3 | Allocate more samples to the observed stopping failure |
| `turn_in_place_probability` | 0.2 | Train zero linear speed with signed yaw explicitly |
| `yaw_rate_min/max` | -0.5 / +0.5 rad/s | Include both evaluation turn directions |
| Resampling | Existing 5–10 seconds | Preserve the baseline interval distribution |

The two probabilities are disjoint fractions of all samples. Their sum must not
exceed one. Other samples combine forward velocity and direct yaw.
Lateral command remains zero. Backward and lateral competence are outside this comparison.
The mixture fractions and limited ranges are PhantomX experiment choices, not
Isaac Lab defaults or established optimal values.

The public three-value `velocity` channel, units, actor inputs and action plan
remain unchanged. Direct yaw is constant within a sampled interval; body heading
does not overwrite it. Standing always zeros all three values.
Selective reset replaces only selected Slots. Timers consume completed physical
time once, before the next observation. Rewards score the preceding command.

Heading-command mode retains its existing curriculum behavior for the recorded
reference task. Direct mode does not gate yaw on curriculum stage. The existing
stage diagnostics do not measure direct-mode command competence.
No terrain, engine, or policy-operator change is required.

The shipped YAML keeps the old command distribution for comparison. Select the
candidate explicitly with these overrides in addition to the unperturbed
[flat baseline](../how-to/phantomx-flat-baseline.md):

```powershell
uv run uerl config --task UERL-PhantomX-Walk-v0 `
  --task.command.heading_command false `
  --task.command.initial_speed_min 0.05 `
  --task.command.initial_speed_max 0.5 `
  --task.command.standing_probability 0.3 `
  --task.command.turn_in_place_probability 0.2 `
  --task.command.yaw_rate_min -0.5 `
  --task.command.yaw_rate_max 0.5
```

Use the same overrides with `uerl train`. The resolver records them in the Run.
Do not resume the heading-mode baseline with this distribution.

## Verification and decision

Before training, check typed YAML and overrides, both yaw signs, all mixture
groups, physical-time resampling, and sparse reset with unchanged other Slots.
Run the default Python suite, static checks, and the affected real UE smoke path.
The author reviews the exact candidate before publication.

Use the same four pilot commands as the reference: standing, forward 0.45 m/s,
and zero-linear-speed yaw +0.5 / -0.5 rad/s. Each lasts 22 physical seconds.
Report planar and yaw RMSE, signed mean velocities, survival, falls, and saturation.
Lower stopping drift and correctly signed turns support this hypothesis.
Keep forward tracking visible so an improvement in one group cannot hide another failure.
These comparisons are pilot results, not held-out acceptance.

If coverage alone is insufficient, run a separate reward comparison. Isaac Lab's
positive exponential tracking is a reference candidate. Keep its comparison
separate from command coverage; do not attribute a combined change to one cause.
Do not add effort or foot-air-time penalties without matching measured inputs.
After pilots, freeze quality criteria and evaluate separate seeds before a
repeatability or deployment-quality claim.
