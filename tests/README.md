# tests · 分层与测试类型

本目录只保存测试、mock、fixture 和 E2E 编排，不定义产品接口。测试按系统层与测试类型组织；产品运行时和 Task 由 `src/uerl` 提供。

## 目录约定

```text
tests/
├── python/
│   ├── unit/          # Task 数学、配置与纯 Python 产品组件
│   └── integration/   # Session seam 与多组件组合行为
├── protocol/
│   ├── unit/          # U4 frame、严格 JSON、JCS 与 layout golden vectors
│   ├── integration/   # Python IPC client + mock Worker
│   └── support/       # U4 test-only protocol oracle 与 Socket client
├── e2e/               # Python/UE 真实 UnrealEditor-Cmd 链路与 support
├── tooling/           # 安装、打包与 archive-checker 行为
└── ue/
    └── unit/          # UE 原生 Automation 测试索引
```

UE 原生 C++ Automation 测试必须随 UBT module 编译，因此实际文件放在各模块的 `Private/Tests/Unit/`，由 `tests/ue/unit/README.md` 索引；不会在 Python 测试目录复制一份不可编译的 C++ 文件。

## 当前覆盖

| 层 | 当前测试类型 | 已实现行为覆盖 |
|---|---|---|
| Python | Unit / Integration | Cart-Pole、PhantomX Task 数学与配置；Session、Protocol seam、Manifest、Registry、DirectEnv、RobotConfig、RobotSpec、索引动作与观测 schema；策略制品容器（含 `robot_runtime`）与导出 |
| Protocol | Unit / Integration | 48-byte Header、严格 JSON/JCS、安全整数、Layout hash、零 padding、非法 flags 与 TCP 分片重组 |
| UE | Unit / E2E | Transport layout、Worker seed/safety/binding/physics gate、variable-dt Step contract、Session CLI；真实 Cart-Pole；`UERLRobot` 拓扑、观测计划、运动学、窗口末 contact、root 相对 35 点 terrain、provider 配置与真实内容 smoke；`UERLPolicy` 制品/plan/NNE 与 `UERLPolicyEditor` 导入/重导入；`UERL.Unit.Policy.DeployParity` 端到端部署一致性，UE NNE 与 Python ONNX 均按 `atol/rtol=2e-5` 对 reviewed expected 校验；`UERL.Unit.Policy.PhysicsGate` 部署同步子步判据；`UERL.Integration.Policy.Contact` 末 solver step 采样与控制器接线；`UERL.Integration.Policy.Ground` 两侧 terrain 查询、缓存与错误传播；`UERL.Integration.Policy.Clock` 实际完成 solver 时钟与暂停；`UERL.Integration.Policy.Controller` 部署控制循环；`UERL.Integration.Policy.Component` 资产导入、命令锁存与显式 Start/Stop |
| Cross-lang | Parity | 算子 / 动作 plan / 制品 / **deploy**（`tests/parity/cases/deploy/`，Python `ReferencePolicyRunner` ↔ UE plan+NNE+action） |

用例数量刻意保持较小：每条已实现关键链路保留一个代表性集成/E2E 用例，边界语义在纯单元测试中覆盖。

## 运行

```powershell
# 快速 Python 测试；真实 E2E 不进入 pytest 默认收集
uv run pytest -q

# 完整默认验证：Python + UE Automation + UE E2E
uv run python scripts/run_all_tests.py

# 快速单元测试
uv run pytest tests/python/unit tests/protocol/unit tests/tooling -q

# Python / mock Worker 集成测试
uv run pytest tests/python/integration tests/protocol/integration -q

# 真实 UE E2E（U4 SocketBridge + U5 运行形态）；脚本负责启动门禁与 UE 前置检查
uv run python scripts/run_e2e.py --suite ue

# 全部真实 UE E2E（P1/P2/Terrain、U4/U5 UE Worker）
# all 缺少 UE 运行时会返回非零，不会静默 skip
uv run python scripts/run_e2e.py --suite all

# P2 DirectEnv typed Session + real UE lifecycle
uv run python scripts/run_e2e.py --suite p2

# Product Cart-Pole + RSL-RL real-UE smoke training
uv run uerl train `
  --task UERL-CartPole-Direct-v0 `
  --num-envs 2 `
  --max-iterations 1 `
  --run-dir runs/cartpole_smoke

# 直接运行当前 UE E2E 编排器
uv run python -m tests.e2e.support.worker_runner
uv run python -m tests.e2e.support.phase1 all

# 只运行 U4 破坏性协议门禁
uv run python scripts/run_e2e.py --suite ue -k "u4_005 or u4_006"

# 只运行 U5 展示、ownership、失败收口和真实 PIE attach 门禁
uv run python scripts/run_e2e.py --suite ue -k "u5_007 or u5_008 or u5_009 or u5_010"
```

