# PhantomX 高等级地形训练与消融实验（2026-09-24）

## 结论

现在已经有完整训练支撑结果。基线从已有 `model_450.pt` 续训 550 个迭代，最终保存为总迭代 `model_999.pt`；固定地形逐级评测时，level 0–6 都保持行走，level 7 仍不能稳定前进。此前只看 `model_450.pt` 时把 level 5/6 判成失败，主要是训练未完成造成的判断偏差。

同一 `model_450.pt` 起点的完整对照训练表明：

- `action_scale ×2`（从头训练和公平续训两组）都在 level 4–7 接近原地，不能作为当前修复方案。
- `effort_limit ×2` 续训可以在 level 4/5 保持行走，但 level 6 明显变差，level 7 只得到不稳定的部分前进并出现跌倒；增加驱动力不能解决最高等级。
- 将高等级几何替换为 level 4 参数时，短时探针立即恢复前进，地形几何仍是 level 7 的首要阻塞因素。

当前可用边界是 **level 0–6**；**level 7 尚未达到稳定行走**。如果验收目标是 level 7，仍需要修改地形难度、课程或策略结构并继续训练；如果目标到 level 6，基线已达到稳定阶段，可以停止继续训练基线。

PhysicsAsset 反射结果中 18 个关节的 `lower_limit` 和 `upper_limit` 均为 `null`。因此当前失败不是 UE 关节硬限位触发；`action_scale` 实验验证的是策略目标角度范围/步幅，不是放宽物理关节限制。

## 训练与评测口径

- Task：`UERL-PhantomX-ContinuousTerrain-v0`
- 本地 UE 5.8 Chaos Worker，`physics_dt=0.005`，`decimation=[1,7]`
- 完整训练：64 个 Slot，自动 terrain curriculum，总迭代 1000（最终 checkpoint 为 `model_999.pt`）
- 最终行为评测：每个固定 terrain level 4000 control steps，3 个 timeout episode；每次 1 个评测 Slot
- `success_rate=1` 只表示该评测中没有跌倒/基础接触，不表示达到指令速度；行走判断同时看 `forward_velocity` 和 `speed_error`
- 训练命令的 `VC-007 iterations=550` 表示本次从 `model_450.pt` 续训的迭代数，最终模型的总迭代为 1000

完整基线训练产物：

- `runs/20260924-phantomx-complete/baseline/model_final.pt`
- `runs/20260924-phantomx-complete/baseline/rsl_rl/model_999.pt`

完整对照训练产物：

- `runs/20260924-phantomx-complete/action_scale_x2/model_final.pt`（从头训练 1000 迭代）
- `runs/20260924-phantomx-complete/action_scale_x2_resume/model_final.pt`（从 `model_450.pt` 续训到总迭代 1000）
- `runs/20260924-phantomx-complete/effort_x2_resume/model_final.pt`（从 `model_450.pt` 续训到总迭代 1000）

## 完整基线最终评测

以下每行均为 `episodes=3`、`success_rate=1`、`fall_rate=0`、`base_contact_rate=0`。

| level | forward velocity (m/s) | speed error | action clip | torque clip | torque over limit |
|---:|---:|---:|---:|---:|---:|
| 0 | 0.626221 | 0.210336 | 0.265486 | 0.566806 | 1.774307 |
| 1 | 0.618599 | 0.204008 | 0.261833 | 0.571056 | 1.776521 |
| 2 | 0.610565 | 0.198839 | 0.265097 | 0.575500 | 1.783345 |
| 3 | 0.589873 | 0.185874 | 0.259278 | 0.574903 | 1.753586 |
| 4 | 0.562159 | 0.169115 | 0.259181 | 0.576681 | 1.773321 |
| 5 | 0.514883 | 0.161282 | 0.247750 | 0.575833 | 1.745254 |
| 6 | 0.512534 | 0.169589 | 0.234250 | 0.578347 | 1.741115 |
| 7 | 0.056350 | 0.408346 | 0.091778 | 0.226139 | 0.810782 |

level 7 没有跌倒但几乎不前进，属于“站住/停住”而不是通过。基线 level 5/6 的可行性只有在完整训练后才得到确认。

## 完整动作幅度对照

### 从头训练：`action_scale=0.40`

| level | forward velocity (m/s) | speed error | action clip | torque clip | torque over limit |
|---:|---:|---:|---:|---:|---:|
| 4 | 0.001850 | 0.443043 | 0.000069 | 0.155611 | 0.148708 |
| 5 | 0.000187 | 0.444760 | 0.000000 | 0.007556 | 0.002088 |
| 6 | -0.000056 | 0.444923 | 0.000014 | 0.220097 | 0.131025 |
| 7 | 0.001255 | 0.443928 | 0.000097 | 0.196361 | 0.100287 |

四个等级均为 `episodes=3`、`success_rate=1`、`fall_rate=0`，但都没有形成有效步态。力矩指标变小是因为策略停止输出有效步幅，不是性能改善。

