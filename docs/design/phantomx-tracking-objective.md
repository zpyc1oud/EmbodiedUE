# PhantomX tracking-objective comparison

Status: candidate implementation. Real training results are pending.
Related: [Issue #41](https://github.com/zpyc1oud/EmbodiedUE/issues/41).
Prerequisite: [command-coverage experiment](phantomx-command-coverage.md).

## Measured starting point

[PR #48](https://github.com/zpyc1oud/EmbodiedUE/pull/48) completed 1000 PPO loops
with mixed commands and the original reward. Stopping planar RMSE fell from
0.508 to 0.011 m/s. For commands of +0.5 and -0.5 rad/s, mean yaw remained near
zero. Forward planar RMSE increased from 0.142 to 0.157 m/s. All four pilot
groups survived. Action clipping exceeded 88% in the two turning groups.
These results justify a controlled reward comparison.
They do not establish a fault in reward mathematics or actuator limits.

## Reference

Use Isaac Lab revision `b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8`:

- [Velocity task](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab_tasks/isaaclab_tasks/manager_based/locomotion/velocity/velocity_env_cfg.py) uses positive exponential linear and yaw tracking with a standard deviation of 0.5.
- [Go1 configuration](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab_tasks/isaaclab_tasks/manager_based/locomotion/velocity/config/go1/rough_env_cfg.py) sets tracking weights to 1.5 and 0.75. It disables pushes and adapts other costs to the robot.

The reference supplies a concrete objective to test. It does not establish
optimal parameters for PhantomX or Chaos. Keep the existing body-frame velocity
contract and physical-time reward normalization.

## Candidate

The `task.tracking_reward` choice selects `shifted` or `exponential`.
The shipped choice remains `shifted` to preserve the recorded reference.
The explicit choice lets saved configuration identify the experimental objective.
Both choices use the same command channel and actor inputs.

The exponential choice scores planar tracking as `exp(-squared_xy_error / std²)`.
Yaw tracking uses `exp(-squared_yaw_error / std²)`.
Both use body-frame velocities. Their value at exact tracking is one.
The shifted choice subtracts the score that stationary motion would receive.
That subtraction depends on the command, not the current action. Removing it
alone is not proof of a better optimal policy; termination and value estimation
can still affect the learning comparison.

Apply these overrides to the complete unperturbed mixed-command configuration
from PR #48:

```powershell
uv run uerl config --task UERL-PhantomX-Walk-v0 `
  --task.tracking_reward exponential `
  --task.velocity_tracking_std 0.5 `
  --task.yaw_rate_tracking_std 0.5 `
  --task.linear_velocity_progress_weight 0.0 `
  --task.yaw_rate_progress_weight 0.0
```

This command shows the reward overrides only. Add the same mixed-command,
fixed-friction and zero-noise overrides used by the reference Run before training.
Keep tracking weights at 1.5 and 0.75. Keep all other costs, action clip, action
scale, Robot assets and PPO parameters fixed.
The comparison changes an objective family: score offset, linear width and
progress terms. It cannot assign causality to any one of these changes.
A wider kernel supplies a less local score but also rewards larger errors more.
Measure its effect rather than assume improvement.

Start a fresh Run with 64 Slots, seed 0, 5 ms physics, D4, 40 rollout steps and
1000 PPO loops. This is 2,560,000 transitions and 800 physical seconds per Slot.
Save every 50 loops. Preserve the reference Run and final checkpoint.
Do not restore a checkpoint across different reward configurations.
Old Runs without the explicit choice require their recorded source revision.

## Tests and decision

Unit tests must check the shared yaw term against independent values. Include a
rotated body, opposite motion and distinct rows. Test the composed reward through
the Task API for both objectives. Use 5 ms and 35 ms intervals and a physical
fall to check rate scaling and the one-time fall cost. Check YAML materialization
and an invalid objective name. Run the default suite and static checks.

On the UE host, verify the effective configuration difference first. Run a real
smoke path before the full budget. Evaluate the final checkpoint with the same
four commands: stop, forward 0.45 m/s, yaw +0.5 rad/s and yaw -0.5 rad/s.
Use 22 seconds per group. Report planar and yaw RMSE, mean signed velocities,
survival, falls, action clipping and effort clipping. Keep the training curves.
Export the final artifact and run real PolicyDemo and numerical parity checks.

A useful candidate must retain stopping and improve signed yaw tracking without
hiding forward drift. A completed budget alone is not task acceptance. If yaw
remains stationary, inspect actuator response and saturation before another
weight sweep. Record negative results. Freeze acceptance thresholds and use
independent seeds and held-out scenarios before a quality or repeatability claim.
