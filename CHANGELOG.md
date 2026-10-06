# Changelog

## Unreleased

- Make Stylized Egypt an optional Fab scene installed by the user. Exclude 304 raw asset/export files and keep host/Pursuit integration. Default the host to Engine Entry. Reject explicit Egypt launches without a usable map and give setup instructions. Historical Git/LFS content still requires a publication decision.

- Adopt Apache-2.0 for original project code, documentation and configuration. Mark the project Early-Stage. Record third-party rights separately and document the unresolved Fab asset publication scope without removing content.

- Repair the bundled legacy PhantomX converter. Include required timing metadata and keep its historical 115-input observation contract.

- Fix training resume so an explicit missing checkpoint fails before UE startup. PPO now bootstraps pure timeouts only, excluding physical failures that coincide with the time limit.
- Release Session resources after startup cancellation and transport-close errors. Reject reset masks with the wrong Slot shape before a Reset request.
- Clear previous-action history during in-game policy reset, and restore authored ground material overrides when a training Session ends. Keep claimed robot materials alive until restoration, including across garbage collection.
- Before a target change, reject overlapping plugin install paths and invalid source packages or demo inputs. Reject invalid deployment physics timesteps. Compare host timing capacity with the supplied policy artifact. Remove unused host authoring-plugin requirements and allow UE test runners to use `UE_ROOT` or `UE_58_ROOT`.
- Remove the private task-journal archive hook and its dedicated checks and tests. Public contributions no longer require a `.scratch` journal. Runtime and functional test gates remain in place.
- Rewrite public documentation in English. Add setup, configuration, troubleshooting, and contribution guides. Identify the license and asset decisions required before publication. Remove internal experiment reports and development overview material. Describe training-environment use and extensions in the public guides.
- Restore the PhantomX action-change penalty to `‖a_k−a_(k−1)‖²`, without dividing by the previous control interval squared. Its weight is `2.4e-3`, converted from the referenced joint-target scale. Exploration noise no longer dominates the reward. Randomize the first 20-second episode timeout phase for each Slot, including after resume. Terrain curriculum uses command-direction progress. This prevents incorrect demotion during turns and extra promotion credit for excessive speed. Earlier terrain checkpoints cannot restore the adaptive curriculum. The command `uerl train --terrain-level N --resume` restores only the command curriculum.
- Integrate PhantomX continuous rewards, PPO discounts/GAE, and 20-second episodes over physical time under variable decimation. Promotion beyond the highest terrain level resamples across all levels. Old-objective checkpoints are rejected for resume. Vibration-frequency evaluation uses actual sample times.
- Consolidate product commands under `uerl`. Remove `uerl-train`, `uerl-play`, `uerl-export`, `uerl-tasks`, `uerl-config`, and the mesh-only `install_demo_robot.ps1` script.
- Add `uerl runs` to list Runs and resumable checkpoints. `--run` and `--resume` accept `latest`. Completed Runs select `model_final.pt`; interrupted Runs select the highest-numbered `rsl_rl/model_<iteration>.pt`.
- Add runtime installation, deployment preflight, and task-specific `UERLPolicyImport` command generation to `uerl deploy`. Only `--import` launches the Editor.
- Add `--num-envs`, `--seed`, `--device`, and `--max-iterations` to training. Add `--seed` and `--device` to playback. Add `--device` to export.
- Report CLI input errors (Task IDs, override paths, Run directories) as a concise `[FAIL]` message and next step. Unknown Task IDs include nearby candidates.
- Set Session connection and request defaults to 120 seconds for Worker startup and Slot initialization. Remove per-training-command overrides.
- Default PhantomX policy training to `runner.device: cuda:0`. CPU execution requires an explicit override.
- Show the robot asset (for example `SK_PhantomX`) in the `uerl tasks` ROBOT column, and add `robot_asset_path` to JSON output.
- Align documentation timing and parameter tables with task YAML defaults.
- Remove single-use `tools/` scripts and their tests. Product entry points remain `uerl.cli` and `scripts/`.
- Remove the RobotArm training task, assets, and integration guide. Current product robots are CartPole and PhantomX.
- Reorganize README and how-to documentation, add PhantomX inference videos, and document commands corresponding to the earlier CartPole configuration and PhantomX Runs.
- Repair pre-push E2E. Include `step_decimation` in Step requests and supply lockstep physics parameters for PIE attachment. Allow 120 seconds for Worker startup requests.

## 1.0.0 — 2026-09-17

Train robot policies in UE 5.8 Chaos.
Use the same deployment artifact to control a Skeletal Mesh in a game.

- Introduce a generic robot runtime. CartPole, PhantomX, and the then-included RobotArm use assets and Python declarations instead of robot-specific runtimes.
- Add in-game deployment through `UUERLPolicyComponent` and `BP_UERLPolicyRobot`, Editor project checks, and target-project integration via `scripts/install_into_project.py`.
- Require a C++ Game Target for packaging with an installed engine. The installer can generate an empty module. `package_plugin.py` distributes Editor precompiled plugin binaries only.
