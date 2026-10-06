# Recording video after training

`uerl play --record` captures UE viewport frames and encodes an MP4 at the selected path.
Playback uses one robot.
It requires a working Unreal Editor and a trained checkpoint.

Recording requires `viewport` presentation and a newly launched Worker.
The CLI rejects other presentation modes and attached Sessions.
The default playback presentation is `viewport`.
Viewport rendering reduces throughput.
Use recordings for inspection and presentation.

## Record a policy

Replace the placeholder with the Run directory printed during training.
Then run:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl play `
  --task UERL-PhantomX-ContinuousTerrain-v0 `
  --run $runDir `
  --terrain-level 0 `
  --record artifacts/walk.mp4
```

| Option | Meaning |
|---|---|
| `--task` | Optional for an explicit Run directory. If supplied, the ID must match the saved Task. Required for `--run latest`. |
| `--run` | Run directory or `latest`. Use `--checkpoint` to select a file instead. |
| `--record` | Output MP4 path |
| `--record-seconds` | Maximum duration. Default: 20 seconds. |
| `--record-fps` | Encoded frame rate. Default: 30 fps. |
| `--controller` | `task` for Task commands, or `player` for keyboard control. Default: `task`. |
| `--terrain-level` | Fixed procedural terrain tier. Without this option, playback uses registered Task curriculum settings. |

For an objective comparison, select an explicit Run directory.
The value `latest` selects by time, not by training objective.
Use [per-level evaluation](phantomx-robust-training.md#per-level-evaluation) to measure performance.
The recorder also writes a JSON evidence file beside the video.

## Keyboard control

Run:

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl play `
  --task UERL-PhantomX-ContinuousTerrain-v0 `
  --run $runDir `
  --terrain-level 0 `
  --controller player `
  --presentation viewport `
  --steps 20000
```

1. Give keyboard focus to the viewport.
2. Hold **W** for forward motion.
3. During forward motion, use **A/D** or **Q/E** to turn.
4. Release W or press **S** for the default standing joint targets.

The supplied Task commands teach forward motion and turns during motion.
Historical policies do not establish reverse motion, lateral motion, or turns without forward motion.
Those behaviors require suitable training and evaluation.

Without `--controller player`, the Task publishes its own commands.

## Historical examples

- [Walking](../media/phantomx-walk.mp4): one robot on continuous terrain, 20 seconds.
- [Terrain](../media/phantomx-terrain.mp4): discrete box terrain, 20 seconds.
