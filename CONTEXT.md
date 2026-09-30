# UE RL Engine Context

本上下文统一 UE RL Engine 的领域语言，描述 Session、Environment、Robot、资产、语义和训练/仿真边界；实现细节留在稳定设计、规格说明与架构决策中。

## 机器人领域

**Robot**:
一个可被训练任务驱动、观测并重置的模拟实体。
_Avoid_: Actor, Model, Agent

**Robot asset**:
定义机器人可模拟结构的资产集合，包括实体层级、身体和关节关系。
_Avoid_: robot model, scene object

**Topology**:
机器人资产中可被稳定引用的身体、关节、父子关系及关节结构限制。
_Avoid_: semantics, configuration

**Robot semantics**:
说明哪些结构参与驱动、观测和重置，以及这些行为如何解释的领域声明。
_Avoid_: topology, asset metadata

**Robot Interface**:
训练侧与仿真侧之间交换机器人结构、动作、观测和重置信息的契约。
_Avoid_: CartPole-specific protocol

## 配置与运行描述

**RobotConfig**:
声明机器人语义的配置，包含执行器、观测清单和重置分布。
_Avoid_: asset description

**RobotSpec**:
将资产 Topology 与 RobotConfig 合并后的可运行机器人描述，包含稳定的动作和观测索引关系。
_Avoid_: raw config, raw topology

**SessionSpec**:
Python 在初始化边界把 Robot、Environment、Slot、physics_dt、闭区间 decimation 和 wire projection 编译成的不可变 Session 契约。decimation 的 `[min,max]` 端点是正 int32，`[N,N]` 表示固定步进。
_Avoid_: mutable runtime state, training source config

**Execution plan**:
仿真侧在 Initialize commit 时由 SessionSpec 编译出的不可变热路径计划；它缓存 body、constraint、column 和单位转换索引。
_Avoid_: plugin framework, user configuration

**Canonical default state**:
Initialize 时为每个 Slot 捕获的完整机器人默认物理状态；reset 总是先恢复它，再应用本次覆盖值。
_Avoid_: current state, partial reset fallback

**Initial episode state**:
Session Ready 后由训练侧对全部 Slot 执行一次正常 reset 得到的 Post-Reset State；它应用与后续 episode 相同的 Reset distribution，并且是策略可见的第一份状态。Initialize 返回的 Canonical default state 只完成运行时建立与协议验收，不能直接作为首个 episode。
_Avoid_: asset spawn pose, startup grace period

**Actuator**:
把一个动作维度转换为某个可驱动关节物理作用的机器人部件。
_Avoid_: action, controller

**Action target**:
某个 Actuator 在当前控制时刻收到的数值目标；它不携带关节名称。
_Avoid_: policy output

**Observation**:
训练任务从机器人当前物理状态读取的一项有明确类型、单位和顺序的值。
_Avoid_: sensor plugin

**Terrain-height scan**:
以 Robot root 为参考、沿世界水平面建立 footprint 的固定 7x5 地形高程 primitive；每个值是射线命中点 Z 减 root.Z 的米制高度 `(hit.Z-root.Z)/100`。它由 Robot Runtime 在 Observation 采集边界计算，供 Task 作为外感知输入使用，不复制 terrain 配置，也不暴露 UE 世界坐标。训练 Shared World 通过 Environment 记录的 terrain owner 白名单限定射线命中，Slot-isolated 使用所属 Slot 的碰撞 query channel；部署显式使用 blocking WorldStatic 查询。成功命中会缓存世界命中点，短暂失联时按当前 root/body 重新计算；初始或 reset 没有命中则返回可诊断错误。
_Avoid_: terrain seed, heightfield mesh, world-space elevation

**Reset distribution**:
描述机器人每次重置时初始状态如何采样的规则。
_Avoid_: terrain curriculum

**Contact observation**:
表示指定身体在控制窗口末、最后一个已完成 solver step 的几何支撑状态，
取值为 `0/1`。它只在窗口末查询，不累计窗口早期的支撑或 `OnComponentHit` 事件。
Slot-isolated 只计所属 Environment，Shared World 计 `WorldStatic` 地面；地形高度扫描
的 owner 白名单不限制 contact。
_Avoid_: contact force, collision manifold

**Contact-force observation**:
表示指定身体在同一个窗口末已完成 solver step 的净冲量按该 solver step 秒数换算
得到的牛顿值，即 `|I| / (100 * SolverStepSeconds)`。它不是控制窗口内的峰值，
也不使用游戏 DeltaTime、控制窗口时长或 clamp 后的 observation dt；没有新的物理
结果时不复用旧冲量。
_Avoid_: window force maximum, control-frame impulse

## 运行边界

**Slot**:
一次并行仿真中的单个机器人实例及其对应的任务状态。
_Avoid_: world, environment process

**Session**:
一个 Worker bridge 在一个 UWorld 中运行的一次不可变训练连接，拥有固定数量的 Slot、execution plans、physics_dt 和 decimation 范围；每个 Step 额外携带一个由该范围约束的 `step_decimation` 标量。
_Avoid_: episode, task

**Environment**:
为 Slot 提供放置、地面和地形条件的仿真上下文；它不拥有 Robot semantics、机器人控制或训练任务数学。
_Avoid_: robot, task

