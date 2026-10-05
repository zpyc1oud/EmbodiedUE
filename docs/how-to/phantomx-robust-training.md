# PhantomX terrain training and evaluation

This guide describes the current objective for `UERL-PhantomX-ContinuousTerrain-v0`. Flat-ground, discrete-terrain, and pursuit tasks reuse PhantomX's physical-time reward and episode rules. Terrain-level curriculum applies to tasks with procedural terrain.

## Terrain configuration and parallel Slots

[continuous.yaml](../../src/uerl/configs/environments/terrains/phantomx/continuous.yaml) defines eight difficulty levels and their generation parameters. The task factory expands the patch dimensions to cover episode travel and scan reach. `resolved_config.yaml` records both the terrain definition and `worker.slot_count`; 64 Slots does not mean 64 independently maintained terrain configurations.

Continuous terrain defaults to Slot-isolated geometry and an 8×8 placement grid for 64 Slots. Each Slot has terrain geometry, a level, and episode state. Changing `--num-envs` changes parallelism; review collision isolation, placement, and terrain coverage before increasing it. Discrete terrain instead defaults to a shared terrain atlas. These are task-factory choices, not universal PhantomX defaults.

All difficulty regions are generated at Session initialization. Each control step projects horizontal displacement onto the command direction, converting body-frame commands into the world frame. At reset, progress above the smaller of 80% of commanded distance or half the terrain width promotes one level; insufficient progress (below half the commanded distance) demotes one level. Turning paths count toward distance, and overspeed does not earn extra promotion credit. Promotion beyond the highest level resamples across all levels. Termination and timeout use the same rule. Geometry is not regenerated during an episode. Checkpoints preserve per-Slot curriculum state and its random stream.

## Physical-time training objective

`physics_dt` is **0.005 seconds**. Each control step samples one `step_decimation` in the inclusive range `[1, 7]`, shared across the synchronized Slots. This gives **5–35 ms control intervals with a fixed physics timestep**. `reference_dt_s: 0.02` calibrates reward weights and PPO parameters; it does not force 20 ms steps.

| Quantity | Current rule |
|---|---|
| Continuous rewards and penalties | Multiply the 20 ms reference value by `actual_step_seconds / 0.02` |
| Falls and Worker Slot faults | Charge once per event, without duration scaling |
| PPO discount and GAE | Use `gamma^(actual_step_seconds / 0.02)` and `lambda^(actual_step_seconds / 0.02)`; timeout bootstrap uses the transition discount |
| Episode limit | 20 seconds of simulation time, accumulated from completed solver steps; sparse reset clears only the selected Slots |
| Initial timeout phase | At training startup, including resume, initialize each Slot's first episode clock at a random time in `[0, 20)` seconds to stagger timeouts |
| Action-change penalty | Sum squared differences between consecutive normalized policy actions; do not divide by the control interval |
| Evaluation vibration frequency | Use the actual sampling times of completed control windows |

CartPole retains its fixed-step PPO, rewards, and timeout behavior. The design draws on Isaac Lab's time-integrated rewards and terrain-level resampling, but no Isaac Lab checkout is needed. Matching objective timing does not imply identical Chaos and PhysX trajectories.

## Start and resume a Run

```powershell
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --run-name robust-time
```

Record the printed `[RUN] directory=...` and explicitly select it for evaluation. To resume:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --resume $runDir
```

`uerl runs` lists runs and resumable checkpoints. A Run selects `model_final.pt` when present, otherwise its highest-numbered `rsl_rl/model_<iteration>.pt`. `latest` chooses by time, not objective compatibility.

Checkpoints created before the physical-time objective lack the `phantomx_physical_time_v1` marker and are rejected for resume before UE starts. Terrain checkpoints from before command-direction progress was introduced also lack the state needed for adaptive terrain resume. `uerl train --terrain-level N --resume ...` bypasses restoration of the terrain curriculum while retaining command-curriculum restoration; it does not bypass the objective compatibility check. Earlier terrain checkpoints may also lack the curriculum random stream.

Single-robot playback starts with registered Task curriculum settings instead of restoring per-Slot checkpoint state. Use `--terrain-level N` to evaluate one selected procedural tier. Historical policy behavior cannot establish the effectiveness of the current objective.

## Per-level evaluation

Continuous-terrain levels are numbered **0–7**. `--steps` counts control steps, so equal step counts need not cover equal physical time with variable decimation. Select the actual Run directory:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
0..7 | ForEach-Object {
  uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 `
    --run $runDir --terrain-level $_ --steps 4000 --presentation none
}
```

Compare completed episode counts, success/fall rates, forward velocity, speed error, and body vibration. `success_rate` counts completed episodes without base contact; it does not establish command tracking. A stationary robot can score well on survival. Curriculum mean level and a video are also insufficient on their own.

See [recording](record-video.md) and [test boundaries](../../tests/README.md) for the next steps.
