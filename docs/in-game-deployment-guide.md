# UERL 游戏内策略部署指南

本指南把 `PhantomXContinuousTerrain-116` 策略加载到 UE 5.8 游戏里的真实 Skeletal Mesh Actor。运行时只依赖已导入的 `UUERLPolicyArtifactAsset` 和网格/PhysicsAsset；源 `.uerlpol2` 文件只用于编辑器导入和重导入，不会成为包内运行时文件。

在本仓库宿主工程按 Play，默认打开 `Stylized_Egypt_Demo`：宿主 GameMode 生成 PhantomX、自动启动并追逐玩家。把插件接到别的游戏时，用插件里的 `BP_UERLPolicyRobot`，不要依赖宿主 C++。

## 发布前准备

把本仓库接到另一份 UE 5.8 工程时，跑一条命令：

```powershell
uv run uerl deploy --project <目标工程目录> --demo phantomx
```

它会拷贝 `UERLEngine` 插件、启用 `UERLEngine` / `ProceduralMeshComponent` / `NNERuntimeORT`、在显式 `--demo phantomx` 时把演示 PhantomX 三件资产放到 `/Game/Robots/PhantomX/`，并写入部署所需的同步子步物理键。省略 `--demo` 时是通用 runtime-only 安装，不会带入 PhantomX 资产。目标工程还没有 C++ 模块时，会生成一个空的 Game Target（只含 `IMPLEMENT_PRIMARY_GAME_MODULE`，不含策略逻辑），安装版引擎打包时 UAT 才能把插件链进游戏 exe。已存在的插件、网格和已有 Modules 默认跳过；`--force` 覆盖插件与选中的 demo 资产。只补演示网格时再跑一次带 `--demo phantomx` 的同一命令。

接入机器人只使用 Blueprint 和 Content，不改插件源码、不写策略 C++。打开工程后若刚生成了空模块，先编译，再在控制台执行 `UERL.CheckProject`（见下文"编辑器项目体检"）。

编辑器启动前也可以做只读命令行预检；它不改工程、不启动 UE：

```powershell
uv run uerl deploy --project <目标工程目录> --check
uv run uerl deploy --project <目标工程目录> --check --task <TaskID> --artifact latest
```

`--artifact` 可以是 `.uerlpol2` 文件、Run 目录，或该 Task 最新一次 Run。

## 无界面导入制品

工程已编译 Editor target 且插件启用后，`uerl deploy` 会按 Task 填好制品、策略资产和 RobotMesh，并打印 Commandlet。确认后加 `--import` 才会启动 Editor：

```powershell
uv run uerl deploy --project <目标工程目录> --task UERL-PhantomX-ContinuousTerrain-v0 --artifact latest
uv run uerl deploy --project <目标工程目录> --task UERL-PhantomX-ContinuousTerrain-v0 --artifact latest --import
```

打印出的命令等价于：

```powershell
& "<UE_ROOT>\Engine\Binaries\Win64\UnrealEditor.exe" <目标工程目录>\<Project>.uproject `
  -run=UERLPolicyImport `
  -artifact="<RunDir>\exported\<task>.uerlpol2" `
  -asset=/Game/UERLEngine/Policies/<PolicyName> `
  -robotmesh=/Game/Robots/PhantomX/SK_PhantomX `
  -unattended -nop4