Active Step 取消用例使用仅在 `WITH_DEV_AUTOMATION_TESTS` 构建中启用的 post-physics 延迟参数，确定性地让 deadline 落在两个物理帧之间；该参数不是 Worker 产品配置或 wire Config。

UE Automation：

```powershell
& "<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" `
  engine/UERLHost.uproject /Engine/Maps/Entry `
  -ExecCmds="Automation RunTests UERL.Unit+UERL.Integration.Worker.SlotCollision+UERL.Integration.Worker.SharedWorldCollision+UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_003+UERL.Integration.Policy.Contact+UERL.Integration.Policy.Ground+UERL.Integration.Policy.Clock+UERL.Integration.Policy.Controller+UERL.Integration.Policy.Component;Quit" `
  -unattended -nullrhi
```

## 测试规范

1. 只测试当前已经实现且存在可执行入口的行为；未实现功能不得创建测试、预留测试目录、AC/VC 或性能门禁。
2. 测试名包含 AC 编号和行为语义，按 Arrange / Act / Assert 组织。
3. `[VERIFY]` 输出只记录可观察结果，不替代断言；真实 UE E2E 必须明确当前原型边界。
4. 单元测试不依赖外部进程；集成测试只用 test-only mock；E2E 才启动真实 UE。
5. 只 mock 外部依赖，不 mock 被测逻辑；每个测试独立、可重复、自动判定 pass/fail。

Manager 测试参考仓库内 `references/IsaacLab` 的小型确定性环境：用固定输入验证每个 term 的结果，再在 `DirectEnv` seam 验证终止、奖励、课程、复位、指令、interval 事件和下一次观测的顺序。奖励数值用独立常量作预期；稀疏 Slot 使用稳定 Slot ID 检查累计与复位；不同事件 term 使用不同固定间隔检查计时互不串扰。UE Reset event 先生成 Session Reset payload，interval event 在 Reset 后执行；Chaos 数值只由 UE Automation 和真实 E2E 验证。

PhantomX 变 decimation 测试覆盖 5 ms / 35 ms 的流量奖励、离散失败成本、20 秒仿真时间终止、首个 episode 的随机超时起点、稀疏 Slot 计时复位、PPO 逐 transition 折扣与 timeout bootstrap、评测振动频率、沿指令方向前进量的地形升降级（转向、超速、原地不动），以及最高地形等级的随机回卷、checkpoint 延续和固定等级续训只恢复指令课程。旧目标 checkpoint 在 Worker 启动前拒绝续训；固定步长 CartPole 保持原有 PPO 路径。

## 已验证边界

当前记录的 UE 5.8 / Windows 条件下，已验证 Transport / Worker / CartPole 模块拆分、请求驱动固定 Chaos 子步、物理门禁拒绝、typed wire Config、Schema / Layout negotiation、Ready 门禁、Header-before-Payload 校验、Socket binary batch、稳定协议错误码、timeout 时 Active Step 取消、半关闭 / Ready 后断连 fail-fast、真实 Cart-Pole 动作与稀疏 Reset、NONE / VIEWPORT 同轨迹、PROCESS / ATTACHED 生命周期、Worker Fatal 资源收口、真实 Editor / PIE attach / reattach，以及产品 DirectEnv 语义。

## P1 Python import boundary

P1 的真实 UE Session 由 E2E runner 显式启动：

```powershell
uv run python scripts/run_e2e.py --suite p1
```

P1 / P2 的产品代码在 `src/uerl`。测试经 `pyproject.toml` 的 `pythonpath = ["src", "."]` 导入该包。`tests/protocol/support/` 只作为 test-only wire oracle，不被 `src/uerl` 导入，也不是训练入口。
