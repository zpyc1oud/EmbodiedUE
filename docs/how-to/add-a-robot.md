# Adding a robot

接入一台新机器人 = **UE 资产 + 一份 Python 机器人声明 + 一个 Python Task**。不新增机器人专用引擎代码，不新增动作字段，不新增第二条 reset 路径。

现役例子是 CartPole 与 PhantomX。原则说明见 [项目说明文档 2.6](../UE-RL-Engine-项目说明文档.html#s2-6)。

可以先用脚手架生成待审查文件：

```powershell
uv run uerl new robot phantomx2 --asset /Game/Robots/PhantomX2/SK_PhantomX2
uv run uerl new task walk2 --robot phantomx2 --template phantomx-walk
```

脚手架拒绝覆盖已有文件，也不会偷偷修改显式 registry；生成的 `TODO` 拓扑和 Task math 必须完成后才能登记。脚手架中的 `max_episode_steps: 1000` 是占位值；若新 Task 使用 PhantomX 式变 decimation 训练，应按[连续地形指南](phantomx-robust-training.md)明确设计按物理时间计算的奖励、折扣和 episode。

## 1. UE 资产

在宿主工程放入 Skeletal Mesh、Skeleton 和 PhysicsAsset。结构由资产反射，不在 Python 里复述质量、惯量或关节限位。

PhantomX 的网格路径是 `/Game/Robots/PhantomX/SK_PhantomX`。

## 2. Python 机器人声明

在 `src/uerl/assets/robots/` 增加一份声明：资产路径、关节与刚体名、参考姿态、执行器组、观测选择、重置分布。把它登记进 `src/uerl/assets/robots/__init__.py` 的 `ROBOT_ASSETS`。

对照：

- `src/uerl/assets/robots/cartpole.py`
- `src/uerl/assets/robots/phantomx.py`

## 3. Python Task

在 `src/uerl/tasks/` 增加任务配置、builder 与 registration，并在 `src/uerl/tasks/registry/defaults.py` 里 `register_*`。Task ID 一经注册即可被 `uerl train`、`uerl play` 和 `uerl export` 使用。

对照：

- `src/uerl/tasks/cartpole/`
- `src/uerl/tasks/phantomx/`

## 4. 训练制品

用同一条 CLI 训练并导出 `.uerlpol2`。游戏内部署见 [In-Game Deployment](../in-game-deployment-guide.md)。