```

`-asset` 可以写 `/Game/Path/Asset` 或 `/Game/Path/Asset.Asset`；目标资产已存在时必须显式加 `--replace-existing`（Commandlet 开关是 `-replaceexisting`），否则命令失败且不会覆盖。Commandlet 会解析制品、绑定 `RobotMesh`、执行与 `StartPolicy` 相同的资产部署校验并保存 `.uasset`。它不负责把 Blueprint 放进地图；完成导入后仍需执行 `UERL.CheckProject`，再做 Play/cook 验收。默认资产名由 Task ID 去掉非字母数字得到；要用别的包名时传 `--asset`。

需要把**编辑器**预编译二进制拷到其它机器时：

```powershell
uv run python scripts/package_plugin.py --output <输出目录>
```

把输出目录里的 `UERLEngine` 拷进目标 `Plugins/`，或把同一路径传给 `uerl deploy --from-package`。这条命令不产生安装版 `UnrealGame.exe` 能加载的游戏 DLL；发行包仍然要求工程带 C++ Game Target（安装脚本可生成，也可在编辑器 **Tools → New C++ Class** 加任意空类）。

本仓库提供的演示资产是：

```text
/UERLEngine/Policies/PhantomXContinuousTerrain116
/UERLEngine/Blueprints/BP_UERLPolicyRobot
```

制品里的 `RobotMesh` 指向游戏工程路径 `/Game/Robots/PhantomX/SK_PhantomX`，网格、Skeleton 和 PhysicsAsset **不在插件里**。复制后的对象路径必须是 `/Game/Robots/PhantomX/SK_PhantomX`。打开制品检查 Summary 时确认 `RobotMesh` 不是空引用；保存或 cook 时资产的 IsDataValid 也会把缺网格报成错误。`robot_id` 只用于制品身份校验，不能用来推导或替换 `RobotMesh`。

制品契约为 observation `116`、action `18`、`physics_dt=0.005 s`、控制间隔 `[1,7]` 个物理步，命令通道为 `velocity` 三个值 `[forward, lateral, yaw]`。

## 配置游戏物理时钟

`uerl deploy` 会写入这些键。游戏工程默认使用同步 Chaos 子步，直接 Play 即可满足部署门禁。若要手工维护，在目标工程 `Config/DefaultEngine.ini` 写入：

```ini
[/Script/Engine.PhysicsSettings]
bTickPhysicsAsync=False
bSubstepping=True
bSubsteppingAsync=False
MaxSubstepDeltaTime=0.005
MaxSubsteps=7
MaxPhysicsDeltaTime=0.033333
```

`MaxSubstepDeltaTime` 不得大于制品的物理步，`MaxSubsteps` 不得小于制品的 decimation 上限。策略组件不改这些设置。Worker 训练进程会在启动参数里改回锁步（子步关闭），与游戏 Play 分开。不要因为引擎默认 `MaxPhysicsDeltaTime=1/30` 再去关同步子步。

策略组件以已完成的 Chaos solver clock 为控制时钟。World time dilation 会改变游戏时间和陈旧命令计时；需要稳定控制时保持默认 time dilation。

## 编辑器项目体检

编辑器完成加载后会自动做一次项目体检；也可以随时在控制台（`~`）执行：

```text
UERL.CheckProject
```

体检覆盖三类问题，全部输出到 **Message Log 的 UERL 页签**（发现问题才自动弹出，并伴有右下角提示）：

- **Packaging**：纯蓝图工程且没有任何预编译 `UnrealGame` 插件二进制时，提示打包会在运行时失败。安装版引擎上的补救是加空 C++ Game Target（安装脚本可生成，或 Tools → New C++ Class）。`package_plugin.py` 只分发编辑器二进制，不能替代 Game Target。
- **Artifact**：工程内每个 `UUERLPolicyArtifactAsset` 的制品字节可解析性、`RobotMesh` 是否可解析；缺网格会给出"资产编辑器里指定"和"迁移资产到引用路径"两个动作。保存/cook 资产时 `IsDataValid` 也会强制同一校验。
- **DeployPhysics**：用当前工程物理设置对每个制品的 timing 跑与 StartPolicy 相同的部署门禁；不通过时给出要改的 `[/Script/Engine.PhysicsSettings]` 键。这里只评估工程设置；运行时的实际 solver 仍由 StartPolicy 的实时门禁判定。cook/命令行模式下不做启动体检。

## 导入制品和放置示例蓝图

若目标工程还没有制品，在 Content Browser 中导入 `PhantomXContinuousTerrain-116.uerlpol2`，打开生成的 `UUERLPolicyArtifactAsset`，检查 Summary、observation/action 宽度、timing 和 command channels，并把 `RobotMesh` 明确选成 `/Game/Robots/PhantomX/SK_PhantomX`。PhysicsAsset 需与训练时的 19 bodies/18 joints 拓扑匹配。导入后的资产要随地图引用一起 cook；源 `.uerlpol2` 不要作为包内文件读取。

本仓库插件已带导入后的 `/UERLEngine/Policies/PhantomXContinuousTerrain116`。把 `BP_UERLPolicyRobot` 拖进有地面和 PlayerStart 的地图。示例蓝图认领自身的 `SkeletalMesh`：

- `Artifact` = `PhantomXContinuousTerrain116`
- `bClaimOwnerMesh` = true，`OwnerMesh` 指向自身骨骼网格（Owner 上有多个匹配网格时必须这样指定）。蓝图里填的是类默认组件；`StartPolicy` 会按组件名解析到当前 Owner 实例。
- `bAutoStart` = false，BeginPlay 自己完成启动顺序
- `AutoReceiveInput` = Player 0，用于键盘诊断

BeginPlay 顺序是：图上已绑定四个诊断事件 → `GetRequiredCommandChannels` → `SetCommand(velocity, [0,0,0])` → `StartPolicy`。Tick 用 `GetPlayerPawn(0)` 和 `GetRobotTransform` 把玩家平面位置写成 `velocity`；距离小于 1.25 m 时写零。插件不提供 `SetCommandVelocity`。`GetRobotTransform` 是认领或生成后的骨骼网格世界变换，不是组件 Owner。认领时网格会从 Owner 根组件拆开，Chaos 带着网格走；Owner 可以留在原地。`StartPolicy` 从网格位置向下测 WorldStatic 离地高度，并忽略 Owner，室内屋顶不会挡住启动。部署时的地形扫描、离地高度和 `ResetToReferencePose` 也从机身附近向下找地面，头顶的静态遮挡不会被当成地面。

键盘诊断（需焦点在游戏视口）：

```text
1  StopPolicy
2  SoftReset            // PrintString 返回值
3  ResetToReferencePose // PrintString 返回值
4  StartPolicy          // PrintString 返回值
```

组件默认也可以 `bAutoStart=true`：Actor BeginPlay 里先 `SetCommand`，组件在第一个 tick 自动 `StartPolicy`。示例蓝图关掉它，是为了把事件绑定和显式 Start 留在图上。本仓库 Egypt 关卡由 `AUERLEgyptChaseGameMode` 生成组件并打开 `bAutoStart`；那是宿主玩法，不是插件接口。

## 运行、停止和复位

`SetCommand` 会复制并锁存每个通道的值。调用前可用 `GetRequiredCommandChannels` 读取当前制品要求的名称和宽度；本制品只有 `velocity` 宽度 3。命令超过 `CommandStalenessSeconds` 时触发 `OnCommandStale`，不把陈旧诊断当作控制周期。将它设为 `0` 可关闭该诊断。

在 Blueprint 中把以下四个事件接到日志、HUD 或 gameplay 状态机。示例蓝图已接到 Print String：

- `OnControlStepOverrun(GameSeconds, PhysicsSeconds, ObservationSeconds)`：游戏时间、实际 solver 时间和策略观测窗口分别报告，三者含义不同。
- `OnCommandStale(Channel, StaleSeconds)`：只说明该通道未更新，不会自动改变动作。
- `OnPolicyFault(Reason)`：策略停止并保留实体，先处理错误再决定是否复位。
- `OnPhysicsBaselineMismatch(Report)`：启动时物理 gate 不满足，先修正 World Physics Settings。

停止和两种复位用组件节点完成，并把返回值接到 Branch/Print String 或 UI：

```text
StopPolicy()                 // void：停止推理，不冻结 Chaos 物理
SoftReset() -> bResetOK      // 清历史、接触、地形缓存和 solver 累计
ResetToReferencePose() -> bPoseOK
                              // 复位 live robot pose，Actor 不被销毁