**Collision scope**:
Environment 声明其碰撞几何属于各自 Slot，还是由所有 Slot 共享的 Session 级边界。
_Avoid_: collision mode, spacing policy

**Slot-isolated Environment**:
每个 Slot 各自拥有碰撞几何的 Environment；该几何只与同 Slot Robot 交互。
_Avoid_: cloned world, private scene

**Shared World Environment**:
多个 Slot Robot 共享同一份 Environment 地形几何、但彼此保持 Slot isolation 的 Environment；共享几何可以来自已加载的 World Map，也可以在该 World 中程序化生成。
_Avoid_: Shared Map Environment, global environment, multi-agent environment

**Terrain source**:
Environment 获得地形几何与 Ground frame 的来源；当前来源是 procedural terrain 或 authored World Map，来源不改变 Environment 的 collision scope。
_Avoid_: collision mode, map mode

**Terrain curriculum**:
Environment 在 Session 初始化时生成全部难度区域，训练侧只在 episode reset 时依据上一 episode 沿指令方向的累计前进量和累计指令距离调整难度；一般晋级或降级移动一级，最高级再次晋级时在全部等级中重新抽样。终止和超时遵循相同规则。它不在行走中途修改几何，也不重新生成地形。
_Avoid_: Reset distribution, runtime terrain mutation

**World Map identity**:
一次 Session 实际加载的 UE World 长包名，是 resolved config、Worker projection 和 Run manifest 共同确认的运行身份。
_Avoid_: launch argument, level filename

**Ground frame**:
一个 Slot 的局部地面参考系；root/body 的 Slot-local pose 和 velocity 以它为参考。
_Avoid_: world transform, terrain random seed

**Slot isolation**:
不同 Slot 的 robot collision 和 contact 不能互相影响的运行不变量；SlotIsolated 还隔离 terrain 和 query。空间错开放置本身不构成隔离。
_Avoid_: spacing heuristic

**Task**:
训练侧对动作、观测、奖励、终止和 episode 规则的定义。
_Avoid_: Worker, Robot provider

**Worker**:
仿真侧编排 Slot 生命周期、初始化、固定步进、状态采集和重置的运行实体。
_Avoid_: trainer, task

**Control frame**:
训练侧提交一次 Action target 和本次 `step_decimation`，仿真侧在该数量物理子步内保持 target，随后返回一次 Observation 的完整控制周期。物理窗口为 `physics_dt × step_decimation`；制品的 `ArtifactTiming` 保存 physics_dt 与 decimation 范围，并派生最小/最大控制间隔。
_Avoid_: physics substep

**Control frame length**:
第 `k` 个控制帧的实际长度为 `dt_k = physics_dt × d_k`，其中 `d_k` 是该 Step 携带的 `step_decimation`。时间序列按 `o_k → a_k → physics(d_k) → o_{k+1}` 解释，返回的观测间隔属于刚完成的帧；Initialize 和 Reset 后的首个观测使用 `DtMin` 作为起始约定。
_Avoid_: nominal control period, wall-clock latency

**Slot fault**:
只破坏一个 Slot 物理状态、可通过显式 sparse reset 尝试恢复的运行故障。
_Avoid_: terminated, truncated

**Session-fatal fault**:
破坏协议、资产、执行计划、固定帧关系或 Slot 隔离，必须停止整个 Session 的故障。
_Avoid_: automatic retry, task termination

**Training side**:
拥有 Robot semantics、动作映射、动作解算和 Reset distribution 的一侧。
_Avoid_: physics executor

**Simulation side**:
根据资产提供 Topology、执行数值动作、采集选定 Observation 并应用重置值的一侧。
_Avoid_: robot owner

## 配置归属与训练宿主

**Training configuration**:
描述一次训练运行的任务、Worker、Environment、runner 参数，以及所使用的 Robot asset 和 semantics 配置引用。它不重复 Robot asset 的物理事实。
_Avoid_: robot definition

**Robot semantics configuration**:
描述训练如何使用 Robot，包括 actuator、控制增益、参考姿态、动作缩放、观测清单和 Robot reset 分布；它不描述质量、惯量、几何或关节限制。
_Avoid_: asset metadata

**Worker projection**:
由训练侧将已解析的 RobotSpec 投影成仿真侧可消费的运行配置。它是内部运行产物，不是用户维护的第二份 Robot 配置。
_Avoid_: source configuration

**Training UE host**:
随 UERL 交付、承载 Worker 和项目级 Chaos 基线的专用 UE 工程。训练宿主拥有全局物理配置，插件负责读取和校验。
_Avoid_: plugin installer

**Chaos baseline**:
训练宿主在物理场景创建前必须生效的固定步长相关全局物理设置。
_Avoid_: per-robot semantics

**Deployment physics gate**:
部署启动前对实际 World、Chaos solver、同步子步设置和 artifact timing 的只读判定；它要求同步子步有效，不复用训练侧关闭子步的 Worker gate，也不修改宿主设置。
_Avoid_: training physics gate, configured time as completed time

**Completed solver clock**:
在同一 World 的安全完成点读取 Chaos solver 的 `GetSolverTime`、当前 frame 和 `GetLastDt`；只有 frame/time 实际推进才产生新的完成样本，暂停或未求解不伪造推进。
_Avoid_: game DeltaTime, configured physics window
