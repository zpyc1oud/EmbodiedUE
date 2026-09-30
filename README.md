# UE RL Engine

<video src="docs/media/phantomx-walk.mp4" controls muted playsinline width="720"></video>

[![Unreal](https://img.shields.io/badge/Unreal-5.8-black.svg)](https://www.unrealengine.com/)
[![Python](https://img.shields.io/badge/python-3.11-blue.svg)](https://docs.python.org/3/whatsnew/3.11.html)
[![Windows](https://img.shields.io/badge/platform-windows--64-orange.svg)](https://www.microsoft.com/en-us/)
[![Version](https://img.shields.io/badge/version-1.0.0-green.svg)](CHANGELOG.md)

UE RL Engine 用 Unreal Engine 5.8 的 Chaos 物理训练机器人，再把训练好的策略放进 UE 游戏里运行。训练和游戏使用同一套机器人资产与策略格式。目前提供 CartPole 和 PhantomX 六足机器人。

上方视频展示 PhantomX 在地形上的一次回放。视频来自历史模型；当前训练效果以[评测报告](docs/diagnostics/phantomx-terrain-ablation-2026-09-24.md)为准。

## 能做什么

- **训练**：用 Python 定义任务，在 UE 里同时运行多台机器人，用 rsl-rl 训练策略。
- **回放**：选一个训练结果，在 UE 视口看机器人行走、用键盘控制，或录制 MP4。
- **部署**：把策略导出为 `.uerlpol2`，导入另一份 UE 5.8 工程，在游戏里的 Skeletal Mesh 上运行。
- **扩展**：增加新机器人时，添加 UE 资产、Python 机器人与任务声明，无需编写机器人专用的 UE 运行时代码。

## 快速开始

以下命令在仓库根目录的 PowerShell 中运行。需要 Windows、Unreal Engine 5.8、Python 3.11 和 [uv](https://docs.astral.sh/uv/)。PhantomX 默认使用 NVIDIA GPU 训练；没有 GPU 时可以给训练命令加 `--device cpu`。

### 1. 安装并选择任务

```powershell
uv sync
uv run uerl tasks --filter PhantomX
```

`tasks` 会列出可用任务。这里选用 `UERL-PhantomX-ContinuousTerrain-v0`：让 PhantomX 在逐渐变难的连续地形上学习行走。想先检查一条较短的训练链路，可选 CartPole（任务列表见下方）。

### 2. 训练

```powershell
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --run-name terrain
```

启动时会看到 `[RUN] directory=...`。把等号后面的路径记下来：那里存放本次训练的配置、日志和模型。`runs/` 是本机生成的目录，不随仓库分发；已有训练结果可用 `uv run uerl runs` 查找。

### 3. 回放或手动控制

先把上一条命令打印的目录填入 `$runDir`。训练时默认同时运行 64 台机器人；回放只有一台，因此用 `--terrain-level 0` 固定它所在的地形等级。

```powershell
$runDir = '<把 [RUN] directory= 后面的路径粘贴到这里>'
uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir --terrain-level 0
```

这条命令让任务自动发出行走指令，所以不按键，机器人也会往前走。要自己控制，改用：

```powershell
uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir --terrain-level 0 --controller player --steps 20000
```

按住 **W** 前进；前进时按 **A/D** 或 **Q/E** 转向；松开 W 或按 **S** 会切换到默认站姿。当前模型不能可靠地倒车、横移或原地转向。更多说明见[回放与录屏指南](docs/how-to/record-video.md)。

### 4. 导出到游戏

```powershell
uv run uerl export --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir
uv run uerl deploy --project <目标工程目录> --demo phantomx --task UERL-PhantomX-ContinuousTerrain-v0 --artifact $runDir
```

第一条命令生成策略文件；第二条把通用运行时和 PhantomX 演示资产接入目标工程，并打印导入策略的命令。给 `deploy` 加 `--import` 才会启动 UE Editor 完成策略资产导入。放置机器人、检查工程和打包的步骤见[游戏部署指南](docs/in-game-deployment-guide.md)。

## PhantomX 现在训练到什么程度

2026-09-24 完成的连续地形基线共训练 1000 次迭代。在每级 3 个 episode 的固定地形评测中，等级 0–6 能持续前进，平均速度为 0.51–0.63 m/s；等级 7 约为 0.06 m/s，基本停在原地。扩大动作幅度或驱动力的完整对照训练没有解决等级 7。数据和实验条件见[训练与消融报告](docs/diagnostics/phantomx-terrain-ablation-2026-09-24.md)。

地形配置只有一份。默认的 64 指同时训练的机器人数量；每台机器人根据同一份配置生成自己的地形。修改机器人数量不会把地形定义变成 64 份。细节与逐级评测命令见[PhantomX 训练指南](docs/how-to/phantomx-robust-training.md)。

## 可用任务

| Task ID | 做什么 |
|---|---|
| `UERL-CartPole-Direct-v0` | 用倒立摆检查训练链路 |
| `UERL-PhantomX-Walk-v0` | 在平地上行走 |
| `UERL-PhantomX-ContinuousTerrain-v0` | 在连续起伏地形上行走 |
| `UERL-PhantomX-DiscreteTerrain-v0` | 在离散障碍地形上行走 |
| `UERL-PhantomX-Pursuit-v0` | 在美术地图里追逐玩家 |

可用 `uv run uerl config --task <TaskID>` 查看任务默认配置；`uv run uerl --help` 查看所有命令与选项。

## 更多文档

| 你想做什么 | 从这里开始 |
|---|---|
| 续训、固定地形等级评测 PhantomX | [PhantomX 训练指南](docs/how-to/phantomx-robust-training.md) |
| 手动控制或录制视频 | [回放与录屏指南](docs/how-to/record-video.md) |
| 接入新机器人 | [新机器人接入指南](docs/how-to/add-a-robot.md) |
| 把策略放进另一份游戏工程 | [游戏部署指南](docs/in-game-deployment-guide.md) |
| 理解系统结构和术语 | [项目说明](docs/UE-RL-Engine-项目说明文档.html) · [术语表](CONTEXT.md) |
| 查看测试方式或全部文档 | [测试说明](tests/README.md) · [文档目录](docs/README.md) |

源代码在 `src/uerl/`，UE 宿主工程和插件在 `engine/`。
