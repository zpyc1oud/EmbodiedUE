# Third-party notices and asset inventory

Original EmbodiedUE project code, documentation, and configuration use [Apache-2.0](LICENSE).
That grant does not relicense third-party software or content. Unreal Engine,
Fab/Epic assets, robot assets, policy artifacts, and media are outside that grant
unless an explicit project licensing statement covers the specific item.

This is a preliminary inventory.
It is not complete license clearance or a replacement for required upstream license texts.
The entries separate verified observations from permissions that still require evidence.

## Python dependencies

The project declares dependencies in [pyproject.toml](pyproject.toml) and resolves versions in [uv.lock](uv.lock).
The review on 2026-10-03 compared these license identifiers with installed distribution metadata and license files.
It did not establish every license in each platform wheel or transitive dependency.

| Dependency / inspected version | Verified license information |
|---|---|
| rsl-rl-lib 5.4.2 | BSD-3-Clause; upstream notice names ETH Zurich and NVIDIA CORPORATION & AFFILIATES |
| PyTorch 2.11.0+cu128 | BSD-3-Clause metadata; distribution also includes a NOTICE file |
| torchvision 0.26.0+cu128 | BSD-3-Clause license file |
| NumPy 2.4.6 | Metadata: BSD-3-Clause AND 0BSD AND MIT AND Zlib AND CC0-1.0; bundled component licenses apply |
| PyYAML 6.0.3 | MIT |
| imageio-ffmpeg 0.6.0 | BSD-2-Clause for the Python wrapper; not the license of the FFmpeg executable |
| Pillow 12.3.0 | MIT-CMU |
| ONNX Runtime 1.29.0 (development dependency) | MIT metadata |

The inspected main tree had no conventional vendored library tree or native dependency binaries.
Development environments and ignored `references/` were not part of that tree.
Dependencies retain their separate licenses. For any binary,
wheel, container, or installer release, include the upstream license/notice files
required by the actual components distributed, including CUDA and other bundled
libraries where applicable. FFmpeg licensing depends on its build configuration;
see [FFmpeg's official guidance](https://ffmpeg.org/legal.html).

## Engine and content

| Item | Verified provenance/status | Publication action still needed |
|---|---|---|
| Unreal Engine 5.8 | Separately acquired prerequisite; `engine/` contains the project, not a bundled engine distribution | Follow the applicable [UE agreement](https://www.unrealengine.com/eula/unreal); project Apache licensing does not grant engine rights |
| `engine/Content/Stylized_Egypt/` | Owner confirmed Fab origin; official [Stylized Egypt by AleksandrIvanov](https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd) displayed Free on 2026-10-03; 304 raw files excluded from current tree | Local acquisition required; specific license and intended-use permissions unresolved; historical Git/LFS copies still block visibility change |
| `engine/Content/Characters/Mannequins/`, `FirstPerson/`, `LevelPrototyping/`, `Weapons/` and related maps/external objects | Present; exact source classification and rights not verified | Distinguish Epic Examples/Templates, other Epic content, and third-party additions; retain applicable notices |
| `engine/Content/Robots/CartPole/`, `PhantomX/` | Robot meshes, skeletons and PhysicsAssets present | Establish authorship/source and permission for editable-asset redistribution |
| Policy `.uasset` / `.uerlpol2` files, parity fixtures and `docs/media/` | Present; no complete artifact/media rights inventory | Confirm origin and intended licensing of each distributed artifact or recording |

Fab origin alone is not public source-asset redistribution permission. The
[Fab license summary](https://www.fab.com/eula) distinguishes use in projects from
standalone redistribution; the exact acquired terms and asset classification must
be established. Some content may instead carry another license. Likewise, Epic's
[content agreement](https://www.unrealengine.com/eula/content) and the UE agreement's
Examples provisions are distinct; directory names alone do not determine rights.

The current tree excludes the Fab collection and its text map export.
The removal did not change third-party rights.
Original integration code remains. See the
[asset exclusion and history plan](docs/release-readiness.md#remaining-publication-decisions) and
[remaining release decisions](docs/release-readiness.md).
