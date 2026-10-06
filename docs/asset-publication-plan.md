# Fab exclusion and publication history

The recorded exclusion removes raw Stylized Egypt content and retains the optional scene integration.
Users acquire their own copy through the [official Fab listing](https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd).
Use the [local setup guide](how-to/optional-egypt-demo.md) for installation.
No replacement scene was created.
The listing displayed Free on 2026-10-03.
Its specific license field was unresolved.
Price is not redistribution permission, and the Apache license does not cover Fab content.

## Removed and preserved scope

Exactly **304 tracked files** under `engine/Content/Stylized_Egypt/` were removed:

- 302 `.uasset` files and one `.umap`: **303 distinct LFS objects**.
  The pointer-declared sizes totaled **644,598,500 bytes**, about 615 MiB.
  The working files were pointers, not downloaded payloads.
  The operation did not download or purge LFS objects.
- `Maps/Stylized_Egypt_Demo.utxtmap`: **24,295,013 bytes**, about 23 MiB, stored in ordinary Git.
  This excluded map export also contains vendor scene data.
- Categories include meshes, materials, textures, the demo map, built lighting
  data and landscape layer assets. No source integration files were in this folder.

The operation retained these items:

- `UERLEgyptChaseGameMode.{h,cpp}` and the Pursuit registration
- The original (-9.4, 1.6)m ground origin and authored-robot contract
- Robot and policy assets outside the vendor directory
- Deployment integration

The setup guide describes the source settings.
The modified binary map is not a distributed replacement for setup.

The host defaults to the existing empty map `/Engine/Maps/Entry`.
The retained Egypt GameMode is inactive there because of its map-name condition.
An explicit Egypt launch fails before UE startup if the map is absent, empty, or an LFS pointer.
Pursuit does not select an alternative map.
CartPole and continuous/discrete terrain registrations remain unchanged.
Flat walking still selects `/Game/Maps/NewMap`.

## Recovery evidence

Before removal, the directory contained exactly 304 tracked files.
It had no untracked additions or modifications relative to `d050c2fcb6fa850a398ecde20e3bcc6bd007b9c2`.
A private recovery archive and file manifest were created outside the repository.
Every archive entry matched its working file byte-for-byte before removal.
The archive contains 303 pointer files and the text export, not the unavailable LFS payloads.
Do not publish that archive as an alternative distribution.

Git retains those files at the recorded commit.
An authorized local restoration can use `git restore --source=d050c2fcb6fa850a398ecde20e3bcc6bd007b9c2 --worktree -- engine/Content/Stylized_Egypt/`.
This command restores Git content.
It does not establish LFS payload availability or redistribution rights.
Prefer the usable package from Fab.
Git now ignores the directory to exclude local installations and map changes from future commits.

## Remaining publication decision

**Deleting current paths does not remove historical assets. Do not change repository
visibility on the assumption that this deletion cleared history.** The collection exists from `00c2e1c` (Initial import).
Historical LFS pointers remain.

A separate decision must select a rights-cleared snapshot without this history or an explicitly authorized history/LFS cleanup.
The recorded exclusion performed neither option.
It did not force-push, rewrite history, delete remote data, purge LFS objects, or change visibility.

## Verification limits

The path and pointer inspection found no retained Content LFS entry with an excluded asset's object ID.
References, modified copies, or embedded content can still exist.
Other `.umap`/`.uasset` files, 64 external actor/object paths, robot assets, and media require separate provenance and dependency review.
A binary dependency inspection has not cleared `NewMap` or `UERLPolicyDemo`.

On a licensed UE host, examine dependencies with Asset Registry or Reference Viewer.
With the Fab directory absent, validate clean-project startup, CartPole, procedural terrain, flat walking, and cooking.
Separately validate installed Egypt materials, collisions, gameplay, and Worker Pursuit setup.
Python preflight examines file presence only.
This record contains no native UE map-loading validation.

See [third-party inventory](../THIRD_PARTY_NOTICES.md) and
[release readiness](release-readiness.md).
