# Recording video after training

UE RL Engine 从 UE 视口抓帧并编码为 MP4。录屏是 `uerl play --record`，它会强制 `viewport` 呈现并写入指定输出路径。播放固定一台机器人。

需要本机有可用的 Unreal Editor 与一份训练好的 checkpoint。开启视口会降低吞吐，只用于展示，不用于正式训练。

## Command

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<新目标 Run 目录名>' # 用训练输出的目录替换
uv run uerl play `
  --task UERL-PhantomX-ContinuousTerrain-v0 `
  --run $runDir `
  --terrain-level 0 `
  --record artifacts/walk.mp4
```

常用参数：

- `--task`：注册表中的 Task ID；README 录屏用连续地形任务
- `--run`：Run 目录或 `latest`；`--checkpoint` 仍可指向一个具体文件
- `--record`：MP4 路径
- `--record-seconds`：最长录制时长，默认 20
- `--record-fps`：输出视频帧率，默认 30
- `--controller`：`task`（默认，任务自己的指令）或 `player`（按住 W 前进；S 刹停；按住 W 时 A/D 或 Q/E 转向；松开 W 切换到默认关节目标）。当前策略没有可靠的倒车、横移或原地转向能力。
- `--terrain-level`：程序化地形等级。用训练 checkpoint 播放单机器人时应显式指定；0 是最低等级

比较新物理时间目标的策略时，给 `--run` 指定新目标 Run 目录；`latest` 只按时间选择，不区分历史目标。录屏用于展示，效果评估应按 [PhantomX 连续地形指南](phantomx-robust-training.md)逐级运行。

## 手动播放

使用已训练的 PhantomX Run 时，显式选择 `player` 控制器：

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<训练 Run 目录名>'
uv run uerl play `
  --task UERL-PhantomX-ContinuousTerrain-v0 `
  --run $runDir `
  --terrain-level 0 `
  --controller player `
  --presentation viewport `
  --steps 20000
```

按住 W 前进；前进时按 A/D 或 Q/E 转向。松开 W 或按 S 时，播放控制器把动作目标设为机器人默认站姿。当前 checkpoint 的训练指令只覆盖前进和伴随前进的转向；倒车、横移和原地转向需要专门训练，不能用这份模型可靠完成。未指定 `--controller player` 时，`task` 控制器会继续自动发布训练任务的行走指令。

## Examples in this repository

仓库 README 使用的两段推理录屏：

- [../media/phantomx-walk.mp4](../media/phantomx-walk.mp4) — 连续起伏地形，单实例 20 秒
- [../media/phantomx-terrain.mp4](../media/phantomx-terrain.mp4) — 离散箱体地形，20 秒
