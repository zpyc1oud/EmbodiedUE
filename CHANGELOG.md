# Changelog

## Unreleased

- PhantomX `action_rate` 改回 Isaac Lab 的 `‖a_k−a_(k−1)‖²`（不再除以上一控制间隔的平方），权重按 Isaac Lab 关节目标单位换算为 `2.4e-3`；探索噪声不再是最大单项奖励。训练首个 episode 的 20 秒超时从随机起点计时（含续训），各 Slot 不再同步超时。地形课程按沿指令方向的累计前进量判定升降级，转向阶段准确跟踪不再被误降级，超速也不再更易晋级；此前的地形 checkpoint 不能自适应续训。`uerl train --terrain-level N --resume` 跳过 checkpoint 的地形课程，只恢复指令课程阶段。
- PhantomX 变 decimation 训练按物理时间计算持续奖励、PPO 折扣/GAE 与 20 秒 episode；最高地形等级成功后在全部等级重新抽样，旧目标 checkpoint 拒绝用于新目标续训。评测振动频率使用实际采样时间。
- 产品命令只保留 `uerl`。删除 `uerl-train`、`uerl-play`、`uerl-export`、`uerl-tasks`、`uerl-config` 和只复制演示网格的 `install_demo_robot.ps1`。
- `uerl runs` 列出每次训练 Run 和它能续上的 checkpoint。`--run` / `--resume` 接受 `latest`；完成的 Run 用 `model_final.pt`，中断的 Run 用编号最大的 `rsl_rl/model_<iteration>.pt`。
- `uerl deploy` 安装 runtime、做部署预检，并按 Task 填好 `UERLPolicyImport` 命令；`--import` 才启动 Editor。
- 训练增加 `--num-envs`、`--seed`、`--device`、`--max-iterations`；回放增加 `--seed` 和 `--device`，导出增加 `--device`。README 的示例改成这些默认值下的一行命令。
- CLI 的用户输入错误（Task ID、覆盖路径、Run 目录）打印 `[FAIL]` 一行和下一步，不再抛 traceback；未知 Task 附带最接近的候选。
- Session 默认连接/请求超时改为 120 秒，与 Worker 启动和 Slot 初始化的真实耗时一致；训练不再逐条覆盖该值。
- PhantomX 任务默认 `runner.device: cuda:0`；CPU 训练需要显式覆盖。
- `uerl tasks` 的 ROBOT 列显示机器人资产（如 `SK_PhantomX`）而不是机器人种类，JSON 增加 `robot_asset_path`。
- 说明文档的时钟和参数表改成与任务 YAML 一致；可执行命令只留在 README。
- 删除一次性 `tools/` 脚本及其 tooling 测试；产品入口仍是 `uerl.cli` 与 `scripts/`。
- 删除 RobotArm 训练任务、资产与接入指南。产品机器人保留 CartPole 与 PhantomX。
- README 与 `docs/` 按 Isaac Lab 落地页组织：Key Features、Getting Started、Environments、How-to。
- README 增加 PhantomX 训练后推理录屏。
- README 训练示例改为可复现先前 CartPole YAML 与 PhantomX 正式 Run 的命令。
- 修复 pre-push E2E：Step 写入 `step_decimation`，PIE attach 带锁步物理参数，Worker 启动请求超时 120 秒。

## 1.0.0 — 2026-09-17

在 UE 5.8 Chaos 中训练机器人策略，并用同一份制品在游戏里驱动 Skeletal Mesh。

- 通用机器人 runtime：CartPole、PhantomX、RobotArm 只增加资产和 Python 声明。
- 游戏内部署：`UUERLPolicyComponent` / `BP_UERLPolicyRobot`，编辑器项目体检，`scripts/install_into_project.py` 一键接入目标工程。
- 安装版引擎打包要求目标工程带 C++ Game Target；安装脚本可生成空模块。`package_plugin.py` 只分发编辑器预编译插件。
