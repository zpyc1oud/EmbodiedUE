#pragma once

#include "Misc/AutomationTest.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "GameFramework/Actor.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PreviewScene.h"
#include "UObject/UnrealType.h"
#include "UERLPolicyArtifactAsset.h"
#include "UERLPolicyComponent.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UERLPolicyComponentTestsPrivate
{
	/** Pin a valid synchronous-substep deploy baseline for the artifact contract (DtMin=10ms, DtMax=20ms). */
	struct FDeployPhysicsSettingsGuard
	{
		FDeployPhysicsSettingsGuard(float SubstepDelta = 0.005f, int32 Substeps = 6)
		{
			UPhysicsSettings* Settings = GetMutableDefault<UPhysicsSettings>();
			OldTickAsync = Settings->bTickPhysicsAsync;
			OldSubstepping = Settings->bSubstepping;
			OldSubsteppingAsync = Settings->bSubsteppingAsync;
			OldSubstep = Settings->MaxSubstepDeltaTime;
			OldMaxSubsteps = Settings->MaxSubsteps;
			OldMinDelta = Settings->MinPhysicsDeltaTime;
			Settings->bTickPhysicsAsync = false;
			Settings->bSubstepping = true;
			Settings->bSubsteppingAsync = false;
			Settings->MaxSubstepDeltaTime = SubstepDelta;
			Settings->MaxSubsteps = Substeps;
			Settings->MinPhysicsDeltaTime = 0.0f;
		}

		~FDeployPhysicsSettingsGuard()
		{
			UPhysicsSettings* Settings = GetMutableDefault<UPhysicsSettings>();
			Settings->bTickPhysicsAsync = OldTickAsync;
			Settings->bSubstepping = OldSubstepping;
			Settings->bSubsteppingAsync = OldSubsteppingAsync;
			Settings->MaxSubstepDeltaTime = OldSubstep;
			Settings->MaxSubsteps = OldMaxSubsteps;
			Settings->MinPhysicsDeltaTime = OldMinDelta;
		}

		bool OldTickAsync = false;
		bool OldSubstepping = false;
		bool OldSubsteppingAsync = false;
		float OldSubstep = 0.0f;
		int32 OldMaxSubsteps = 0;
		float OldMinDelta = 0.0f;
	};

	inline bool ResolveComponentArtifact(TArray<uint8>& OutBytes, FString& OutError, const TCHAR* FileName)
	{
		const TArray<FString> Candidates = {
			FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT(".."), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("controller"),
				FileName)),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("controller"),
				FileName)),
		};
		for (const FString& Candidate : Candidates)
		{
			if (FFileHelper::LoadFileToArray(OutBytes, *Candidate))
			{
				return true;
			}
		}
		OutError = TEXT("component artifact corpus not found");
		return false;
	}

	inline UUERLPolicyArtifactAsset* MakeTransientAsset(
		FString& OutError, const TCHAR* FileName = TEXT("cartpole_controller.uerlpol2"))
	{
		TArray<uint8> Bytes;
		if (!ResolveComponentArtifact(Bytes, OutError, FileName))
		{
			return nullptr;
		}
		UUERLPolicyArtifactAsset* Asset = NewObject<UUERLPolicyArtifactAsset>(GetTransientPackageAsObject());
		if (!Asset->SetImportedBytes(Bytes, OutError))
		{
			return nullptr;
		}
		Asset->RobotMesh = LoadObject<USkeletalMesh>(nullptr, TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole"));
		return Asset;
	}

	inline AActor* SpawnWorldStaticBox(UWorld& World, const FVector& Location, const FVector& Scale)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube)
		{
			return nullptr;
		}
		AStaticMeshActor* Box = World.SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator);
		if (!Box)
		{
			return nullptr;
		}
		UStaticMeshComponent* Component = Box->GetStaticMeshComponent();
		Component->SetStaticMesh(Cube);
		Component->SetMobility(EComponentMobility::Static);
		Component->SetCollisionObjectType(ECC_WorldStatic);
		Component->SetCollisionResponseToAllChannels(ECR_Block);
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetWorldScale3D(Scale);
		return Box;
	}

	inline AActor* SpawnGround(UWorld& World)
	{
		return SpawnWorldStaticBox(World, FVector(0.0, 0.0, -50.0), FVector(10.0, 10.0, 0.5));
	}

	inline AActor* SpawnDynamicProbe(UWorld& World, const FVector& Location)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube)
		{
			return nullptr;
		}
		AStaticMeshActor* Probe = World.SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), Location, FRotator::ZeroRotator);
		if (!Probe)
		{
			return nullptr;
		}
		UStaticMeshComponent* Component = Probe->GetStaticMeshComponent();
		Component->SetStaticMesh(Cube);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetCollisionObjectType(ECC_PhysicsBody);
		Component->SetCollisionResponseToAllChannels(ECR_Block);
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetSimulatePhysics(true);
		return Probe;
	}

	inline void TickPolicyWorld(UWorld* World, float DeltaSeconds)
	{
		World->Tick(ELevelTick::LEVELTICK_All, DeltaSeconds);
		++GFrameCounter;
	}

	template <typename TDelegate>
	inline void BindRecorderEvent(TDelegate& Delegate, UObject* Recorder, const FName& FunctionName)
	{
		FScriptDelegate ScriptDelegate;
		ScriptDelegate.BindUFunction(Recorder, FunctionName);
		Delegate.Add(ScriptDelegate);
	}

	struct FComponentTestRig
	{
		TUniquePtr<FPreviewScene> PreviewScene;
		AActor* Host = nullptr;
		AActor* Ground = nullptr;
		UUERLPolicyComponent* Component = nullptr;

		bool Build(FAutomationTestBase& Test, UUERLPolicyArtifactAsset* Asset)
		{
			PreviewScene = MakeUnique<FPreviewScene>(
				FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(true).ShouldSimulatePhysics(true));
			UWorld* World = PreviewScene->GetWorld();
			Test.TestNotNull(TEXT("component test preview world"), World);
			if (!World)
			{
				return false;
			}
			Host = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), FTransform(FVector(0.0, 0.0, 25.0)));
			Ground = SpawnGround(*World);
			Test.TestNotNull(TEXT("component test host"), Host);
			Test.TestNotNull(TEXT("component test WorldStatic ground"), Ground);
			if (!Host || !Ground)
			{
				return false;
			}
			Component = NewObject<UUERLPolicyComponent>(Host);
			Host->AddInstanceComponent(Component);
			Component->Artifact = Asset;
			Component->bClaimOwnerMesh = false;
			Component->RegisterComponent();
			return true;
		}

		UWorld* World() const { return PreviewScene->GetWorld(); }
	};

	/** The floating-root PhantomX policy (DtMin=5ms, DtMax=35ms at 5ms physics_dt). */
	inline UUERLPolicyArtifactAsset* RequirePhantomXAsset(FAutomationTestBase& Test)
	{
		UUERLPolicyArtifactAsset* Asset = LoadObject<UUERLPolicyArtifactAsset>(
			nullptr,
			TEXT("/UERLEngine/Policies/PhantomXContinuousTerrain116.PhantomXContinuousTerrain116"));
		Test.TestNotNull(TEXT("PhantomX policy asset loads"), Asset);
		if (!Asset || !Asset->RobotMesh)
		{
			Test.AddError(TEXT("PhantomX policy asset or its RobotMesh is unavailable"));
			return nullptr;
		}
		return Asset;
	}

	inline bool StartWithZeroCommand(FAutomationTestBase& Test, UUERLPolicyComponent* Component)
	{
		for (const FUERLPolicyCommandChannelInfo& Channel : Component->GetRequiredCommandChannels())
		{
			TArray<float> Zeros;
			Zeros.SetNumZeroed(Channel.Width);
			Test.TestTrue(TEXT("zero command latches"), Component->SetCommand(Channel.Name, Zeros));
		}
		const bool bStarted = Component->StartPolicy();
		Test.TestTrue(TEXT("policy starts for timing tests"), bStarted);
		if (!bStarted)
		{
			Test.AddError(FString::Printf(TEXT("StartPolicy error: %s"), *Component->GetLastError()));
		}
		return bStarted;
	}
}

#endif
