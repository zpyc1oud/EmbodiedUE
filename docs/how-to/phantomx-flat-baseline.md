# PhantomX flat-ground baseline

Use this procedure for the first locomotion baseline in [Issue #41](https://github.com/zpyc1oud/EmbodiedUE/issues/41).
The initial target is `UERL-PhantomX-Walk-v0` on `/Game/Maps/NewMap`.
Establish standing and forward tracking before terrain or pursuit experiments.
This procedure defines the experiment. It does not report a learned policy.

## Freeze the first experiment

Use 64 Slots, a 5 ms physics step, and fixed decimation `[4, 4]`.
Each control window then contains 20 ms of physical time.
Keep the existing Robot pose, actuator gains, action scale, reward weights, and PPO settings.
Set observation noise and push magnitude to zero. Use fixed ground friction.
These settings remove disturbances from the initial diagnosis.
They do not change the shipped default Task.

The registered command source starts with forward motion and standing samples.
Forward speed is 0.4–0.5 m/s. Standing probability is 0.1.
Commands resample after 5–10 physical seconds.
The command curriculum can enable turns for newly reset episodes after promotion.
Its threshold is mean planar tracking error below 0.20 m/s, with timeout and no termination.
The rolling window is 512 episodes, with at least 128 episodes and a 0.6 qualifying fraction.
Record the promoted stage; do not describe a promoted Run as straight-only training.

The existing reward includes shifted exponential tracking, progress, posture,
clearance, joint-velocity, action-change, and terminal fall terms.
Keep these weights fixed for this baseline. Test a different reward in a separate Run.
The [Task comparison](../design/task-runtime-stabilization.md) records the retained differences from Isaac Lab.

## Check the effective configuration

From the repository root in PowerShell:

```powershell
uv run uerl config --task UERL-PhantomX-Walk-v0 `
  --worker.slot_count 64 `
  --worker.decimation '[4, 4]' `
  --task.observation_noise.lin_vel_b 0 `
  --task.observation_noise.ang_vel_b 0 `
  --task.observation_noise.gravity_b 0 `
  --task.observation_noise.joint_pos_rel 0 `
  --task.observation_noise.joint_vel 0 `
  --task.events.push_velocity_min_mps 0 `
  --task.events.push_velocity_max_mps 0 `
  --task.events.static_friction_min 0.9 `
  --task.events.static_friction_max 0.9 `
  --task.events.dynamic_friction_min 0.7 `
  --task.events.dynamic_friction_max 0.7
uv run uerl train --task UERL-PhantomX-Walk-v0 --seed 0 --max-iterations 1 --run-name flat-smoke `
  --worker.slot_count 64 `
  --worker.decimation '[4, 4]' `
  --task.observation_noise.lin_vel_b 0 `
  --task.observation_noise.ang_vel_b 0 `
  --task.observation_noise.gravity_b 0 `
  --task.observation_noise.joint_pos_rel 0 `
  --task.observation_noise.joint_vel 0 `
  --task.events.push_velocity_min_mps 0 `
  --task.events.push_velocity_max_mps 0 `
  --task.events.static_friction_min 0.9 `
  --task.events.static_friction_max 0.9 `
  --task.events.dynamic_friction_min 0.7 `
  --task.events.dynamic_friction_max 0.7
```

The second command starts a short training smoke check on a configured UE host.
Use a new Run. Preserve previous Runs and checkpoints.
Save the printed Run directory, exact source revision, resolved YAML, host, and logs.
Check finite losses, valid observations, reset behavior, checkpoint creation, export,
and process shutdown. One iteration does not establish walking quality.

## Evaluate behavior separately

Select the exact Run directory. Evaluate at least one complete 20-second episode
per command; 1100 steps at fixed D4 provide 22 seconds.
Run fixed commands in separate invocations so standing cannot mask moving errors.

```powershell
$runDir = 'runs/UERL-PhantomX-Walk-v0/<run-directory>'
uv run uerl play --run $runDir --controller fixed --fixed-velocity '0,0,0' --steps 1100 --presentation none
uv run uerl play --run $runDir --controller fixed --fixed-velocity '0.45,0,0' --steps 1100 --presentation none
```

Use the CLI's supported fixed-command syntax from `uerl play --help` if invoking
from a shell with different argument quoting.
Add low forward speed, stopping, and signed yaw cases as separate held-out tests
when their command training coverage is implemented. A successful forward case
does not establish backward, lateral, or turning ability.

The report separates these measurements:

| Field | Meaning |
|---|---|
| `survival_rate` | Completed timeout episodes without base contact / completed episodes |
| `fall_rate` | Base-contact termination count / completed episodes |
| `speed_error` | Physical-time-weighted mean planar velocity error magnitude, m/s |
| `linear_velocity_rmse` | Square root of the time-weighted mean squared planar error, m/s |
| `yaw_rate_rmse` | Square root of the time-weighted mean squared yaw-rate error, rad/s |
| `measured_seconds` | Sum of completed control-window durations per synchronized Slot |

Continuous diagnostic means weight each window by its completed physical duration.
All Slots contribute equally within a window. Incomplete episodes contribute to
tracking metrics, but not to episode survival or fall denominators.
Reset diagnostics retain event counts. Episode length remains a control-step count.

The output field `success_rate` was renamed to `survival_rate`.
Update report consumers. Historical `success_rate` values mean survival only.
A stationary robot can survive while its forward tracking error remains 0.45 m/s.
No command-qualified pass is inferred from survival.

## Learning acceptance

After the smoke path works, train a bounded pilot with the same configuration.
Record simulated seconds, transitions, optimizer updates, and wall time.
Use pilot data to freeze feasible error and fall thresholds before held-out evaluation.
Report standing drift, forward and yaw tracking, stopping response, and failures separately.
Use three independent training seeds for a final baseline and separate evaluation seeds.

Only then compare command coverage, reward changes, randomization, and variable
control intervals. Change one factor per comparison.
Keep the fixed-interval baseline as the reference.
A runtime benchmark measures throughput, not learning quality.

## Remaining work

This slice corrects measurement and curriculum time weighting.
The current command curriculum still aggregates standing and moving episodes,
and its turn-stage history does not require angular tracking.
Broader command coverage and these promotion criteria remain in Issue #41.
The reward comparison and learned-policy acceptance also remain open.
Deployment acceptance in Issue #8 requires a quality policy and held-out static scenes.
