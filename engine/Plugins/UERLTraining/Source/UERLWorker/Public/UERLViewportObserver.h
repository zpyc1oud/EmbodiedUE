#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class UWorld;
class AActor;
class APlayerController;
class APawn;
class ASpectatorPawn;
class ADirectionalLight;
class ASkyLight;
class ASkyAtmosphere;
class AExponentialHeightFog;
class USkeletalMeshComponent;
class FUERLViewportRecorder;

/**
 * Viewport-only scene presentation layer.
 *
 * The Worker plugin is a headless training backend: it spawns simulation bodies
 * but never a camera or lighting. Under PresentationMode.VIEWPORT the RHI and a
 * window exist, yet nothing is lit or viewed, so the window is black.
 *
 * Install() runs a fixed two-step scene load - lighting then camera - and is
 * invoked automatically for every training World in viewport mode, independent
 * of which Environment or Robot is being trained. It is created only in viewport
 * mode and never touches Worker task semantics; headless sessions never
 * construct it.
 */
class FUERLViewportObserver
{
public:
	FUERLViewportObserver();
	~FUERLViewportObserver();
	/** Run the fixed lighting-then-camera scene load into the training World. */
	void Install(UWorld& World);
	/** Destroy every actor this observer spawned. */
	void Uninstall();
	/** Keep the viewport camera moving with the first generic robot. */
	void UpdateCamera(bool bAdvanceSimulationTime, double PhysicsDt);

private:
	/** Spawn the native UE5 level-lighting actor set so the scene is lit. */
	bool InstallLighting(UWorld& World);
	/** Take over the existing game PlayerController with a free-flying spectator camera and restore it on uninstall. */
	bool InstallCamera(UWorld& World);

	TWeakObjectPtr<APlayerController> Controller;
	TWeakObjectPtr<ASpectatorPawn> Camera;
	TWeakObjectPtr<USkeletalMeshComponent> FollowTarget;
	TWeakObjectPtr<APawn> PreviousPawn;
	TWeakObjectPtr<AActor> PreviousViewTarget;
	FRotator PreviousControlRotation = FRotator::ZeroRotator;
	bool bPreviousPawnHidden = false;
	bool bPreviousShowMouseCursor = false;
	bool bSavedControllerState = false;
	bool bFollowRobot = false;
	bool bFollowTargetLogged = false;
	TWeakObjectPtr<ADirectionalLight> SunLight;
	TWeakObjectPtr<ASkyLight> Sky;
	TWeakObjectPtr<ASkyAtmosphere> Atmosphere;
	TWeakObjectPtr<AExponentialHeightFog> Fog;
	bool bOwnSunLight = false;
	bool bOwnSky = false;
	bool bOwnAtmosphere = false;
	bool bOwnFog = false;
	TUniquePtr<FUERLViewportRecorder> ViewportRecorder;
};
