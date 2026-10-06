# Optional Stylized Egypt demo

The Egypt integration is optional.
The current tree excludes Fab meshes, materials, textures, map, built data, and text map export.
Original host GameMode and Pursuit Task code remain available.

The default game map is the empty engine map `/Engine/Maps/Entry`.
Open the optional Egypt map explicitly in the Editor.
The setting `GameDefaultMap` does not configure Editor startup.

## Acquire and install your copy

1. Open the official [Stylized Egypt listing by AleksandrIvanov](https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd).
   It displayed **Free** on 2026-10-03. Review and accept the applicable terms yourself;
   the displayed price is not public redistribution permission. The inspected page
   did not expose a specific license value and displayed “Allows usage with AI: No”.
   Confirm that the terms cover your intended use, including any RL experiment.
2. Acquire the content through your own Fab account.
   Add it to `engine/UERLHost.uproject` through the supported Fab/Epic procedure.
   If the package requires a supported-version staging project, migrate its assets through Unreal's asset migration procedure.
   Keep the content root name and all map dependencies.
3. Make sure that this file contains an actual map package:
   `engine/Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap`.
   Its Unreal package path must be `/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo`.
   An LFS pointer is not sufficient.
4. Examine materials, textures, and collisions in the Editor.
5. Open the map explicitly.
   Keep local vendor-map changes private.

The repository ignores `engine/Content/Stylized_Egypt/` to keep installed content private.
UE 5.8 compatibility is not established here.
This checkout does not supply a modified vendor map.
It does not guarantee that the current Fab package matches the historical revision.

## Restore the integration settings locally

The original integration remains in
[`UERLEgyptChaseGameMode.cpp`](../../engine/Source/UERLHost/UERLEgyptChaseGameMode.cpp)
and [`registration.py`](../../src/uerl/tasks/phantomx/registration.py).

### Gameplay demo

1. If the map overrides the project GameMode, select `UERLEgyptChaseGameMode` for the local map.
2. Make sure that Player 0 possesses a Pawn and has a usable PlayerStart.
3. Make sure that the separate Robot and policy assets are available.

The project keeps this global GameMode.
Its chase logic activates only on map names that contain `Stylized_Egypt`.
It loads `/Game/UERLEngine/Policies/PhantomXContinuousTerrain` and spawns the policy Robot near the player.
See [deployment](../in-game-deployment-guide.md).

### Worker Pursuit

The optional `UERL-PhantomX-Pursuit-v0` Task selects the Egypt map.
It requires one Slot, Player 0, and an authored actor with the configured PhantomX Skeletal Mesh.
It also requires WorldStatic ground below **(-9.4, 1.6) metres**.
The ground trace extends from z=10 m to z=-10 m.
The original coordinates and `robot.claim_authored_actor=1` remain unchanged.

1. Select a local non-chase GameMode with a Player 0 Pawn.
2. Add one authored actor with the configured PhantomX Skeletal Mesh.
3. Disable the chase GameMode's separate policy Robot and any other automatic policy component.
4. Launch with `--presentation gameplay` to keep the map's player setup.

A fresh Fab package does not include this project's authored Robot integration.
Keep gameplay-demo and Worker-task GameMode settings separate.

For another pack revision, examine each integration assumption.
Do not silently change ground origin, Robot topology, policy contracts, or Task semantics to obtain a successful launch.
Native map loading, gameplay, and Pursuit validation remain outstanding in this guide.

## Missing-content behavior and non-Fab workflows

Before training, playback, or export starts UE, Python examines the selected Egypt map file.
An absent, empty, or LFS-pointer `.umap` causes rejection with the Fab link and this guide.
This is a presence check only.
It does not establish asset dependencies, map customization, license rights, or package compatibility.

Attach mode uses an existing host and cannot inspect its remote map locally.
The command `uerl check task` validates declarations only.

CartPole and continuous/discrete PhantomX terrain keep `/Engine/Maps/Entry`.
They do not substitute another map for Egypt.
Flat walking keeps `/Game/Maps/NewMap`.
Its binary dependency closure still requires an Editor inspection.
Robot asset provenance and LFS availability remain separate prerequisites.

The exclusion did not purge historical assets.
Before a repository visibility change, read [publication status and recovery](../asset-publication-plan.md).
