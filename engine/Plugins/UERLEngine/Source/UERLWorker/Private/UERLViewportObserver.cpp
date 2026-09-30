#include "UERLViewportObserver.h"

#include "UERLWorkerLog.h"
#include "UERLViewportRecorder.h"

#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawn.h"
#include "EngineUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	const FName NativeSunLabel(TEXT("UERL_NativeSun"));
	const FName NativeAtmosphereLabel(TEXT("UERL_NativeSkyAtmosphere"));
	const FName NativeFogLabel(TEXT("UERL_NativeHeightFog"));
	const FName NativeSkyLabel(TEXT("UERL_NativeSkyLight"));
	const FName GenericRobotComponentName(TEXT("RobotSkeletalMesh"));
	const FVector FollowCameraOffset(-80.0, -80.0, 120.0);

	template <typename ActorType>
	ActorType* FindNativeActor(UWorld& World, FName Label)
	{
		ActorType* ExistingActor = nullptr;
		for (TActorIterator<ActorType> Iterator(&World); Iterator; ++Iterator)
		{
			if (Iterator->GetFName() == Label)
			{
				return *Iterator;
			}
			if (!ExistingActor)
			{
				ExistingActor = *Iterator;
			}
		}
		return ExistingActor;
	}

	template <typename ActorType>
	void ReleaseNativeActor(TWeakObjectPtr<ActorType>& Actor, bool& bOwned)
	{
		if (bOwned && Actor.IsValid())
		{
			Actor->Destroy();
		}
		Actor.Reset();
		bOwned = false;
	}

	template <typename ActorType>
	ActorType* SpawnNativeActor(
		UWorld& World,
		FName Name,
		const FTransform& Transform,
		const FActorSpawnParameters& BaseParameters)
	{
		FActorSpawnParameters Parameters = BaseParameters;
		Parameters.Name = Name;
		return World.SpawnActor<ActorType>(ActorType::StaticClass(), Transform, Parameters);
	}
}

FUERLViewportObserver::FUERLViewportObserver() = default;
FUERLViewportObserver::~FUERLViewportObserver() = default;

void FUERLViewportObserver::Install(UWorld& World)
{
	if (SunLight.IsValid() && Sky.IsValid() && Atmosphere.IsValid() && Fog.IsValid() && Camera.IsValid())
	{
		return;
	}
	if (SunLight.IsValid() || Sky.IsValid() || Atmosphere.IsValid() || Fog.IsValid() || Camera.IsValid())
	{
		Uninstall();
	}
	if (!InstallLighting(World))
	{
		Uninstall();
		return;
	}
	if (!InstallCamera(World))
	{
		Uninstall();
	}
	int32 FollowRobot = 0;
	FParse::Value(FCommandLine::Get(), TEXT("uerlfollowrobot="), FollowRobot);
	bFollowRobot = FollowRobot != 0;

	FString RecordDirectory;
	if (FParse::Value(FCommandLine::Get(), TEXT("uerlrecorddir="), RecordDirectory))
	{
		int32 FramesPerSecond = 30;
		int32 Width = 1280;
		int32 Height = 720;
		int32 MaxFrames = 750;
		FParse::Value(FCommandLine::Get(), TEXT("uerlrecordfps="), FramesPerSecond);
		FParse::Value(FCommandLine::Get(), TEXT("uerlrecordwidth="), Width);
		FParse::Value(FCommandLine::Get(), TEXT("uerlrecordheight="), Height);
		FParse::Value(FCommandLine::Get(), TEXT("uerlrecordmaxframes="), MaxFrames);
		ViewportRecorder = MakeUnique<FUERLViewportRecorder>();
		if (!ViewportRecorder->Start(World, RecordDirectory, FramesPerSecond, Width, Height, MaxFrames))
		{
			ViewportRecorder.Reset();
		}
	}
}

