# Recording video after training

`uerl play --record` captures frames from the UE viewport and encodes an MP4 at the requested path. Recording requires `viewport` presentation (the playback default) and a newly launched Worker; non-viewport presentation and attached Sessions are rejected. Playback uses one robot and requires a working Unreal Editor plus a trained checkpoint. Viewport rendering reduces throughput; use it for inspection and presentation.

## Record a policy

Replace the placeholder with the directory printed by training:

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
| `--task` | Optional with an explicit Run directory; when supplied, it must match the saved Task ID. Required with `--run latest`. |
| `--run` | Run directory or `latest`; `--checkpoint` can select a specific file instead |
| `--record` | Output MP4 path |
| `--record-seconds` | Maximum recording duration; default 20 seconds |
| `--record-fps` | Encoded frame rate; default 30 fps |
| `--controller` | `task` for task commands (default), or `player` for keyboard control |
| `--terrain-level` | Fixed procedural terrain tier; otherwise single-robot playback starts with registered Task curriculum settings |

For objective comparisons, name the intended Run explicitly. `latest` selects by time and does not distinguish training objectives. Recordings illustrate behavior; use [per-level evaluation](phantomx-robust-training.md#per-level-evaluation) to measure performance. Recording also writes a JSON evidence file beside the video.

## Keyboard control

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

With the viewport focused, hold **W** to move forward. While moving, use **A/D** or **Q/E** to steer. Release W or press **S** to command the default standing joint targets. The supplied task commands train forward motion and turns while moving; reverse, lateral motion, and in-place turns are not established capabilities of the historical policies. They require appropriate training and evaluation.

Without `--controller player`, the task continues to publish its own commands.

## Historical examples

- [Walking](../media/phantomx-walk.mp4): one robot on continuous terrain, 20 seconds.
- [Terrain](../media/phantomx-terrain.mp4): discrete box terrain, 20 seconds.
