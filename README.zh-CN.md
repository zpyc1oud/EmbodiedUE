# EmbodiedUE：面向 Unreal 游戏的机器人训练与部署平台

[English](README.md) | **简体中文**

为 **Unreal Engine 游戏开发者**提供机器人训练与部署工具。
在 **UE 5.8 / Chaos** 中训练控制策略，再把策略部署到游戏里的物理机器人上。

我们的目标是让机器人根据自身状态、环境和游戏指令调整动作。
开发者定义机器人、训练任务和游戏指令；策略控制关节，Chaos 模拟刚体运动与接触。
游戏中的“自适应”指策略根据输入作出响应，不表示游戏运行时会在线训练新的权重。

## 从训练到游戏

1. **准备机器人。** 配置刚体、关节、碰撞几何和执行器。
2. **定义行为。** 用 Python 定义观测、指令、奖励和训练条件。
3. **评估效果。** 测量指令跟踪、稳定性，以及目标条件下的表现。
4. **接入游戏。** 导出策略，将 UE 运行时连接到游戏指令。

游戏逻辑决定机器人要做什么，策略负责已经训练过的物理控制任务。
训练使用 Python 和 **rsl-rl**；游戏运行导出的网络。
训练与部署共享观测和动作执行计划，并使用同一种物理后端。

CartPole 是入门示例，PhantomX 六足机器人是当前的运动控制与游戏部署示例。
平台希望让其他机器人也能复用这些接口。
Isaac Lab 是任务配置、训练流程和测试设计的参考；项目的重点是 Unreal 游戏中的物理机器人控制。
Isaac Sim 和 Isaac Lab 不是运行依赖。

[步行演示](docs/media/phantomx-walk.mp4) · [地形演示](docs/media/phantomx-terrain.mp4) · [项目定位](docs/product-direction.md)

视频展示历史策略的表现。新策略需要在目标场景中单独评估。

## 项目状态

项目仍处于早期开发阶段，已包含训练、评估、导出和游戏内推理流程。
在不同游戏场景中提供可靠的自适应行为，仍是正在推进的目标。
训练结束或模型导入成功，不等于机器人已经达到预期行为。

接口、策略格式和操作流程可能变化，不保证向后兼容。
发布前还需要验证策略质量、目标场景表现，以及独立环境下的构建和打包。
详见[发布准备](docs/release-readiness.md)。

Egypt 游戏场景需要自行完成[可选 Fab 内容安装](docs/how-to/optional-egypt-demo.md)。
请通过自己的账号获取 [Stylized Egypt](https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd)，以页面实际条款为准。
该场景的源资产不在当前源码树中。
默认游戏地图是空的 Engine Entry 地图；使用 Egypt 演示时，需要明确打开已安装的地图。
CartPole 与程序化地形任务不依赖这份可选内容。

## 当前包含的能力

- UE 世界中的按请求推进物理与批量机器人实例。
- CartPole 平衡，以及 PhantomX 步行、地形和追踪任务。
- PhantomX 固定 5 ms 物理步长，每次动作保持 1–7 个物理步。
- 通过 `uerl` 完成训练、检查 checkpoint、回放、录制、导出与部署。
- `.uerlpol2` 策略包：包含观测与动作计划、时序、机器人元数据，以及供 UE NNE 推理的 ONNX 网络。
- Python、协议、跨语言数值一致性、UE Automation 和端到端测试。

## 快速开始

### 环境要求

完整训练与部署流程面向 **Windows x64 和 UE 5.8**。
其他 UE 版本及 Linux/macOS 上的 UE 执行不在当前验证范围内。
纯 Python 配置检查和测试可以不启动 UE。

| 工具 | 用途 |
|---|---|
| Git、Git LFS | 获取源码及 `.uasset`、`.umap` 资产 |
| UE 5.8 及对应 Windows C++ 工具链 | 构建 `UERLHostEditor Win64 Development`，运行 Chaos Worker |
| Python 3.11、`uv` | 安装 Python 包及锁定依赖 |
| NVIDIA GPU 与兼容 CUDA 12.8 PyTorch 的驱动 | 默认在 `cuda:0` 训练 PhantomX；`--device cpu` 可选择 CPU 策略执行 |

Chaos 仿真在 UE 中运行。选择 CPU 策略执行仍然需要 UE。
使用 UE 5.8 支持的 C++ 编译器和 Windows SDK。
项目锁定 `torch==2.11.0+cu128`、`torchvision==0.26.0+cu128` 和 `rsl-rl-lib==5.4.2`。
目前没有已验证的最低内存、显存或吞吐保证。

宿主项目启用 `UERLEngine`、`ProceduralMeshComponent` 和 `NNERuntimeORT`。
构建前请确认引擎提供后两个插件。遇到问题可查阅[故障排查](docs/troubleshooting.md)。

### 安装与构建

以下命令在仓库根目录的 **PowerShell** 中执行。替换为实际安装路径。

```powershell
git lfs install
git lfs pull
uv sync --locked

$ueRoot = 'C:\Program Files\Epic Games\UE_5.8'
$project = (Resolve-Path 'engine/UERLHost.uproject').Path
& "$ueRoot\Engine\Build\BatchFiles\Build.bat" UERLHostEditor Win64 Development $project -WaitMutex
```

LFS 指针文件不能作为 UE 资产加载。训练前先确认资产下载完成、Editor 构建成功。
Unreal Engine 需要自行安装，仓库不授予引擎或第三方内容的再分发权利。

### 不启动 UE，先检查任务