void FUERLViewportObserver::UpdateCamera(bool bAdvanceSimulationTime, double PhysicsDt)
{
	if (ViewportRecorder)
	{
		ViewportRecorder->Tick(bAdvanceSimulationTime, PhysicsDt);
	}
	ASpectatorPawn* Pawn = Camera.Get();
	if (!bFollowRobot || !Pawn)
	{
		return;
	}
	if (!FollowTarget.IsValid())
	{
		UWorld* World = Pawn->GetWorld();
		for (TActorIterator<AActor> Iterator(World); Iterator && !FollowTarget.IsValid(); ++Iterator)
		{
			TInlineComponentArray<USkeletalMeshComponent*> Components(*Iterator);
			for (USkeletalMeshComponent* Component : Components)
			{
				if (Component && Component->GetFName() == GenericRobotComponentName)
				{
					FollowTarget = Component;
					break;
				}
			}
		}
	}
	USkeletalMeshComponent* Target = FollowTarget.Get();
	if (!Target)
	{
		return;
	}
	const FVector TargetLocation = Target->GetComponentLocation();
	const FVector CameraLocation = TargetLocation + FollowCameraOffset;
	Pawn->SetActorLocationAndRotation(
		CameraLocation, (TargetLocation - CameraLocation).Rotation(), false, nullptr, ETeleportType::TeleportPhysics);
	if (!bFollowTargetLogged)
	{
		UE_LOG(LogUERLWorker, Display, TEXT("[FLOW] viewport camera following generic robot"));
		bFollowTargetLogged = true;
	}
}

bool FUERLViewportObserver::InstallLighting(UWorld& World)
{
	// Use the same native actor set as a UE5 level instead of inventing a
	// skybox/material or overriding the engine's lighting defaults.
	FActorSpawnParameters LightParams;
	LightParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	SunLight = FindNativeActor<ADirectionalLight>(World, NativeSunLabel);
	if (!SunLight.IsValid())
	{
		SunLight = SpawnNativeActor<ADirectionalLight>(World, NativeSunLabel, FTransform::Identity, LightParams);
		bOwnSunLight = SunLight.IsValid();
	}
	Atmosphere = FindNativeActor<ASkyAtmosphere>(World, NativeAtmosphereLabel);
	if (!Atmosphere.IsValid())
	{
		Atmosphere = SpawnNativeActor<ASkyAtmosphere>(
			World, NativeAtmosphereLabel, FTransform::Identity, LightParams);
		bOwnAtmosphere = Atmosphere.IsValid();
	}
	Fog = FindNativeActor<AExponentialHeightFog>(World, NativeFogLabel);
	if (!Fog.IsValid())
	{
		Fog = SpawnNativeActor<AExponentialHeightFog>(
			World, NativeFogLabel, FTransform::Identity, LightParams);
		bOwnFog = Fog.IsValid();
	}
	Sky = FindNativeActor<ASkyLight>(World, NativeSkyLabel);
	if (!Sky.IsValid())
	{
		Sky = SpawnNativeActor<ASkyLight>(World, NativeSkyLabel, FTransform::Identity, LightParams);
		bOwnSky = Sky.IsValid();
	}
	if (!SunLight.IsValid() || !Atmosphere.IsValid() || !Fog.IsValid() || !Sky.IsValid())
	{
		UE_LOG(LogUERLWorker, Error, TEXT("[FLOW] native UE5 lighting actor set could not be installed"));
		return false;
	}

	// A transient level has no baked capture. Capture only a SkyLight owned by
	// this observer; an existing level SkyLight keeps its authored capture.
	if (bOwnSky)
	{
		Sky->GetLightComponent()->RecaptureSky();
	}
	return true;
}

