# Fab exclusion and publication history

The current release branch excludes the raw Stylized Egypt content and retains
its optional scene integration. Users acquire the asset themselves through the
[official Fab listing](https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd)
and follow [local setup](how-to/optional-egypt-demo.md). No replacement scene was authored.
The listing displayed Free on 2026-10-03; its specific license field was unresolved.
Price is not redistribution permission, and the Apache license does not cover Fab content.

## Removed and preserved scope

Exactly **304 tracked files** under `engine/Content/Stylized_Egypt/` were removed:

- 302 `.uasset` and one `.umap`: **303 distinct LFS objects**, pointer-declared sizes
  totaling **644,598,500 bytes** (about 615 MiB). The local working files were pointers,
  not downloaded payloads. No LFS download or purge was performed.
- `Maps/Stylized_Egypt_Demo.utxtmap`: **24,295,013 bytes** (about 23 MiB), stored in
  ordinary Git. This map export also contains vendor scene data and is excluded.
- Categories include meshes, materials, textures, the demo map, built lighting
  data and landscape layer assets. No source integration files were in this folder.

Preserved: `UERLEgyptChaseGameMode.{h,cpp}`, the Pursuit registration, original
(-9.4, 1.6)m ground origin and authored-robot contract, robot/policy assets outside
the vendor folder, and deployment integration. Source settings are described in the
setup guide; the modified binary map is not redistributed as a substitute for setup.

The host now defaults to the existing `/Engine/Maps/Entry` empty map. The retained
Egypt GameMode is inactive there through its existing map-name gate. Explicit Egypt
launches fail before UE starts when their map is absent, empty or an LFS pointer.
Pursuit does not fall back to a different map. CartPole and continuous/discrete terrain
registrations remain unchanged. Flat walking still selects `/Game/Maps/NewMap`.

## Recovery evidence

Before removal, the directory contained exactly the 304 tracked files, with no
untracked additions or modifications against commit
`d050c2fcb6fa850a398ecde20e3bcc6bd007b9c2`. A private recovery archive and file manifest
were created outside this repository. Every archive entry was compared byte-for-byte
with its working file before removal. The archive contains the 303 pointer files and
text export, not unavailable LFS payloads; it must not be published as a workaround.

Git retains all those files at the above commit. An authorized local restoration can
use `git restore --source=d050c2fcb6fa850a398ecde20e3bcc6bd007b9c2 --worktree -- engine/Content/Stylized_Egypt/`.
This restores Git content, not proof of LFS payload availability or redistribution
rights. Prefer obtaining the usable package from Fab. The directory is now ignored
to keep local installations and map customization out of future commits.

## Remaining publication decision

**Deleting current paths does not remove historical assets. Do not change repository
visibility on the assumption that this deletion cleared history.** The collection
exists from `00c2e1c` (Initial import), and historical LFS pointers remain.

Choose separately between publishing a new rights-cleared snapshot without this
history, or explicitly authorizing history/LFS cleanup for the existing repository.
Neither option has been executed. No force-push, history rewrite, remote deletion,
LFS purge, or visibility change is part of this commit.

## Verification limits

Path and pointer inspection found no retained Content LFS entry with an excluded
asset's object ID. This does not rule out references, modified copies, or embedded
content. Other `.umap`/`.uasset` files, 64 external actor/object paths, robot assets
and media need their own provenance/dependency review. In particular, `NewMap` and
`UERLPolicyDemo` have not been cleared by a binary dependency inspection.

On a licensed UE host, check Asset Registry/Reference Viewer dependencies, then
validate clean-project startup, CartPole and procedural-terrain startup, flat walk,
and cooking with the Fab folder absent. Separately validate installed Egypt map
materials/collisions, gameplay and Worker Pursuit setup. The Python preflight checks
file presence only; native UE map loading has not been validated here.

See [third-party inventory](../THIRD_PARTY_NOTICES.md) and
[release readiness](release-readiness.md).
