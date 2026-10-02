# Release-readiness inventory

This inventory separates documentation work from publication decisions. The documentation is prepared for external readers; the repository is **not yet represented as licensed open source**. No license has been selected, no functional assets removed, and no publication action performed by this update.

## Owner decisions before publication

| Item | Checkout evidence | Required decision or verification |
|---|---|---|
| Project license | No root `LICENSE` or `COPYING` file found; Python metadata does not declare a license | Select the license and copyright holders, then add the approved text and metadata |
| Third-party notices | No project-level third-party notice inventory found | Inventory dependencies, incorporated code, binary runtime components, and required notices for the intended distribution |
| UE content | `engine/Content/Stylized_Egypt/`, `Characters/Mannequins/`, `FirstPerson/`, `LevelPrototyping/`, robot assets, and plugin demo policies are present | Establish origin, author, source/license, permitted redistribution, and attribution for each collection; determine what can be shipped in source versus cooked form |
| Training artifacts and media | `.uerlpol2` examples, policy `.uasset` files, parity fixtures, and `docs/media/` videos are present | Confirm provenance and distribution permission; identify the task/configuration used by each demonstration artifact |
| Unreal Engine | UE 5.8 is a separately acquired prerequisite | Confirm the planned source/binary distribution respects the applicable engine and content terms; do not imply the project license covers UE |
| Clean-host build | Host enables the bundled runtime and core engine dependencies only; live UE validation is unavailable here | Run build, training smoke, import, and packaging on a clean Windows machine |
| Git LFS | `.gitattributes` tracks `.uasset` and `.umap` | Verify external users can retrieve the required objects and that storage/access policy supports the intended release |
| Security reporting | No designated private contact or support policy supplied | Configure a private reporting channel and update `SECURITY.md` |

Dependency names such as rsl-rl, PyTorch, torchvision, NumPy, PyYAML, imageio-ffmpeg, Pillow, and ONNX Runtime identify review targets, not completed license clearance. Local `references/` is excluded by `.gitignore` and is not required by the public guides.

## Inspection scope

The documentation audit checks the current checkout for licensing files, asset groups, external prerequisites, machine-specific examples, and common credential patterns. It is not an exhaustive legal review, a scan of binary asset internals, or a full-history secret audit. Credential findings should be reported by location and type only. Before release, the owner must review history and binary/content provenance as appropriate for the distribution.

## Validation boundaries

Python CLI help, task enumeration, resolved configurations, and selected Python checks can be verified in the current Linux environment. It has no visible NVIDIA GPU/driver or Unreal Editor. Windows build, live Chaos training, recording, deployment import, and packaged-game validation remain outstanding; no such workflow was run for this documentation update.
