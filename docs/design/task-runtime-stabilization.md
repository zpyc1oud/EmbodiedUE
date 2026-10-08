# Task configuration and runtime stabilization

Status: implementation and verification in progress.
Baseline: `97e56898d366ee739e88cd2908e65b906ea91e13`.
Related work: [Issue 41](https://github.com/zpyc1oud/EmbodiedUE/issues/41) and
[Issue 8](https://github.com/zpyc1oud/EmbodiedUE/issues/8).

## Scope and order

1. Review the existing Task configuration against Isaac Lab.
2. Verify input, reset and physical behavior across training and deployment.
3. Measure runtime latency and memory without policy learning.

Formal training, tuning and held-out behavior evaluation follow these checks.
A short smoke run can check a training/export path. It cannot establish policy
quality. Catch is outside this work. This change does not restore the reward or
command-range changes from the withdrawn PRs 42 and 43.

## Reference

Pin Isaac Lab at `b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8`:

- [Velocity environment configuration](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab_tasks/isaaclab_tasks/manager_based/locomotion/velocity/velocity_env_cfg.py)
- [Go1 configuration overrides](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab_tasks/isaaclab_tasks/manager_based/locomotion/velocity/config/go1/rough_env_cfg.py)
- [Joint observation functions](https://github.com/isaac-sim/IsaacLab/blob/b0542fe2d45bf91c4e1d9ef6952b9c709c80b4e8/source/isaaclab/isaaclab/envs/mdp/observations.py)

The Go1 overrides disable pushes and change action scale, resets and reward
weights. Thus, the generic velocity configuration is not one recipe for all
robots. PhantomX has six legs. Retain explicit robot-specific choices and record
them rather than copy the Go1 constants.

## Configuration comparison

The last column records this review's decisions, not an upstream requirement.

| Area | Isaac Lab reference | EmbodiedUE baseline | Decision |
|---|---|---|---|
| Composition | Typed environment, asset and term configurations | Typed Robot assets, Task factories and manager terms; YAML supplies parameters | Keep the composition. A configuration-format rewrite is unnecessary. |
| Joint reference | Relative positions use the asset's default joint positions | Actions use resolved defaults; PhantomX observations used fixed constants | Repair the mismatch by joint name. |
| Actions | Default-relative position targets; robot-specific scale | Position targets from RobotSpec and a shared action plan | Keep explicit actuator configuration. Test order and units. |
| Commands | Planar/yaw ranges, standing and timed resampling | Forward range 0.4–0.5 m/s, zero lateral command, standing mixture and turn progression | Record limited coverage. Defer new ranges until the physical baseline is established. |
| Timing | 5 ms physics and decimation 4 | 5 ms physics, decimation 1–7 and an actor dt input | Keep the accepted completed-window contract. Use fixed decimation for a matched runtime measurement. |
| Rewards | Positive exponential tracking and configured motion costs | Reference-subtracted tracking, progress terms and additional costs | This is a training-design difference, not a proven physics defect. Defer objective tuning. |
| Events | Base pushes at 10–15 s; Go1 disables them | Three-axis velocity increments at 1–1.5 s plus material events | Document operation, axes and interval. Do not infer causation from source differences. |
| Sensing | Height rays and contact history | Negotiated terrain/clearance fields and final-solver-step geometric contact | Preserve the documented semantics. These contact values are not interchangeable sensor histories. |
| Reset | Asset and environment reset terms | Resolved reference pose plus sampled offsets, with sparse Slot reset | Verify the effective references and unaffected Slots. |
| Evaluation | Play overrides remove selected training randomization | Deterministic play, shared actor plans and task-owned reports | Keep runtime validity separate from learned behavior. |
| Execution | Simulator-owned scene and GPU-capable simulation | UE Worker process, CPU Chaos and explicit request/response timing | Treat this as an engine adaptation. Profile transport and physics separately. |

Repository sources:
[Robot asset](../../src/uerl/assets/robots/phantomx.py),
[training configuration](../../src/uerl/configs/tasks/phantomx/training.yaml),
[Task composition](../../src/uerl/tasks/phantomx/composed.py),
[commands](../../src/uerl/tasks/phantomx/commands.py),
[curriculum](../../src/uerl/tasks/phantomx/curriculum.py),
[Task events](../../src/uerl/tasks/phantomx/registration.py), and
[Direct environment](../../src/uerl/core/direct/env.py).

## Confirmed repair: reference-pose inputs

Before this repair, changing a Robot asset's default joint positions changed
zero-action targets and reset references. The PhantomX observation plan still
subtracted the built-in constants. For example, a measured angle of 0.06 rad
with a configured reference of 0.02 rad must produce 0.04 rad. The old plan used
the built-in thigh reference of 0.15 rad and produced -0.09 rad.

Build the observation defaults from the resolved RobotSpec. Select them by joint
name, because actuator order need not equal observation order. Store these
values in the exported plan. The built-in reference pose remains unchanged.
The standalone historical observation-plan helper remains the fixed built-in
fixture definition; it is not the custom-asset composition path.

Tests must cover a changed RobotAssetCfg reference, reversed actuator order,
zero-action physical targets and plan serialization. A real UE case must check
reset and completed-window observations against measured joint angles minus
the declared references. Existing nonuniform `joint_pos_rel` parity fixtures
check the native operator independently.

Custom-pose policies built with the old mismatch need reevaluation and a new
export after correction. No historical policy result establishes this repair's
behavior. No training-objective marker or built-in reward is changed here.

## Input consistency boundaries

| Input | Training source | Deployment requirement |
|---|---|---|
| Body pose and velocities | Reflected Robot fields in declared frames | Use the same field units, frame convention and plan transforms. |
| Joint position | Measured angle minus the configured reference | Carry the resolved reference in the artifact plan. |
| Previous action | Task action history | Clear history at the same explicit reset/bootstrap boundary. |
| Control interval | Completed solver frames times physics dt | Report the completed game physics interval; use the documented bootstrap value. |
| Terrain and clearance | Environment-owned surface queries | Verify the target map's supported WorldStatic ground and query filtering. |
| Contact | Geometric support at the final solver step | Preserve that sampling rule; do not substitute accumulated collision events. |
| Velocity command | Task or host command source | Publish the same channel width and units before the policy decision. |
| Critic contact force | Training-only additional state | Do not add it to the deployed actor input. |

Training-only observation noise must not enter the clean export plan. A matching
field name and shape do not prove matching ground filters or sample timing.
Use independent input/action replay and real static-ground cases to check these
boundaries. The existing artifact format and closed operator set remain intact.
This review does not add a vision sensor or claim arbitrary moving-ground support.

## Runtime measurements

Use the [bounded runtime benchmark](../how-to/runtime-benchmark.md). Start with
one Slot. Compare 16 and 64 Slots only when the host has enough commit capacity.
Run one UE process at a time. Record startup and shutdown separately from the
measured steady-state interval. Record system commit and owned-process private
bytes beside the timing report.

A zero-action benchmark measures the Worker and DirectEnv path. It excludes
policy inference, PPO, curriculum adaptation and training events. It is useful
for bottleneck isolation, not an estimate of complete training throughput.
Measure the missing components separately when that phase is approved.

## Acceptance for this phase

- Independent reference-pose tests fail before repair and pass afterward.
- Built-in numerical fixtures retain their existing tolerances.
- The real UE custom-reference/reset case and affected native parity pass.
- A bounded benchmark reports measured steps, actual physical time, latencies,
  Slot throughput and clean owned-process shutdown.
- Memory evidence distinguishes system commit, process private bytes and GPU
  memory. A successful run does not imply that a larger batch is safe.
- Default Python, Ruff, Mypy, documentation and affected real runtime checks pass
  on the final candidate. Review the final diff before submission.

Learning and final package acceptance remain separate later work.
