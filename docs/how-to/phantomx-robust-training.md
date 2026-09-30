# PhantomX 连续地形训练与评测

本页说明 `UERL-PhantomX-ContinuousTerrain-v0` 当前的训练目标。平地、离散地形和追逐任务复用 PhantomX 的物理时间奖励与 episode 规则；地形等级课程只用于配置了程序化地形的任务。

## 地形配置与 64 Slot

连续地形的难度定义只有一份，来自 [`configs/environments/terrains/phantomx/continuous.yaml`](../../configs/environments/terrains/phantomx/continuous.yaml)：8 个等级、每级的生成参数和地形尺寸。Run 中的 `resolved_config.json` 保存了整次运行的配置，因此同时包含 `worker.slot_count: 64` 和这份地形定义。这里的 64 是默认并行机器人 Slot 数，不是 64 份地形配置。

当前连续地形任务默认采用 Slot 独立的地形路线，并预设 8×8 的 Slot 放置网格。每个 Slot 由同一份地形定义生成独立的几何，拥有自己的地形等级和 episode 状态。调整 `--num-envs` 会改变并行实例及其地形几何实例的数量，不会复制地形定义；若要超过预设的 64 Slot，还需核定碰撞隔离、放置网格和地形覆盖范围。

地形在 Session 初始化时生成全部等级，episode 中途不改几何。每个控制 Step 把机身水平位移投影到指令方向（机体系指令按当前朝向转到世界系），累加成该 episode 的前进量。重置时，前进量超过累计指令距离的 80%（或地形半宽，取较小者）晋级一级，不足一半降级一级；转向路径按路径长度计，超速不会比准确跟踪更容易晋级。最高级再次满足晋级条件时，在全部等级中重新抽样。随机流及每个 Slot 的等级随 checkpoint 保存。

## 物理时间训练目标

PhantomX 的 `physics_dt` 为 0.005 秒，每次控制 Step 的 `step_decimation` 在闭区间 `[1, 7]` 中取值，所以控制窗口为 5–35 ms。配置中的 `reference_dt_s: 0.02` 是奖励权重和 PPO 参数的参考间隔，不要求每步恰好 20 ms。

| 项目 | 当前规则 |
|---|---|
| 持续奖励与惩罚 | 每项的 20 ms 参考值乘以 `实际 Step 秒数 / 0.02` |
| 跌倒与 Worker Slot 故障 | 每次事件只计一次，不按 Step 时长放大 |
| PPO 折扣与 GAE | 每步使用 `gamma^(Step 秒数 / 0.02)` 和 `lambda^(Step 秒数 / 0.02)`；超时 bootstrap 使用该步折扣 |
| Episode 上限 | 20 秒仿真时间；按已完成 solver steps 累计，稀疏重置只清零对应 Slot。训练开始（含续训）时每个 Slot 的首个 episode 从 [0, 20) 秒的随机起点计时，错开各 Slot 的超时 |
| 动作变化率 | 惩罚相邻两次策略动作之差的平方和，不除以控制间隔 |
| 评测振动频率 | 使用每个控制窗口的实际采样时间 |

CartPole 保持固定步长的原有 PPO、奖励与超时规则。Chaos 与 Isaac Lab 的 PhysX 接触求解仍可能产生不同轨迹；上述规则对齐的是训练目标的时间语义。

本地 Isaac Lab 对照中，[`TerrainImporter.update_env_origins`](../../references/IsaacLab/source/isaaclab/isaaclab/terrains/terrain_importer.py)在最高级晋级时重新抽样，[`RewardManager.compute`](../../references/IsaacLab/source/isaaclab/isaaclab/managers/reward_manager.py)按 `dt` 积分奖励。本项目因为每个 Step 的 decimation 可变，还对 PPO 折扣、GAE 与 episode 时长使用实际物理时间。

## 新 Run 与 checkpoint

先预览配置，再开始新的训练 Run：

```powershell
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --run-name robust-time
```

命令会打印 `[RUN] directory=...`。记录这个目录，后续续训和评测显式选它。这次物理时间目标实施前生成的 PhantomX checkpoint 没有新目标标记；`uerl train --resume` 会在启动 UE 前拒绝它。新目标的 checkpoint 带 `phantomx_physical_time_v1` 标记，可以作为同目标续训来源。旧连续地形 checkpoint 还缺少新的课程随机流状态；单机器人回放无论使用新旧 checkpoint，都应显式固定 `--terrain-level`，跳过 64 Slot 课程状态恢复。地形课程改为按指令方向前进量判定之前的地形 checkpoint 缺少前进量状态，不能自适应续训；`uerl train --terrain-level N --resume` 跳过 checkpoint 中的地形课程，只恢复指令课程阶段。旧策略表现不能证明新目标的效果。

续训时用新目标 Run 的实际目录替换占位值：

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<新目标 Run 目录名>'
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --resume $runDir
```

## 分级评测

`uerl play --terrain-level` 固定一次评测的地形等级，等级编号为 0–7。训练 Run 有 64 Slot 的课程状态，而播放只运行一个机器人，所以当前单机器人地形回放应显式给出等级，跳过课程状态恢复。`--steps` 计控制 Step，不是仿真秒数；变 decimation 时，相同 Step 数不保证相同物理时长。用新 Run 的实际目录替换下面的占位值：

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<新 Run 目录名>'
0..7 | ForEach-Object {
  uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 `
    --run $runDir --terrain-level $_ --steps 1000 --presentation none
}
```

逐级查看成功率、跌倒率、速度误差与机身振动频率。评测输出中的 `success_rate` 只统计完成 episode 且没有 base contact 的比例，仍要结合速度误差判断是否按指令行走；课程平均等级或一段录屏也不足以证明新策略效果。录屏命令见 [Record Video](record-video.md)，测试边界见 [tests/README.md](../../tests/README.md)。
