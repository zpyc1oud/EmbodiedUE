# PhantomX terrain training and evaluation

This guide describes the current objective for `UERL-PhantomX-ContinuousTerrain-v0`.
Flat-ground, discrete-terrain, and Pursuit Tasks use the same physical-time reward and episode rules.
The terrain-level curriculum applies to Tasks with procedural terrain.

## Terrain configuration and parallel Slots

[continuous.yaml](../../src/uerl/configs/environments/terrains/phantomx/continuous.yaml) defines eight difficulty levels and their generation parameters.
The Task factory expands patch dimensions for episode travel and scan reach.
The file `resolved_config.yaml` records the terrain definition and `worker.slot_count`.
A count of 64 Slots does not mean 64 independently maintained terrain configurations.

Continuous terrain defaults to Slot-isolated geometry and an 8×8 placement grid for 64 Slots.
Each Slot has terrain geometry, a level, and episode state.
The option `--num-envs` changes parallelism.
Before an increase, examine collision isolation, placement, and terrain coverage.
Discrete terrain defaults to a shared terrain atlas.
These defaults come from the Task factories, not from every PhantomX Task.

The Session generates all difficulty regions during initialization.
Each control step converts body-frame commands to the world frame.
It then projects horizontal displacement onto the command direction.
Geometry does not regenerate during an episode.

At reset, promotion occurs when progress exceeds the smaller of 80% of commanded distance or half the terrain width.
Progress below half the commanded distance causes demotion by one level.
Turning paths count toward distance.
Excessive speed gives no extra promotion credit.
Promotion above the highest level causes random selection across all levels.
Termination and timeout use the same rule.

Checkpoints keep the per-Slot curriculum state and its random stream.

## Physical-time training objective

The value of `physics_dt` is **0.005 seconds**.
Each control step samples one `step_decimation` from the inclusive range `[1, 7]`.
All synchronized Slots share this sample.
Thus, control intervals are **5–35 ms**, with a fixed physics timestep.

The setting `reference_dt_s: 0.02` calibrates reward weights and PPO parameters.
It does not require a 20 ms control interval.

| Quantity | Current rule |
|---|---|
| Continuous rewards and penalties | Multiply the 20 ms reference value by `actual_step_seconds / 0.02` |
| Falls and Worker Slot faults | Charge once per event, without duration scaling |
| PPO discount and GAE | Use `gamma^(actual_step_seconds / 0.02)` and `lambda^(actual_step_seconds / 0.02)`; timeout bootstrap uses the transition discount |
| Episode limit | 20 seconds of simulation time, accumulated from completed solver steps; sparse reset clears only the selected Slots |
| Initial timeout phase | At training startup, including resume, initialize each Slot's first episode clock at a random time in `[0, 20)` seconds to stagger timeouts |
| Action-change penalty | Sum squared differences between consecutive normalized policy actions; do not divide by the control interval |
| Evaluation vibration frequency | Use the actual sampling times of completed control windows |

CartPole retains fixed-step PPO, reward, and timeout behavior.
The design uses Isaac Lab's time-integrated reward and terrain resampling concepts without an Isaac Lab runtime dependency.
Equal objective timing does not establish equal Chaos and PhysX trajectories.

## Command curriculum measurement

The command curriculum integrates linear velocity error over completed physical time.
A 35 ms transition contributes seven times as much as a 5 ms transition.
Only valid observations contribute. A completed episode without valid elapsed time
cannot qualify for promotion. Reset clears the selected Slots' integrals only.

The linear error limit is half the commanded speed, bounded by 0.03 and 0.20 m/s.
The curriculum integrates this limit over the same physical intervals as the error.
Thus, standing under a 0.1 m/s command does not count as successful tracking.
Turning episodes also require mean yaw-rate error below 0.25 rad/s.
Only timeout episodes without a physical failure can qualify.

Moving commands cover 0.05–0.5 m/s. Ten percent of sampled commands request standing.
After turn promotion, twenty percent of the remaining commands request turning
without translation. The other moving commands keep forward translation.

Linear and yaw tracking use positive exponential rewards. Exact tracking has a
unit peak before weighting, including a stationary command. Drift reduces that
reward. Existing progress terms remain separately weighted.

## Start and resume a Run

Run:

```powershell
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --run-name robust-time
```

Record the printed `[RUN] directory=...`.
Select that directory explicitly for evaluation.
To resume the Run, execute:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --resume $runDir
```

The command `uerl runs` lists Runs and resumable checkpoints.
A Run selects `model_final.pt` when that file exists.
Otherwise, it selects the highest-numbered `rsl_rl/model_<iteration>.pt`.
The value `latest` selects by time, not objective compatibility.

New training uses `phantomx_tracking_time_v2`. Earlier objectives, including
`phantomx_physical_time_v1`, cannot resume into the changed reward objective.
Start a fresh Run and preserve the previous checkpoint for historical evaluation.
The rejection occurs before UE startup.
Terrain checkpoints from before command-direction progress also lack the required adaptive terrain state.
Some earlier terrain checkpoints lack the curriculum random stream.

The command `uerl train --terrain-level N --resume ...` omits terrain-curriculum restoration and retains command-curriculum restoration.
It does not bypass the objective compatibility requirement.

Single-robot playback uses registered Task curriculum settings instead of per-Slot checkpoint state.
Use `--terrain-level N` for a fixed procedural tier.
Historical policy behavior does not establish the effectiveness of the current objective.

## Per-level evaluation

Continuous-terrain levels are **0–7**.
The option `--steps` counts control steps.
With variable decimation, equal step counts can represent different physical durations.
Select the actual Run directory:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
0..7 | ForEach-Object {
  uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 `
    --run $runDir --terrain-level $_ --steps 4000 --presentation none
}
```

Compare completed episodes, success and fall rates, forward velocity, speed error, and body vibration.
The metric `survival_rate` counts episodes that reach the time limit without a
fall or Slot fault. `tracking_success_rate` also requires both linear and yaw
tracking to pass. Both rates use all completed episodes as their denominator.
A stationary robot can have a high survival rate and a low tracking success rate.

For each valid control interval, divide the linear error by
`clamp(0.5 * command_speed, 0.03, 0.20)` m/s. Divide the yaw error by
`clamp(0.5 * abs(command_yaw), 0.05, 0.25)` rad/s. An episode passes each
tracking check when the physical-time mean of the squared normalized error is
less than 1. Evaluate standing, low-speed walking, and turning as separate cases.
These initial criteria must stay fixed during a candidate comparison.

The report also gives linear and yaw RMSE, in m/s and rad/s. Each valid Slot
interval is weighted by its completed physical duration. Invalid samples do not
contribute to error means. An episode with a Slot fault cannot pass.
If no valid samples are available, error metrics are NaN.
Historical reports use `success_rate` for survival only.
The mean curriculum level or a video alone is also insufficient evidence.

See [recording](record-video.md) and [test boundaries](../../tests/README.md).

### Check turn-in-place playback

With the player controller, hold A or Q to turn left. Hold D or E to turn right.
W adds forward motion. S stops both forward and turn commands. A turn command
does not require W. Opposite turn keys
cancel each other. Releasing all keys sends zero velocity and default joint
targets. Use fixed commands for quantitative comparisons.