StartPolicy()                // 复位后显式重新启动
```

`StopPolicy` 后不会继续旧控制步；如果组件是 floating base，pose reset 会按当前地面和 Owner 安装变换恢复；fixed base 保持安装变换。复位失败时读取 `GetLastError`，不要用重新生成 Actor 掩盖错误。

## 多实例和性能

每个组件实例独立拥有控制器、命令锁存、previous action、接触和 terrain cache；多个 PhantomX 可以在同一 World 中互相碰撞，但不会共享策略状态。所有实例共享同一同步 solver，因此 World 参数必须满足全部制品的最小子步长和最大容量。

控制触发由 `DtMin` 和实际物理推进共同决定。常用帧率下一帧通常触发一次；当物理步高于 200 Hz 时，PhantomX 会在 `[1,7]` 范围内累计短帧。部署不另加一个“降频”开关，改变控制区间应重新训练并重新导出制品。

## Cook、打包和验收

确认打包路径就绪（见"发布前准备"）：工程带 C++ Game Target。安装版 `UnrealGame.exe` 是单体链接，没有 Game Target 时打出的包会在启动时报 `UERLInterface could not be found`；编辑器的 `UERL.CheckProject` 会在打包前就把这种情况报出来。

打包前确认地图、`BP_UERLPolicyRobot`、复制进游戏的 PhantomX Skeletal Mesh、PhysicsAsset、导入后的策略资产和 ORT runtime 都被引用并进入 cook。验证包内运行时不读取源 `.uerlpol2`，并检查启动日志包含类似：

```text
[UERLPolicyComponent] StartPolicy succeeded owner=BP_UERLPolicyRobot_C_1
[UERLPolicyComponent] first control step frame=7 observation_dt=0.005000 solver_dt=0.005000
```

本仓库宿主按 Play 打开 `Stylized_Egypt_Demo`：GameMode 生成 PhantomX、自动 Start，并追逐玩家。不要把编辑器启动地图改成 Egypt，自动化会加载该大地图。

## 已知边界

连续 dt 的泛化范围由训练时 `[1,7]` 选择决定；超出范围的窗口只能触发 overrun 诊断，clamp 不会修复未训练的时序。末步接触采样不恢复窗口内已经结束的力峰值。演示地图使用静态地面和缓存的 terrain query；移动地面、动态障碍和新的 command channel 需要新的任务/制品契约。单 World 的同步 solver 支持范围仍由 UE 5.8 和目标平台决定。安装版 `UnrealGame.exe` 不加载工程插件 DLL，打包必须经过带 C++ Game Target 的工程。`package_plugin.py` 只产出编辑器预编译插件，不能替代 Game Target。
