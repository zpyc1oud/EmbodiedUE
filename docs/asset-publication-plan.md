# Fab asset publication plan

This is a read-only inventory and proposed follow-up, not an asset-removal change.
The owner confirmed on 2026-10-03 that `Stylized_Egypt` was acquired from Fab. The
exact listing, publisher, acquired terms and permitted public redistribution scope
remain unverified. The original-code Apache license does not cover these assets.

## Tracked footprint

Inspected against main `451b3bc0eeed2a4de21bdee2183a49a740ccde1e`:

- `engine/Content/Stylized_Egypt/`: **304 tracked files**.
- **303 distinct LFS objects**: 302 `.uasset` and one `.umap`, with pointer-declared
  sizes totaling **644,598,500 bytes** (about 615 MiB). These payloads are not locally
  downloaded in the inspected worktree; no LFS downloads were performed.
- The additional tracked file is
  `Maps/Stylized_Egypt_Demo.utxtmap`, **24,295,013 bytes** (about 23 MiB), stored in
  ordinary Git. It must be included in any exclusion plan, not just `.uasset` files.
- The folder includes the demo map, built lighting data, landscape layer assets,
  materials, meshes and textures. History includes it from `00c2e1c` (Initial import).
- No LFS paths outside that directory contain `Stylized_Egypt` by name. This does
  **not** establish absence of references or copied content. There are 64 tracked
  external actor/object paths elsewhere that need Editor dependency inspection.

Metadata was read with `git ls-files`, `git lfs ls-files --json`, and path-scoped
history inspection. No payloads were purged and no history was rewritten.

## Known references affected by removal

| Location | Effect / required coordinated change |
|---|---|
| `engine/Config/DefaultEngine.ini` | `GameDefaultMap` selects the Egypt demo; global GameMode selects `UERLEgyptChaseGameMode` |
| `engine/Source/UERLHost/UERLEgyptChaseGameMode.{h,cpp}` | Host chase logic activates only on map names containing `Stylized_Egypt`; renaming only the map would disable it |
| `src/uerl/tasks/phantomx/registration.py` | Pursuit selects the Egypt map, uses Egypt-specific origin (-9.4, 1.6) metres and claims an authored robot actor |
| `tests/python/unit/test_phantomx_training_config.py`, `test_training_runner.py` | Contain explicit Egypt map paths |
| `engine/Plugins/UERLEngine/Source/UERLPolicy/Private/Tests/Unit/UERLPolicyControllerTests.cpp` | Contains a test limitation message referencing the demo |
| `docs/in-game-deployment-guide.md` | Describes Egypt gameplay and its GameMode; update when replacing the example |
| Other `.umap`, `.uasset`, external actors/objects, media | Dependency/content inspection is outstanding; textual search cannot clear binary references |

CartPole defaults to `/Engine/Maps/Entry`. PhantomX continuous/discrete terrain also
explicitly use that map; their map selection does not directly require Egypt.
Flat walking uses `/Game/Maps/NewMap`, whose binary dependencies are not cleared by
this audit. Their robot assets still need their own provenance review.

## Publication options

1. **Recommended: exclude the Fab collection from the public distribution and use
   an original minimal demo.** Coordinate the references above before removal.
   Keep any privately licensed copy outside the public distribution. Publish a
   rights-cleared snapshot, or separately authorize an appropriate history/LFS
   cleanup if retaining the current repository history. A normal deletion commit
   alone would leave older assets accessible in history.
2. **Optional external demo:** require users who have the appropriate rights to
   obtain the exact Fab asset themselves, and keep Egypt-specific integration
   instructions optional. Default maps/tasks must still work without that pack.
3. **Retain assets only with evidence:** establish actual permission for the
   intended public editable-asset distribution and document applicable terms.
   Fab purchase/free acquisition or a README exception alone is not that evidence.

No option has been executed. Asset removal, replacement maps, and history/LFS
changes require separate authorization.

## Simplest reusable replacement

Author a small neutral map with an original procedural plane/box floor, simple
obstacles and plain materials; avoid another marketplace pack. Include Player 0
and a rights-cleared authored robot for the existing Pursuit contract. Generalize
the host demo's map-name gate, point the default gameplay/Pursuit map at the new
asset, and set its origin/ground-trace values deliberately. Keep the policy's
observation, action and timing contracts unchanged.

Update the two Python map expectations and deployment guide. On a licensed UE host,
use Asset Registry/Reference Viewer to inspect dependency closure for the new map,
`NewMap`, `UERLPolicyDemo`, external actors/objects and robot assets. Then verify
clean-project load, CartPole smoke, continuous/discrete terrain startup, flat walk,
Pursuit gameplay and cooking with the Fab folder absent. This plan does not claim
that merely deleting the folder preserves those workflows.

See [third-party inventory](../THIRD_PARTY_NOTICES.md) and
[release readiness](release-readiness.md).