```powershell
uv run uerl tasks
uv run uerl config --task UERL-PhantomX-ContinuousTerrain-v0
uv run uerl check task UERL-PhantomX-ContinuousTerrain-v0
```

Task 检查验证 Python 声明，不验证 UE 二进制、内容加载或 GPU 可用性。
`uv run uerl check host` 提供静态 CartPole 宿主文件检查。
使用[宿主配置](docs/how-to/ue-host-profile.md)保存引擎与项目路径。

### 运行最小训练验证

Editor 构建成功后执行：

```powershell
uv run uerl train --task UERL-CartPole-Direct-v0 --num-envs 2 --max-iterations 1 --device cpu --run-name smoke
```

该命令会启动 UE 并运行一轮训练，用于检查基本流程。
它不用于证明策略已经学会任务。

`train`、`play` 和 `export` 的启动模式使用宿主配置中的路径。
`--ue-executable` 和 `--project` 可以分别覆盖相应路径。
这些命令不会自动读取 `UE_ROOT`。

### 训练 PhantomX

```powershell
uv run uerl train --task UERL-PhantomX-ContinuousTerrain-v0 --run-name terrain
```

命令会输出 `[RUN] directory=...`。
请保留该 Run 目录中的配置快照、日志和 checkpoint。
生成的 `runs/` 内容不随源码分发。

连续地形任务默认使用 64 个 Slot，平地步行任务默认使用 512 个 Slot。
一个 Slot 表示一个机器人实例及其任务状态。
调整并行数量或恢复旧 checkpoint 前，阅读[训练与评估指南](docs/how-to/phantomx-robust-training.md)。

### 评估与录制

```powershell
$runDir = 'runs/UERL-PhantomX-ContinuousTerrain-v0/<run-directory>'
uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir --terrain-level 0 --steps 4000 --presentation none
uv run uerl play --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir --terrain-level 0 --record artifacts/walk.mp4
```

将 `<run-directory>` 替换为训练输出的目录名。
回放使用一个机器人，并从已注册 Task 的课程设置开始。
`--terrain-level` 用于选择程序化地形等级。
控制间隔可变时，相同控制步数不代表相同仿真时长。

评估时同时查看实际速度、指令误差、摔倒和完成的 episode 数。
机器人不摔倒，并不代表能够跟踪运动指令。
录制及键盘控制请看[回放与视频指南](docs/how-to/record-video.md)。

### 导出并接入游戏

```powershell
uv run uerl export --task UERL-PhantomX-ContinuousTerrain-v0 --run $runDir
uv run uerl deploy --project '<target-project-directory>' --demo phantomx --task UERL-PhantomX-ContinuousTerrain-v0 --artifact $runDir
```

导出需要初始化 UE Session，不是离线 checkpoint 转换。
部署会复制运行时、修改目标项目设置，并输出导入命令。操作前保留目标项目备份。
目标 Editor 构建成功后，可以添加 `--import` 执行导入；只读检查使用 `deploy --check`。

游戏代码提供目标与指令，策略负责对应的物理控制。
有关 Blueprint、物理设置和打包，请看[游戏部署指南](docs/in-game-deployment-guide.md)。
在目标地图中按[部署验证流程](docs/how-to/policy-deployment-validation.md)检查结果。

## 参考任务

| Task ID | 用途 | 默认 Slot 数 |
|---|---|---:|
| `UERL-CartPole-Direct-v0` | 平衡与训练流程验证 | 64 |
| `UERL-PhantomX-Walk-v0` | 平地运动控制 | 512 |
| `UERL-PhantomX-ContinuousTerrain-v0` | 八级连续地形 | 64 |
| `UERL-PhantomX-DiscreteTerrain-v0` | 六级离散障碍 | 64 |
| `UERL-PhantomX-Pursuit-v0` | 在本地安装的可选 Egypt 地图中追踪目标 | 1 |

用 `uerl config --task <TaskID>` 查看实际解析后的默认配置。
机器人声明位于 `src/uerl/assets/robots/`。Task 工厂可以覆盖基础 YAML 设置。
详见[配置指南](docs/configuration.md)。

## 文档与贡献

[开发者学习路径](docs/README.md#build-your-first-game-robot) · [文档索引](docs/README.md) · [添加机器人](docs/how-to/add-a-robot.md) · [外部任务](docs/how-to/external-tasks.md) · [架构](docs/architecture.md) · [术语](CONTEXT.md)

安装依赖后，可运行纯 Python 测试：

```powershell
uv run pytest -q
```

UE Automation、端到端训练、录制和打包需要 Windows/UE 宿主环境。
根据改动范围选择[验证层级](docs/contribution-workflow.md)，并阅读[测试编写指南](docs/how-to/write-tests.md)。
软件测试通过与策略行为达到预期，是两项不同的检查。

参与开发请阅读[贡献指南](CONTRIBUTING.md)。
其他文档：[测试](tests/README.md)、[安全](SECURITY.md)、[变更记录](CHANGELOG.md)。

## 许可证与资产

项目原创代码、文档与配置采用 [Apache-2.0](LICENSE)。
第三方代码和内容保留各自权利。
此许可证不自动覆盖 Unreal Engine、Fab/Epic 资产，以及没有明确项目授权声明的机器人资产、策略文件和媒体。
详见[第三方声明与资产清单](THIRD_PARTY_NOTICES.md)。

Unreal Engine 需按 Epic 的适用协议单独获取。
`Stylized_Egypt` 来自 Fab，其原始资产已排除在当前源码树之外；历史 Git/LFS 副本仍是公开发布前需要处理的问题。
其他内容的来源与分发权利也仍在审查中，详见[发布准备](docs/release-readiness.md)。
