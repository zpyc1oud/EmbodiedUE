# Troubleshooting

Start with `uv run uerl tasks`, `uv run uerl config --task <TaskID>`, and `uv run uerl check task <TaskID>`. These isolate Python configuration failures before starting UE.

| Symptom | Check and next action |
|---|---|
| Unknown Task ID or override | Copy the ID from `uerl tasks`; inspect the resolved configuration and command help. The CLI suggests nearby task names. |
| Missing or invalid UE assets | Confirm `git lfs pull` completed and the files contain actual assets rather than LFS pointers. Verify access to the repository's LFS storage. |
| UE executable not found | Pass `--ue-executable` explicitly to `train`, `play`, or `export`. Their default is the Windows Epic `UE_5.8` installation path; setting `UE_ROOT` alone does not change them. |
| Host build reports a missing plugin | Confirm the bundled `UERLEngine` source is present and your UE installation supplies `ProceduralMeshComponent` and `NNERuntimeORT`. Rebuild the host with the matching UE version. |
| CUDA unavailable | Check `nvidia-smi` and the active environment's `torch.cuda.is_available()`. A CUDA wheel does not supply an NVIDIA device or driver. Use `--device cpu` when appropriate; UE is still required. |
| Old PhantomX checkpoint refuses to resume | Read the [objective and curriculum compatibility rules](how-to/phantomx-robust-training.md#start-and-resume-a-run). Select a compatible Run or train a new one. |
| Terrain playback restores incompatible curriculum state | Playback starts with registered Task curriculum settings; pass `--terrain-level N` to select a fixed procedural tier. |
| Play/export reports missing, unsupported, or conflicting saved Run config | Compare the checkpoint's embedded YAML with `resolved_config.yaml` and `manifest.yaml`. Preview a separate recovery copy with `python -m uerl.application.run_migration --source <run> --output <recovered-run>`. Historical JSON and earlier YAML schemas are not read at runtime; migration stops on missing fields or Task/version mismatch and never uses current defaults. The default checkpoint copy depends on the Run sidecar; add `--embed-checkpoint-config` only when the dry-run confirms a weights-only-compatible checkpoint and a detached checkpoint is needed. |
| Evaluation reports success but the robot barely moves | Inspect forward velocity and speed error along with episode counts. Success is based on completed episodes without base contact. |
| Worker initialization times out | Inspect UE build/content errors and Worker logs first. Session connection/request defaults are 120 seconds. A timeout invalidates the Session; do not blindly replay a Step. |
| Deployment fails its physics gate | Run `UERL.CheckProject` in the Editor and use the [synchronous substep settings](in-game-deployment-guide.md#game-physics-settings). The runtime gate reads the actual solver. |
| Imported artifact has no RobotMesh | Assign or migrate the matching Skeletal Mesh to its referenced game path. `robot_id` cannot substitute for a mesh reference. |
| Packaged game cannot find `UERLInterface` | Build through a project with a C++ Game Target. Editor-only precompiled plugin binaries do not replace that target. |
| Recording is slow or produces no usable video | Confirm viewport rendering works and the MP4 encoder dependency is installed. Recording requires viewport presentation and a newly launched Worker; it is slower than headless training. |

When reporting a failure, include the command, task, commit, OS, Python/UE versions, relevant redacted logs, and whether the failure occurs before or after Worker initialization. Include driver/GPU information for CUDA failures. Keep credentials, private paths, and proprietary assets out of public reports.
