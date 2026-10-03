# Optional Stylized Egypt demo

The Egypt integration is optional. Fab meshes, materials, textures, map, built data,
and text map export are not included in the current tree. Original host GameMode and
Pursuit task code remain available. The default game map is `/Engine/Maps/Entry`, an empty engine map. Open the
optional Egypt map explicitly in the Editor; `GameDefaultMap` does not configure
Editor startup.

## Acquire and install your copy

1. Open the official [Stylized Egypt listing by AleksandrIvanov](https://www.fab.com/listings/c935ca3e-dbb1-4b7d-a080-65de129c60bd).
   It displayed **Free** on 2026-10-03. Review and accept the applicable terms yourself;
   the displayed price is not public redistribution permission. The inspected page
   did not expose a specific license value and displayed “Allows usage with AI: No”.
   Confirm that the terms cover your intended use, including any RL experiment.
2. Acquire it through your own Fab account and add its Unreal Engine content to the
   local `engine/UERLHost.uproject` through the supported Fab/Epic workflow. If the
   current package requires a supported-version staging project, use Unreal's asset
   migration workflow into this project. Do not rename the content root or copy only
   the map without its dependencies. UE5.8 compatibility is not established here.
3. Confirm this physical path exists with an actual map package, not an LFS pointer:
   `engine/Content/Stylized_Egypt/Maps/Stylized_Egypt_Demo.umap`.
   Its Unreal package path must be `/Game/Stylized_Egypt/Maps/Stylized_Egypt_Demo`.
   Check missing materials, textures and collisions in the Editor. The repository
   ignores `engine/Content/Stylized_Egypt/` so locally installed content stays private.
4. Open that map explicitly in the Editor. Keep local edits to the vendor map private.
   This checkout does not include a modified vendor map or guarantee that the current
   Fab download matches the previously used revision.

## Restore the integration settings locally

The original source integration is preserved in
[`UERLEgyptChaseGameMode.cpp`](../../engine/Source/UERLHost/UERLEgyptChaseGameMode.cpp)
and [`registration.py`](../../src/uerl/tasks/phantomx/registration.py):

- Gameplay: set the local map's GameMode Override to `UERLEgyptChaseGameMode` if an
  asset-specific override selects another mode. The project retains this global
  GameMode, which only activates its chase logic on map names containing
  `Stylized_Egypt`. Ensure Player 0 possesses a Pawn and has a usable PlayerStart.
- The GameMode loads `/Game/UERLEngine/Policies/PhantomXContinuousTerrain` and spawns
  the policy robot near the player. Verify that the separately tracked robot and
  policy assets are available. See [deployment](../in-game-deployment-guide.md).
- The optional `UERL-PhantomX-Pursuit-v0` task still selects the Egypt map. It requires
  one Slot, Player 0, an actor with the configured PhantomX Skeletal Mesh, and
  WorldStatic ground below **(-9.4, 1.6) metres**, traced from z=10 m to z=-10 m.
  These original coordinates and `robot.claim_authored_actor=1` were not changed.
  For Worker Pursuit, use a local non-chase GameMode with a Player 0 Pawn and one
  authored actor using the configured PhantomX Skeletal Mesh. Do not simultaneously
  enable the chase GameMode's separately controlled policy robot or another automatic
  policy component. Launch with `--presentation gameplay` to preserve the map's
  player setup. A fresh Fab environment does not itself include this project's
  authored robot integration. Keep gameplay-demo and Worker-task GameMode settings
  separate in your local setup.

For a different pack revision, inspect whether these assumptions still hold. Do not
silently change ground origin, robot topology, policy contracts or task semantics to
make it run. Native map load, gameplay and Pursuit validation remain outstanding.

## Missing-content behavior and non-Fab workflows

Python launch argument preparation for training, playback and export rejects the
Egypt map when its `.umap` is missing, empty or an LFS pointer. The error supplies the
Fab link and this guide before starting UE. This is a presence check, not validation
of asset dependencies, map customization, license rights or package compatibility.
Attach mode uses an already running host and cannot inspect that remote map locally.
`uerl check task` continues to validate declarations only.

CartPole and continuous/discrete PhantomX terrain retain `/Engine/Maps/Entry`; no
Egypt map substitution occurs. Flat walking still uses `/Game/Maps/NewMap`, whose
binary dependency closure needs an Editor check. Robot asset provenance and LFS
availability remain separate prerequisites.

No historical assets were purged. See [publication status and recovery](../asset-publication-plan.md)
before changing repository visibility.