### 公平续训：从同一 `model_450.pt` 续训，`action_scale=0.40`

| level | forward velocity (m/s) | speed error | action clip | torque clip | torque over limit |
|---:|---:|---:|---:|---:|---:|
| 4 | 0.001545 | 0.443146 | 0.055611 | 0.224653 | 0.635154 |
| 5 | -0.000116 | 0.444753 | 0.000000 | 0.279750 | 0.234965 |
| 6 | -0.000153 | 0.444837 | 0.000222 | 0.163389 | 0.083534 |
| 7 | 0.000135 | 0.444564 | 0.000000 | 0.331125 | 0.407539 |

公平续训与从头训练结论一致，排除了初始化差异造成的主要误判：扩大动作目标范围没有保住基线步态，也没有把 level 7 变成可行等级。

## 完整驱动力对照

从同一 `model_450.pt` 续训，保持 `action_scale=0.20`，只把 `effort_limit` 从 `2.8` 改为 `5.6`。

| level | forward velocity (m/s) | speed error | action clip | torque clip | torque over limit | episodes / success / fall |
|---:|---:|---:|---:|---:|---:|---:|
| 4 | 0.575441 | 0.190177 | 0.248097 | 0.601111 | 1.860325 | 3 / 1.00 / 0.00 |
| 5 | 0.541994 | 0.178463 | 0.240444 | 0.596292 | 1.821454 | 3 / 1.00 / 0.00 |
| 6 | 0.335815 | 0.267961 | 0.226361 | 0.601931 | 1.820363 | 3 / 1.00 / 0.00 |
| 7 | 0.141769 | 0.359390 | 0.171347 | 0.569903 | 1.612548 | 4 / 0.75 / 0.25 |

增加 effort limit 让 level 4/5 的速度与基线同一量级，但没有改善 level 6/7；level 7 还出现了跌倒。它不是“只差力矩”的问题，继续提高 effort limit 也不是当前优先修复。

## 短时单因素探针

短时探针使用 `model_450.pt`、1 个 Slot、1200 control steps，用于定位敏感因素，不能替代完整收敛训练：

| 实验 | level | forward velocity (m/s) | speed error | action clip | torque clip | torque over limit |
|---|---:|---:|---:|---:|---:|---:|
| baseline | 4 | 0.3126 | 0.2463 | 0.1072 | 0.4545 | 1.0077 |
| terrain observation = 0 | 4 | 0.0015 | 0.4575 | 0.4461 | 0.6674 | 2.6237 |
| baseline | 5 | 0.0034 | 0.4557 | 0.0481 | 0.4424 | 1.0978 |
| level 5 geometry replaced by level 4 parameters | 5 | 0.4381 | 0.1450 | 0.1433 | 0.5494 | 1.2955 |
| terrain observation = 0 | 5 | -0.0000 | 0.4594 | 0.5009 | 0.6021 | 2.5169 |
| effort limit ×2 | 5 | 0.0034 | 0.4557 | 0.0481 | 0.2196 | 0.3062 |
| action scale ×2 | 5 | 0.6457 | 0.4129 | 0.2765 | 0.6072 | 3.9355 |
| baseline | 7 | -0.0015 | 0.4605 | 0.1126 | 0.5109 | 0.8598 |
| level 7 geometry replaced by level 4 parameters | 7 | 0.3240 | 0.2335 | 0.1100 | 0.4966 | 1.1506 |
| effort limit ×2 | 7 | -0.0015 | 0.4605 | 0.1126 | 0.0602 | 0.0790 |
| action scale ×2 | 7 | 0.3845 | 0.4283 | 0.2442 | 0.5910 | 3.5356 |

这组结果说明：

1. 中等难度下，terrain-height 观测是有效信息；level 4 清零后速度从 `0.3126` 降到 `0.0015 m/s`。
2. 把 level 5/7 几何替换为 level 4 后立即恢复前进，是高等级地形难度的直接因果证据。
3. `action_scale ×2` 的短探针曾经出现前进，但完整训练没有复现，因此不能据此修改产品动作范围。
4. effort limit 短探针只降低了超限指标；完整续训后也没有把 level 7 变成稳定行走。

## 最终判断与后续决策

- 已确认的训练状态：基线 level 0–6 稳定，level 7 不稳定/不前进。
- 首要因素：level 7 地形几何超出当前策略的可通行范围；完整基线训练已经排除了“level 5/6 只是训练不够”的误判。
- 地形感知：对 level 4 等中等难度是必要信息，但当前观测不能单独解决 level 7。
- 驱动力：加倍 effort 能保持 level 4/5，但不能越过 level 7 边界；不建议只提高 effort limit。
- 关节角度：没有 PhysicsAsset 硬限位证据；加大 `action_scale` 的完整训练反而停步，不建议直接修改产品值。

本轮实验没有修改产品配置或提交任何实验专用参数；动作幅度/effort 的改动只存在于临时训练进程，临时脚本已删除。原始训练与评测日志保存在 `runs/20260924-phantomx-complete*` 下。