bool FUERLViewportObserver::InstallCamera(UWorld& World)
{
	// Reuse the auto-created game PlayerController when present; -game maps
	// without an explicit GameMode still provide the default controller.
	APlayerController* PC = UGameplayStatics::GetPlayerController(&World, 0);
	if (!PC)
	{
		UE_LOG(LogUERLWorker, Warning,
			TEXT("[FLOW] viewport observer requires the existing game PlayerController; camera not installed"));
		return false;
	}
	Controller = PC;

	PreviousViewTarget = PC->GetViewTarget();
	PreviousControlRotation = PC->GetControlRotation();
	bPreviousShowMouseCursor = PC->bShowMouseCursor;
	bSavedControllerState = true;
	if (APawn* ExistingPawn = PC->GetPawn())
	{
		PreviousPawn = ExistingPawn;
		bPreviousPawnHidden = ExistingPawn->IsHidden();
		PC->UnPossess();
		ExistingPawn->SetActorHiddenInGame(true);
	}

	// ASpectatorPawn ships with USpectatorPawnMovement, giving free-flight WASD
	// translation, mouse look, and scroll-wheel speed control out of the box.
	const FVector CameraLocation(-900.0, -900.0, 500.0);
	const FRotator CameraRotation = (FVector::ZeroVector - CameraLocation).Rotation();
	FActorSpawnParameters PawnParams;
	PawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASpectatorPawn* Pawn = World.SpawnActor<ASpectatorPawn>(
		ASpectatorPawn::StaticClass(), CameraLocation, CameraRotation, PawnParams);
	if (!Pawn)
	{
		UE_LOG(LogUERLWorker, Warning,
			TEXT("[FLOW] viewport observer could not spawn a spectator camera"));
		return false;
	}
	// The engine's Spectator profile is QueryOnly but still blocks WorldStatic.
	// Follow mode teleports this presentation-only pawn through generated
	// terrain every physics frame; it must not participate in scene collision.
	Pawn->SetActorEnableCollision(false);
	Camera = Pawn;

	PC->Possess(Pawn);
	PC->SetViewTarget(Pawn);
	PC->bShowMouseCursor = false;

	UE_LOG(LogUERLWorker, Display,
		TEXT("[FLOW] viewport observer installed: spectator camera + native UE5 level lighting"));
	return true;
}

void FUERLViewportObserver::Uninstall()
{
	if (ViewportRecorder)
	{
		ViewportRecorder->Stop();
		ViewportRecorder.Reset();
	}
	APlayerController* PC = Controller.Get();
	if (PC && Camera.IsValid())
	{
		PC->UnPossess();
	}
	if (ASpectatorPawn* Pawn = Camera.Get())
	{
		Pawn->Destroy();
	}
	if (PC && PreviousPawn.IsValid())
	{
		PreviousPawn->SetActorHiddenInGame(bPreviousPawnHidden);
		PC->Possess(PreviousPawn.Get());
	}
	if (PC && PreviousViewTarget.IsValid())
	{
		PC->SetViewTarget(PreviousViewTarget.Get());
	}
	else if (PC && PreviousPawn.IsValid())
	{
		PC->SetViewTarget(PreviousPawn.Get());
	}
	if (PC && bSavedControllerState)
	{
		PC->bShowMouseCursor = bPreviousShowMouseCursor;
		PC->SetControlRotation(PreviousControlRotation);
	}
	ReleaseNativeActor(SunLight, bOwnSunLight);
	ReleaseNativeActor(Sky, bOwnSky);
	ReleaseNativeActor(Atmosphere, bOwnAtmosphere);
	ReleaseNativeActor(Fog, bOwnFog);
	Controller.Reset();
	Camera.Reset();
	FollowTarget.Reset();
	PreviousPawn.Reset();
	PreviousViewTarget.Reset();
	PreviousControlRotation = FRotator::ZeroRotator;
	bPreviousPawnHidden = false;
	bPreviousShowMouseCursor = false;
	bSavedControllerState = false;
	bFollowRobot = false;
	bFollowTargetLogged = false;
	SunLight.Reset();
	Sky.Reset();
	Atmosphere.Reset();
	Fog.Reset();
}
