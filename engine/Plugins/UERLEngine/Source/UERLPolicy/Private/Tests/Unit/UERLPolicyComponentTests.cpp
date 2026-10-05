#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PreviewScene.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/UnrealType.h"
#include "UERLPolicyArtifactAsset.h"
#include "UERLPolicyComponent.h"
#include "UERLPolicyComponentTestEvents.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	bool ResolveComponentArtifact(TArray<uint8>& OutBytes, FString& OutError)
	{
		const TArray<FString> Candidates = {
			FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT(".."), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("controller"),
				TEXT("cartpole_controller.uerlpol2"))),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("controller"),
				TEXT("cartpole_controller.uerlpol2"))),
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

	UUERLPolicyArtifactAsset* MakeTransientAsset(FString& OutError)
	{
		TArray<uint8> Bytes;
		if (!ResolveComponentArtifact(Bytes, OutError))
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

	AActor* SpawnWorldStaticBox(UWorld& World, const FVector& Location, const FVector& Scale)
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

	AActor* SpawnGround(UWorld& World)
	{
		return SpawnWorldStaticBox(World, FVector(0.0, 0.0, -50.0), FVector(10.0, 10.0, 0.5));
	}

	AActor* SpawnDynamicProbe(UWorld& World, const FVector& Location)
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentReflectionTest,
	"UERL.Unit.Policy.Component.AC_UE_UNIT_COMPONENT_001.ReflectionAndEvents",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentReflectionTest::RunTest(const FString& Parameters)
{
	UClass* Class = UUERLPolicyComponent::StaticClass();
	for (const FName Function : {
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponent, StartPolicy),
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponent, StopPolicy),
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponent, SetCommand),
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponent, GetRequiredCommandChannels),
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponent, IsRunning),
	})
	{
		TestNotNull(*FString::Printf(TEXT("BlueprintCallable function %s"), *Function.ToString()), Class->FindFunctionByName(Function));
	}
	for (const FName Property : {
		GET_MEMBER_NAME_CHECKED(UUERLPolicyComponent, OnControlStepOverrun),
		GET_MEMBER_NAME_CHECKED(UUERLPolicyComponent, OnCommandStale),
		GET_MEMBER_NAME_CHECKED(UUERLPolicyComponent, OnPolicyFault),
		GET_MEMBER_NAME_CHECKED(UUERLPolicyComponent, OnPhysicsBaselineMismatch),
	})
	{
		FMulticastDelegateProperty* Delegate = FindFProperty<FMulticastDelegateProperty>(Class, Property);
		TestNotNull(*FString::Printf(TEXT("BlueprintAssignable event %s"), *Property.ToString()), Delegate);
	}
	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_COMPONENT_001: five BlueprintCallable functions and four assignable events reflect"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentAssetShapeTest,
	"UERL.Unit.Policy.Component.AC_UE_UNIT_COMPONENT_002.AssetShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentAssetShapeTest::RunTest(const FString& Parameters)
{
	UClass* ComponentClass = UUERLPolicyComponent::StaticClass();
	FObjectProperty* ArtifactProperty = FindFProperty<FObjectProperty>(
		ComponentClass, GET_MEMBER_NAME_CHECKED(UUERLPolicyComponent, Artifact));
	TestNotNull(TEXT("Artifact property exists"), ArtifactProperty);
	TestTrue(
		TEXT("Artifact uses UERLPolicyArtifactAsset"),
		ArtifactProperty && ArtifactProperty->PropertyClass == UUERLPolicyArtifactAsset::StaticClass());
	FObjectProperty* MeshProperty = FindFProperty<FObjectProperty>(
		UUERLPolicyArtifactAsset::StaticClass(), GET_MEMBER_NAME_CHECKED(UUERLPolicyArtifactAsset, RobotMesh));
	TestNotNull(TEXT("RobotMesh property exists"), MeshProperty);
	TestTrue(
		TEXT("RobotMesh is a USkeletalMesh reference"),
		MeshProperty && MeshProperty->PropertyClass == USkeletalMesh::StaticClass());
#if WITH_METADATA
	TestTrue(
		TEXT("component has BlueprintSpawnableComponent metadata"),
		ComponentClass->HasMetaData(TEXT("BlueprintSpawnableComponent")));
#else
	AddInfo(TEXT("BlueprintSpawnableComponent metadata is editor-only in this target"));
#endif
	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_COMPONENT_002: Blueprint component and cook-tracked asset mesh references are present"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentConfigShapeTest,
	"UERL.Unit.Policy.Component.AC_UE_UNIT_COMPONENT_003.DerivedConfiguration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentConfigShapeTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("component has no actuator enumeration property"), UUERLPolicyComponent::StaticClass()->FindPropertyByName(TEXT("Actuators")) != nullptr);
	TestFalse(TEXT("component has no observation enumeration property"), UUERLPolicyComponent::StaticClass()->FindPropertyByName(TEXT("Observations")) != nullptr);
	TestFalse(TEXT("component has no control-period property"), UUERLPolicyComponent::StaticClass()->FindPropertyByName(TEXT("ControlPeriodSeconds")) != nullptr);
	TestFalse(TEXT("component has no dt-range property"), UUERLPolicyComponent::StaticClass()->FindPropertyByName(TEXT("DtMin")) != nullptr);
	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_COMPONENT_003: actuator/observation/dt configuration remains artifact-derived"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentAssetImportShapeTest,
	"UERL.Unit.Policy.Component.AC_UE_UNIT_COMPONENT_004.AssetBytesAndSummary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentAssetImportShapeTest::RunTest(const FString& Parameters)
{
	FString Error;
	UUERLPolicyArtifactAsset* Asset = MakeTransientAsset(Error);
	TestNotNull(TEXT("transient policy asset created"), Asset);
	if (!Asset)
	{
		AddError(Error);
		return false;
	}
	const TArray<uint8> Original = Asset->ArtifactBytes;
	const FUERLPolicyArtifactSummary OriginalSummary = Asset->Summary;
	TestTrue(TEXT("imported artifact bytes are non-empty"), !Original.IsEmpty());
	TestEqual(TEXT("summary timing is populated"), Asset->Summary.DecimationMin, 2);
	TestTrue(TEXT("summary command channel is velocity"), Asset->Summary.CommandChannels.Contains(FName(TEXT("velocity"))));
	TArray<uint8> Corrupt = Original;
	Corrupt[0] ^= 0xff;
	TestFalse(TEXT("corrupt reimport fails"), Asset->SetImportedBytes(Corrupt, Error));
	TestTrue(TEXT("failed reimport reports an error"), !Error.IsEmpty());
	TestTrue(TEXT("failed reimport preserves bytes"), Asset->ArtifactBytes == Original);
	TestEqual(TEXT("failed reimport preserves summary"), Asset->Summary.TaskId, OriginalSummary.TaskId);
	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_COMPONENT_004: parsed bytes/timing survive import and failed reimport is transactional"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentLifecycleTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_006.StartStopAndClaimContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentLifecycleTest::RunTest(const FString& Parameters)
{
	FString Error;
	UUERLPolicyArtifactAsset* Asset = MakeTransientAsset(Error);
	TestNotNull(TEXT("lifecycle asset created"), Asset);
	if (!Asset || !Asset->RobotMesh)
	{
		AddError(Error.IsEmpty() ? TEXT("CartPole mesh is unavailable") : *Error);
		return false;
	}
	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(true).ShouldSimulatePhysics(true));
	UWorld* World = PreviewScene->GetWorld();
	TestNotNull(TEXT("lifecycle preview world"), World);
	if (!World)
	{
		return false;
	}
	ATargetPoint* Host = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), FTransform(FVector(0.0, 0.0, 25.0)));
	TestNotNull(TEXT("lifecycle host actor"), Host);
	if (!Host)
	{
		return false;
	}
	AActor* Ground = SpawnGround(*World);
	TestNotNull(TEXT("lifecycle WorldStatic ground"), Ground);
	if (!Ground)
	{
		return false;
	}
	UUERLPolicyComponent* Component = NewObject<UUERLPolicyComponent>(Host);
	Host->AddInstanceComponent(Component);
	Component->Artifact = Asset;
	Component->bClaimOwnerMesh = false;
	Component->RegisterComponent();
	const TArray<FUERLPolicyCommandChannelInfo> BeforeStart = Component->GetRequiredCommandChannels();
	TestTrue(TEXT("required channel is available before Start"), BeforeStart.Num() == 1 && BeforeStart[0].Name == FName(TEXT("velocity")));
	TArray<float> ZeroVelocity = {0.0f};
	TestTrue(TEXT("SetCommand is allowed before Start"), Component->SetCommand(TEXT("velocity"), ZeroVelocity));
	FTransform BeforeStartTransform;
	TestFalse(TEXT("live robot transform is unavailable before Start"), Component->GetRobotTransform(BeforeStartTransform));

	UPhysicsSettings* Settings = GetMutableDefault<UPhysicsSettings>();
	const bool OldTickAsync = Settings->bTickPhysicsAsync;
	const bool OldSubsteppingAsync = Settings->bSubsteppingAsync;
	const bool OldSubstepping = Settings->bSubstepping;
	const float OldSubstep = Settings->MaxSubstepDeltaTime;
	const int32 OldMaxSubsteps = Settings->MaxSubsteps;
	const float OldMinDelta = Settings->MinPhysicsDeltaTime;
	Settings->bTickPhysicsAsync = false;
	Settings->bSubsteppingAsync = false;
	Settings->bSubstepping = true;
	Settings->MaxSubstepDeltaTime = 0.004f;
	Settings->MaxSubsteps = 5;
	Settings->MinPhysicsDeltaTime = 0.0f;
	const bool bStarted = Component->StartPolicy();
	TestTrue(TEXT("explicit Start succeeds with a valid synchronous Chaos baseline"), bStarted);
	if (!bStarted)
	{
		AddError(FString::Printf(TEXT("StartPolicy error: %s"), *Component->GetLastError()));
	}
	if (bStarted)
	{
		TestTrue(TEXT("component reports running"), Component->IsRunning());
		FTransform RobotTransform;
		TestTrue(TEXT("running component exposes the live robot transform"), Component->GetRobotTransform(RobotTransform));
		TestTrue(TEXT("spawned robot stays near the measured owner placement"),
			FVector::Dist(RobotTransform.GetLocation(), Host->GetActorLocation()) < 80.0f);
		TestTrue(TEXT("repeated Start is a no-op"), Component->StartPolicy());
		World->Tick(ELevelTick::LEVELTICK_All, 0.04f);
		++GFrameCounter;
		const FUERLControlTiming BootstrapTiming = Component->GetLastControlTiming();
		TestTrue(TEXT("first completed PostPhysics step records bootstrap timing"),
			FMath::IsNearlyEqual(
				BootstrapTiming.ObservationDtSeconds,
				Asset->Summary.PhysicsDt * Asset->Summary.DecimationMin,
				1.0e-6)
				&& BootstrapTiming.LastSolverStepSeconds > 0.0);
		TestTrue(TEXT("pose reset succeeds against current WorldStatic ground"), Component->ResetToReferencePose());
		TestTrue(TEXT("pose reset clears the previous timing before bootstrap"),
			Component->GetLastControlTiming().ObservationDtSeconds == 0.0);
		Component->StopPolicy();
		TestFalse(TEXT("Stop releases running state"), Component->IsRunning());
		TestTrue(TEXT("Start after Stop succeeds with a clean lifecycle"), Component->StartPolicy());
		Component->StopPolicy();

		AActor* ClaimHost = World->SpawnActor<AActor>(
			AActor::StaticClass(), FTransform(FRotator(0.0f, 37.0f, 0.0f), FVector(300.0, 100.0, 25.0)));
		USkeletalMeshComponent* AuthoredMesh = NewObject<USkeletalMeshComponent>(ClaimHost);
		AuthoredMesh->SetSkeletalMesh(Asset->RobotMesh);
		ClaimHost->AddInstanceComponent(AuthoredMesh);
		USceneComponent* ClaimRoot = NewObject<USceneComponent>(ClaimHost, TEXT("DefaultSceneRoot"));
		ClaimHost->SetRootComponent(ClaimRoot);
		ClaimRoot->RegisterComponent();
		AuthoredMesh->SetupAttachment(ClaimRoot);
		AuthoredMesh->RegisterComponent();
		UUERLPolicyComponent* ClaimComponent = NewObject<UUERLPolicyComponent>(ClaimHost);
		ClaimHost->AddInstanceComponent(ClaimComponent);
		ClaimComponent->Artifact = Asset;
		ClaimComponent->bClaimOwnerMesh = true;
		ClaimComponent->OwnerMesh = AuthoredMesh;
		ClaimComponent->RegisterComponent();
		const FTransform AuthoredTransform = ClaimHost->GetActorTransform();
		TestTrue(TEXT("explicit authored mesh claim starts"), ClaimComponent->StartPolicy());
		TestTrue(TEXT("claim preserves authored Actor placement"),
			ClaimHost->GetActorTransform().Equals(AuthoredTransform, 1.0e-3f));
		TestTrue(TEXT("claimed floating mesh is detached from the kinematic root"),
			AuthoredMesh->GetAttachParent() == nullptr);
		TestEqual(TEXT("claimed mesh component follows simulated physics"),
			AuthoredMesh->PhysicsTransformUpdateMode,
			TEnumAsByte<EPhysicsTransformUpdateMode::Type>(EPhysicsTransformUpdateMode::SimulationUpatesComponentTransform));
		FTransform ClaimRobotTransform;
		TestTrue(TEXT("claimed mesh exposes a live robot transform"),
			ClaimComponent->GetRobotTransform(ClaimRobotTransform));
		TestTrue(TEXT("GetRobotTransform is the claimed mesh, not the frozen owner"),
			ClaimRobotTransform.GetLocation().Equals(AuthoredMesh->GetComponentLocation(), 1.0f));
		ClaimComponent->StopPolicy();
		TestTrue(TEXT("claim Stop leaves authored mesh registered"), AuthoredMesh->IsRegistered());

		AActor* RemapHost = World->SpawnActor<AActor>(
			AActor::StaticClass(), FTransform(FRotator::ZeroRotator, FVector(600.0, 0.0, 25.0)));
		USkeletalMeshComponent* InstanceMesh = NewObject<USkeletalMeshComponent>(RemapHost, TEXT("SkeletalMesh"));
		InstanceMesh->SetSkeletalMesh(Asset->RobotMesh);
		RemapHost->AddInstanceComponent(InstanceMesh);
		InstanceMesh->RegisterComponent();
		USkeletalMeshComponent* TemplateMesh = NewObject<USkeletalMeshComponent>(GetTransientPackage(), TEXT("SkeletalMesh"));
		TemplateMesh->SetSkeletalMesh(Asset->RobotMesh);
		UUERLPolicyComponent* RemapComponent = NewObject<UUERLPolicyComponent>(RemapHost);
		RemapHost->AddInstanceComponent(RemapComponent);
		RemapComponent->Artifact = Asset;
		RemapComponent->bClaimOwnerMesh = true;
		RemapComponent->OwnerMesh = TemplateMesh;
		RemapComponent->RegisterComponent();
		TestTrue(TEXT("Blueprint CDO OwnerMesh remaps to the owner instance by name"), RemapComponent->StartPolicy());
		if (!RemapComponent->IsRunning())
		{
			AddError(FString::Printf(TEXT("CDO OwnerMesh remap error: %s"), *RemapComponent->GetLastError()));
		}
		RemapComponent->StopPolicy();

		USkeletalMeshComponent* GeneratedTemplate = NewObject<USkeletalMeshComponent>(
			GetTransientPackage(), TEXT("SkeletalMesh_GEN_VARIABLE"));
		GeneratedTemplate->SetSkeletalMesh(Asset->RobotMesh);
		RemapComponent->OwnerMesh = GeneratedTemplate;
		TestTrue(TEXT("Blueprint generated OwnerMesh remaps by stripped name"), RemapComponent->StartPolicy());
		if (!RemapComponent->IsRunning())
		{
			AddError(FString::Printf(TEXT("GEN_VARIABLE OwnerMesh remap error: %s"), *RemapComponent->GetLastError()));
		}
		RemapComponent->StopPolicy();
		AddExpectedError(
			TEXT("explicit OwnerMesh does not belong to the owner"),
			EAutomationExpectedErrorFlags::Contains,
			1);
		RemapComponent->OwnerMesh = AuthoredMesh;
		TestFalse(TEXT("OwnerMesh on a live foreign actor is rejected"), RemapComponent->StartPolicy());
	}
	Settings->bTickPhysicsAsync = OldTickAsync;
	Settings->bSubsteppingAsync = OldSubsteppingAsync;
	Settings->bSubstepping = OldSubstepping;
	Settings->MaxSubstepDeltaTime = OldSubstep;
	Settings->MaxSubsteps = OldMaxSubsteps;
	Settings->MinPhysicsDeltaTime = OldMinDelta;
	AddInfo(TEXT("[VERIFY] AC_UE_INT_COMPONENT_006: explicit Start/Stop owns lifecycle and does not require a source artifact file"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentAutoStartTest,
	"UERL.Integration.Policy.Component.AutoStartOnFirstTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentAutoStartTest::RunTest(const FString& Parameters)
{
	FString Error;
	UUERLPolicyArtifactAsset* Asset = MakeTransientAsset(Error);
	TestNotNull(TEXT("auto-start asset created"), Asset);
	if (!Asset || !Asset->RobotMesh)
	{
		AddError(Error.IsEmpty() ? TEXT("CartPole mesh is unavailable") : *Error);
		return false;
	}
	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(true).ShouldSimulatePhysics(true));
	UWorld* World = PreviewScene->GetWorld();
	TestNotNull(TEXT("auto-start preview world"), World);
	if (!World)
	{
		return false;
	}
	ATargetPoint* Host = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), FTransform(FVector(0.0, 0.0, 25.0)));
	AActor* Ground = SpawnGround(*World);
	TestNotNull(TEXT("auto-start ground"), Ground);
	if (!Host || !Ground)
	{
		return false;
	}
	UUERLPolicyComponent* Component = NewObject<UUERLPolicyComponent>(Host);
	Host->AddInstanceComponent(Component);
	Component->Artifact = Asset;
	Component->bClaimOwnerMesh = false;
	Component->bAutoStart = true;
	Component->RegisterComponent();
	UPhysicsSettings* Settings = GetMutableDefault<UPhysicsSettings>();
	const bool OldSubstepping = Settings->bSubstepping;
	const bool OldTickAsync = Settings->bTickPhysicsAsync;
	const bool OldSubsteppingAsync = Settings->bSubsteppingAsync;
	Settings->bTickPhysicsAsync = false;
	Settings->bSubsteppingAsync = false;
	Settings->bSubstepping = true;
	Settings->MaxSubstepDeltaTime = 0.004f;
	Settings->MaxSubsteps = 5;
	if (!Component->HasBegunPlay())
	{
		Component->BeginPlay();
	}
	TestFalse(TEXT("BeginPlay does not start before the first tick"), Component->IsRunning());
	TestTrue(TEXT("zero command latches before the first tick"), Component->SetCommand(TEXT("velocity"), {0.0f}));
	World->Tick(ELevelTick::LEVELTICK_All, 0.04f);
	++GFrameCounter;
	TestTrue(TEXT("first tick auto-starts the policy"), Component->IsRunning());
	if (!Component->IsRunning())
	{
		AddError(FString::Printf(TEXT("auto-start error: %s"), *Component->GetLastError()));
	}
	Component->StopPolicy();
	Settings->bSubstepping = OldSubstepping;
	Settings->bTickPhysicsAsync = OldTickAsync;
	Settings->bSubsteppingAsync = OldSubsteppingAsync;
	AddInfo(TEXT("[VERIFY] auto-start waits for Actor BeginPlay commands, then starts on the first tick"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentClearanceProbeTest,
	"UERL.Integration.Policy.Component.MeasuredStartClearance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentClearanceProbeTest::RunTest(const FString& Parameters)
{
	FString Error;
	UUERLPolicyArtifactAsset* Asset = MakeTransientAsset(Error);
	TestNotNull(TEXT("clearance asset created"), Asset);
	if (!Asset || !Asset->RobotMesh)
	{
		AddError(Error.IsEmpty() ? TEXT("CartPole mesh is unavailable") : *Error);
		return false;
	}
	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(true).ShouldSimulatePhysics(true));
	UWorld* World = PreviewScene->GetWorld();
	if (!World || !Asset)
	{
		return false;
	}
	ATargetPoint* Host = World->SpawnActor<ATargetPoint>(ATargetPoint::StaticClass(), FTransform::Identity);
	UUERLPolicyComponent* Component = NewObject<UUERLPolicyComponent>(Host);
	Host->AddInstanceComponent(Component);
	Component->Artifact = Asset;
	Component->bClaimOwnerMesh = false;
	Component->bAutoStart = false;
	Component->RegisterComponent();
	UPhysicsSettings* Settings = GetMutableDefault<UPhysicsSettings>();
	Settings->bTickPhysicsAsync = false;
	Settings->bSubsteppingAsync = false;
	Settings->bSubstepping = true;
	Settings->MaxSubstepDeltaTime = 0.004f;
	Settings->MaxSubsteps = 5;
	AddExpectedError(
		TEXT("no WorldStatic ground under the robot to measure start clearance"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(TEXT("Start fails without WorldStatic ground"), Component->StartPolicy());
	TestTrue(TEXT("missing ground is a start clearance error"),
		Component->GetLastError().Contains(TEXT("clearance"))
		|| Component->GetLastError().Contains(TEXT("ground")));
	AddInfo(TEXT("[VERIFY] Start measures WorldStatic clearance and fails when none exists"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentClearanceIgnoresOverheadAndOwnerTest,
	"UERL.Integration.Policy.Component.StartClearanceIgnoresOverheadAndOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentClearanceIgnoresOverheadAndOwnerTest::RunTest(const FString& Parameters)
{
	FString Error;
	UUERLPolicyArtifactAsset* Asset = MakeTransientAsset(Error);
	TestNotNull(TEXT("overhead-clearance asset created"), Asset);
	if (!Asset || !Asset->RobotMesh)
	{
		AddError(Error.IsEmpty() ? TEXT("CartPole mesh is unavailable") : *Error);
		return false;
	}
	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false).SetCreatePhysicsScene(true).ShouldSimulatePhysics(true));
	UWorld* World = PreviewScene->GetWorld();
	if (!World)
	{
		return false;
	}
	AActor* Ground = SpawnGround(*World);
	AActor* Ceiling = SpawnWorldStaticBox(*World, FVector(0.0, 0.0, 400.0), FVector(10.0, 10.0, 0.5));
	TestNotNull(TEXT("WorldStatic ground under the robot"), Ground);
	TestNotNull(TEXT("WorldStatic ceiling above the robot"), Ceiling);
	if (!Ground || !Ceiling)
	{
		return false;
	}

	AActor* ClaimHost = World->SpawnActor<AActor>(
		AActor::StaticClass(), FTransform(FVector(0.0, 0.0, 25.0)));
	TestNotNull(TEXT("claimed host actor"), ClaimHost);
	if (!ClaimHost)
	{
		return false;
	}
	USkeletalMeshComponent* AuthoredMesh = NewObject<USkeletalMeshComponent>(ClaimHost);
	AuthoredMesh->SetSkeletalMesh(Asset->RobotMesh);
	ClaimHost->AddInstanceComponent(AuthoredMesh);
	AuthoredMesh->RegisterComponent();

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TestNotNull(TEXT("owner WorldStatic shell mesh"), Cube);
	if (!Cube)
	{
		return false;
	}
	UStaticMeshComponent* OwnerShell = NewObject<UStaticMeshComponent>(ClaimHost);
	ClaimHost->AddInstanceComponent(OwnerShell);
	OwnerShell->SetStaticMesh(Cube);
	OwnerShell->SetWorldLocation(ClaimHost->GetActorLocation());
	OwnerShell->SetCollisionObjectType(ECC_WorldStatic);
	OwnerShell->SetCollisionResponseToAllChannels(ECR_Block);
	OwnerShell->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	OwnerShell->RegisterComponent();

	UUERLPolicyComponent* Component = NewObject<UUERLPolicyComponent>(ClaimHost);
	ClaimHost->AddInstanceComponent(Component);
	Component->Artifact = Asset;
	Component->bClaimOwnerMesh = true;
	Component->OwnerMesh = AuthoredMesh;
	Component->bAutoStart = false;
	Component->RegisterComponent();

	UPhysicsSettings* Settings = GetMutableDefault<UPhysicsSettings>();
	const bool OldTickAsync = Settings->bTickPhysicsAsync;
	const bool OldSubsteppingAsync = Settings->bSubsteppingAsync;
	const bool OldSubstepping = Settings->bSubstepping;
	const float OldSubstep = Settings->MaxSubstepDeltaTime;
	const int32 OldMaxSubsteps = Settings->MaxSubsteps;
	Settings->bTickPhysicsAsync = false;
	Settings->bSubsteppingAsync = false;
	Settings->bSubstepping = true;
	Settings->MaxSubstepDeltaTime = 0.004f;
	Settings->MaxSubsteps = 5;

	const bool bStarted = Component->StartPolicy();
	TestTrue(TEXT("Start succeeds with ground under an overhead WorldStatic and an owner WorldStatic shell"), bStarted);
	if (!bStarted)
	{
		AddError(FString::Printf(TEXT("StartPolicy error: %s"), *Component->GetLastError()));
	}
	Component->StopPolicy();
	Settings->bTickPhysicsAsync = OldTickAsync;
	Settings->bSubsteppingAsync = OldSubsteppingAsync;
	Settings->bSubstepping = OldSubstepping;
	Settings->MaxSubstepDeltaTime = OldSubstep;
	Settings->MaxSubsteps = OldMaxSubsteps;
	AddInfo(TEXT("[VERIFY] Start measures downward WorldStatic clearance and ignores the owner"));
	return true;
}

namespace
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

	void TickPolicyWorld(UWorld* World, float DeltaSeconds)
	{
		World->Tick(ELevelTick::LEVELTICK_All, DeltaSeconds);
		++GFrameCounter;
	}

	template <typename TDelegate>
	void BindRecorderEvent(TDelegate& Delegate, UObject* Recorder, const FName& FunctionName)
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
	UUERLPolicyArtifactAsset* RequirePhantomXAsset(FAutomationTestBase& Test)
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

	bool StartWithZeroCommand(FAutomationTestBase& Test, UUERLPolicyComponent* Component)
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentWindowDtSequenceTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_007.BootstrapThenCompletedWindowsDriveObservationDt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentWindowDtSequenceTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	const TArray<FUERLPolicyCommandChannelInfo> BeforeStart = Rig.Component->GetRequiredCommandChannels();
	TestTrue(TEXT("summary exposes the command channels before Start"),
		BeforeStart.Num() == Asset->Summary.CommandChannels.Num());
	for (int32 Index = 0; Index < BeforeStart.Num(); ++Index)
	{
		TestTrue(TEXT("summary channel names and widths match"),
			BeforeStart[Index].Name == Asset->Summary.CommandChannels[Index]
			&& BeforeStart[Index].Width == Asset->Summary.CommandWidths[Index]);
	}

	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}
	const double DtMin = Asset->Summary.PhysicsDt * Asset->Summary.DecimationMin;
	constexpr double Tolerance = 2.0e-3;

	TickPolicyWorld(Rig.World(), 0.005f);
	TestTrue(TEXT("bootstrap observation dt is DtMin"),
		FMath::Abs(Rig.Component->GetLastControlTiming().ObservationDtSeconds - DtMin) < Tolerance);

	TickPolicyWorld(Rig.World(), 0.005f);
	TestTrue(TEXT("first completed 5ms window reports 5ms"),
		FMath::Abs(Rig.Component->GetLastControlTiming().ObservationDtSeconds - 0.005) < Tolerance);

	TickPolicyWorld(Rig.World(), 0.035f);
	TestTrue(TEXT("completed 35ms window reports 35ms with no extra delay"),
		FMath::Abs(Rig.Component->GetLastControlTiming().ObservationDtSeconds - 0.035) < Tolerance);

	TickPolicyWorld(Rig.World(), 0.010f);
	TestTrue(TEXT("completed 10ms window reports 10ms, not the previous 35ms"),
		FMath::Abs(Rig.Component->GetLastControlTiming().ObservationDtSeconds - 0.010) < Tolerance);

	Rig.Component->StopPolicy();
	AddInfo(TEXT("[VERIFY] AC_UE_INT_COMPONENT_007: bootstrap uses DtMin and each observation carries its just-completed window dt"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentShortFrameAccumulationTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_008.ShortFramesAccumulateOnceAndPauseForcesNoProgress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentShortFrameAccumulationTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	// 2ms substeps make every 2ms game frame land exactly on one solver step.
	FDeployPhysicsSettingsGuard Guard(0.002f, 20);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}
	constexpr double Tolerance = 2.0e-3;

	TickPolicyWorld(Rig.World(), 0.002f);
	const FUERLControlTiming Bootstrap = Rig.Component->GetLastControlTiming();
	TestTrue(TEXT("bootstrap consumed the first short frame at DtMin"),
		FMath::Abs(Bootstrap.ObservationDtSeconds - 0.005) < Tolerance);

	TickPolicyWorld(Rig.World(), 0.002f);
	TestTrue(TEXT("2ms below DtMin does not infer"),
		Rig.Component->GetLastControlTiming().ObservationDtSeconds == Bootstrap.ObservationDtSeconds
		&& Rig.Component->GetLastControlTiming().LastSolverStepSeconds == Bootstrap.LastSolverStepSeconds);

	TickPolicyWorld(Rig.World(), 0.002f);
	TestTrue(TEXT("4ms below DtMin still does not infer"),
		Rig.Component->GetLastControlTiming().ObservationDtSeconds == Bootstrap.ObservationDtSeconds);

	TickPolicyWorld(Rig.World(), 0.002f);
	TestTrue(TEXT("2+2+2ms accumulated infers once with the actual 6ms of physics time"),
		FMath::Abs(Rig.Component->GetLastControlTiming().ObservationDtSeconds - 0.006) < Tolerance);

	const FUERLControlTiming BeforePause = Rig.Component->GetLastControlTiming();
	for (int32 PauseTick = 0; PauseTick < 3; ++PauseTick)
	{
		// A paused solver cannot advance the completed clock; game time ages but
		// no fake physics window may be produced.
		Rig.Component->TickComponent(0.001f, ELevelTick::LEVELTICK_All, nullptr);
	}
	TestTrue(TEXT("paused solver fabricates no new control step"),
		Rig.Component->GetLastControlTiming().ObservationDtSeconds == BeforePause.ObservationDtSeconds);

	Rig.Component->StopPolicy();
	AddInfo(TEXT("[VERIFY] AC_UE_INT_COMPONENT_008: short frames merge into one inference with the real accumulated dt; pauses fabricate nothing"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentOverrunEventTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_009.OverrunEventReportsThreeAccurateTimesOncePerWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentOverrunEventTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	// Seven 5ms substeps cap one frame at 35ms of physics, right at DtMax.
	FDeployPhysicsSettingsGuard Guard(0.005f, 7);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}
	constexpr double Tolerance = 2.0e-3;

	UUERLPolicyComponentTestEventRecorder* Recorder = NewObject<UUERLPolicyComponentTestEventRecorder>();
	BindRecorderEvent(
		Rig.Component->OnControlStepOverrun,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnOverrun));

	TickPolicyWorld(Rig.World(), 0.005f);
	TestEqual(TEXT("in-range window does not overrun"), Recorder->OverrunCount, 0);

	// A 40ms game frame exceeds the 35ms substep capacity: physics drops 5ms.
	TickPolicyWorld(Rig.World(), 0.040f);
	TestEqual(TEXT("over-capacity long frame overruns exactly once"), Recorder->OverrunCount, 1);
	TestTrue(TEXT("overrun reports the real game time"),
		FMath::Abs(Recorder->LastGameSeconds - 0.040f) < 1.0e-4f);
	TestTrue(TEXT("overrun reports the real physics time capped by substep capacity"),
		FMath::Abs(Recorder->LastPhysicsSeconds - 0.035f) < 1.0e-4f);
	TestTrue(TEXT("overrun reports the observation clamped to DtMax"),
		FMath::Abs(Recorder->LastObservationSeconds - 0.035f) < 1.0e-4f);

	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("the next window keeps no remainder from the overrun"), Recorder->OverrunCount, 1);
	TestTrue(TEXT("next observation dt is the fresh 10ms window"),
		FMath::Abs(Rig.Component->GetLastControlTiming().ObservationDtSeconds - 0.010) < Tolerance);

	// Game-only overrun: aging game time while the solver is paused must not
	// clamp the observation once physics resumes.
	Rig.Component->TickComponent(0.030f, ELevelTick::LEVELTICK_All, nullptr);
	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("game-only overrun fires on the resumed window"), Recorder->OverrunCount, 2);
	TestTrue(TEXT("game-only overrun reports aged game time"),
		FMath::Abs(Recorder->LastGameSeconds - 0.040f) < 1.0e-4f);
	TestTrue(TEXT("game-only overrun keeps the observation on the real 10ms window"),
		FMath::Abs(Recorder->LastObservationSeconds - 0.010f) < 1.0e-4f
		&& FMath::Abs(Rig.Component->GetLastControlTiming().ObservationDtSeconds - 0.010) < Tolerance);

	Rig.Component->StopPolicy();
	AddInfo(TEXT("[VERIFY] AC_UE_INT_COMPONENT_009: overrun fires once per window with accurate game/physics/observation times for physical drops and game-only aging"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentCommandStalenessTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_010.CommandStalenessFiresOnceAndRearmsOnSetCommand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentCommandStalenessTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	Rig.Component->CommandStalenessSeconds = 0.015;
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}

	UUERLPolicyComponentTestEventRecorder* Recorder = NewObject<UUERLPolicyComponentTestEventRecorder>();
	BindRecorderEvent(
		Rig.Component->OnCommandStale,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnStale));

	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("10ms old command is not stale"), Recorder->StaleCount, 0);
	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("20ms old command crosses the threshold once"), Recorder->StaleCount, 1);
	TestEqual(TEXT("stale event names the velocity channel"), Recorder->LastChannel, FName(TEXT("velocity")));
	TestTrue(TEXT("stale seconds accumulate game time"),
		FMath::Abs(Recorder->LastStaleSeconds - 0.020f) < 1.0e-4f);
	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("an already-stale channel does not refire"), Recorder->StaleCount, 1);

	int32 StaleChannelWidth = 0;
	for (const FUERLPolicyCommandChannelInfo& Channel : Rig.Component->GetRequiredCommandChannels())
	{
		if (Channel.Name == Recorder->LastChannel)
		{
			StaleChannelWidth = Channel.Width;
		}
	}
	TArray<float> ReLatch;
	ReLatch.SetNumZeroed(StaleChannelWidth);
	TestTrue(TEXT("re-latching the command succeeds"),
		Rig.Component->SetCommand(Recorder->LastChannel, ReLatch));
	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("SetCommand rearms the channel"), Recorder->StaleCount, 1);
	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("the rearmed channel can go stale again"), Recorder->StaleCount, 2);

	Rig.Component->StopPolicy();
	AddInfo(TEXT("[VERIFY] AC_UE_INT_COMPONENT_010: staleness fires once per channel on game time and rearms on SetCommand"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentOverrunCallbackGuardTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_011.OverrunCallbackStopOrResetExitsTheCurrentTick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentOverrunCallbackGuardTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	// Ten 5ms substeps let a 40ms frame fully advance past DtMax=35ms.
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}
	constexpr double Tolerance = 2.0e-3;

	UUERLPolicyComponent* Component = Rig.Component;
	UUERLPolicyComponentTestEventRecorder* Recorder = NewObject<UUERLPolicyComponentTestEventRecorder>();
	Recorder->CallbackComponent = Component;
	BindRecorderEvent(
		Component->OnControlStepOverrun,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnOverrunStopOrReset));
	BindRecorderEvent(
		Component->OnPolicyFault,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnFault));

	TickPolicyWorld(Rig.World(), 0.010f);
	TickPolicyWorld(Rig.World(), 0.040f);
	TestFalse(TEXT("Stop inside the overrun callback stops the policy"), Component->IsRunning());
	TestEqual(TEXT("stopping mid-tick does not fault"), Recorder->FaultCount, 0);
	TestTrue(TEXT("the stopped tick consumes no control step"),
		Component->GetLastControlTiming().ObservationDtSeconds == 0.0
		&& Component->GetLastControlTiming().LastSolverStepSeconds == 0.0);

	TestTrue(TEXT("restart after the stopped tick succeeds"), Component->StartPolicy());
	for (const FUERLPolicyCommandChannelInfo& Channel : Component->GetRequiredCommandChannels())
	{
		TArray<float> Zeros;
		Zeros.SetNumZeroed(Channel.Width);
		Component->SetCommand(Channel.Name, Zeros);
	}
	TickPolicyWorld(Rig.World(), 0.010f);
	Recorder->CallbackMode = 1;
	TickPolicyWorld(Rig.World(), 0.040f);
	TestTrue(TEXT("SoftReset inside the overrun callback keeps the policy running"), Component->IsRunning());
	TestTrue(TEXT("the reset tick consumes no control step"),
		Component->GetLastControlTiming().ObservationDtSeconds == 0.0);
	TickPolicyWorld(Rig.World(), 0.010f);
	TestTrue(TEXT("the tick after a callback reset bootstraps at DtMin"),
		FMath::Abs(Component->GetLastControlTiming().ObservationDtSeconds - 0.005) < Tolerance);

	Component->StopPolicy();
	AddInfo(TEXT("[VERIFY] AC_UE_INT_COMPONENT_011: callbacks that Stop or Reset exit the old tick without inference or resource access"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentResetSemanticsTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_012.ResetsClearTimingAndFailuresAreVisible",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentResetSemanticsTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}
	constexpr double Tolerance = 2.0e-3;

	UUERLPolicyComponentTestEventRecorder* Recorder = NewObject<UUERLPolicyComponentTestEventRecorder>();
	BindRecorderEvent(
		Rig.Component->OnPolicyFault,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnFault));

	TickPolicyWorld(Rig.World(), 0.010f);
	TickPolicyWorld(Rig.World(), 0.010f);
	FTransform PoseBeforeSoftReset;
	TestTrue(TEXT("robot transform is available before SoftReset"),
		Rig.Component->GetRobotTransform(PoseBeforeSoftReset));
	TestTrue(TEXT("SoftReset succeeds"), Rig.Component->SoftReset());
	FTransform PoseAfterSoftReset;
	TestTrue(TEXT("robot transform is available after SoftReset"),
		Rig.Component->GetRobotTransform(PoseAfterSoftReset));
	TestTrue(TEXT("SoftReset keeps the robot pose"),
		PoseAfterSoftReset.GetLocation().Equals(PoseBeforeSoftReset.GetLocation(), 0.5f));
	TestTrue(TEXT("SoftReset clears the consumed timing"),
		Rig.Component->GetLastControlTiming().ObservationDtSeconds == 0.0);
	TickPolicyWorld(Rig.World(), 0.010f);
	TestTrue(TEXT("SoftReset re-bootstraps at DtMin"),
		FMath::Abs(Rig.Component->GetLastControlTiming().ObservationDtSeconds - 0.005) < Tolerance);

	FTransform StartPose;
	TestTrue(TEXT("robot transform is available before pose reset"),
		Rig.Component->GetRobotTransform(StartPose));
	TickPolicyWorld(Rig.World(), 0.020f);
	TestTrue(TEXT("pose reset succeeds on the current WorldStatic ground"),
		Rig.Component->ResetToReferencePose());
	FTransform ResetPose;
	TestTrue(TEXT("robot transform is available after pose reset"),
		Rig.Component->GetRobotTransform(ResetPose));
	TestTrue(TEXT("pose reset lands back on the measured start clearance"),
		FMath::Abs(ResetPose.GetLocation().Z - StartPose.GetLocation().Z) < 5.0);
	TestTrue(TEXT("pose reset clears the consumed timing"),
		Rig.Component->GetLastControlTiming().ObservationDtSeconds == 0.0);

	Rig.Ground->Destroy();
	TestFalse(TEXT("pose reset without ground fails instead of reusing the last height"),
		Rig.Component->ResetToReferencePose());
	TestEqual(TEXT("the failed pose reset faults once"), Recorder->FaultCount, 1);
	TestFalse(TEXT("the failed pose reset stops inference"), Rig.Component->IsRunning());

	AddInfo(TEXT("[VERIFY] AC_UE_INT_COMPONENT_012: resets clear history/timing, SoftReset keeps pose, pose reset follows current ground and fails visibly"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentGateFailureBindingTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_013.BoundHandlersObserveGateFailureWithoutDrive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentGateFailureBindingTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	UUERLPolicyComponentTestEventRecorder* Recorder = NewObject<UUERLPolicyComponentTestEventRecorder>();
	BindRecorderEvent(
		Rig.Component->OnPhysicsBaselineMismatch,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnMismatch));
	BindRecorderEvent(
		Rig.Component->OnPolicyFault,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnFault));

	// Events bound before Start must observe the gate failure exactly once.
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	GetMutableDefault<UPhysicsSettings>()->bSubstepping = false;
	AddExpectedError(
		TEXT("deployment physics gate failed"),
		EAutomationExpectedErrorFlags::Contains,
		1);
	TestFalse(TEXT("Start fails when synchronous substepping is off"), Rig.Component->StartPolicy());
	TestEqual(TEXT("baseline mismatch event fires once"), Recorder->MismatchCount, 1);
	TestTrue(TEXT("baseline mismatch report is diagnosable"), !Recorder->MismatchReport.IsEmpty());
	TestEqual(TEXT("policy fault event fires once"), Recorder->FaultCount, 1);
	TestFalse(TEXT("the failed Start does not run"), Rig.Component->IsRunning());

	TickPolicyWorld(Rig.World(), 0.010f);
	FTransform RobotTransform;
	TestFalse(TEXT("no robot exists to drive after a gate failure"),
		Rig.Component->GetRobotTransform(RobotTransform));
	TestTrue(TEXT("no control step was ever consumed"),
		Rig.Component->GetLastControlTiming().ObservationDtSeconds == 0.0
		&& Rig.Component->GetLastControlTiming().LastSolverStepSeconds == 0.0);

	AddInfo(TEXT("[VERIFY] AC_UE_INT_COMPONENT_013: handlers bound before Start observe one gate failure with no drive afterwards"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentForceDenominatorTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_014.OverrunKeepsForceDenominatorOnLastSolverStep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentForceDenominatorTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}

	TickPolicyWorld(Rig.World(), 0.010f);
	TickPolicyWorld(Rig.World(), 0.040f);
	const FUERLControlTiming OverrunTiming = Rig.Component->GetLastControlTiming();
	TestTrue(TEXT("overrun observation clamps to DtMax"),
		FMath::Abs(OverrunTiming.ObservationDtSeconds - 0.035) < 2.0e-3);
	TestTrue(TEXT("force denominator stays the last solver substep"),
		FMath::Abs(OverrunTiming.LastSolverStepSeconds - 0.005) < 1.0e-3);
	TestTrue(TEXT("the clamped window never becomes the force denominator"),
		FMath::Abs(OverrunTiming.LastSolverStepSeconds - OverrunTiming.ObservationDtSeconds) > 1.0e-3);

	Rig.Component->StopPolicy();
	AddInfo(TEXT("[VERIFY] AC_UE_INT_COMPONENT_014: observation clamp and contact-force denominator never mix"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentClaimedResetTest,
	"UERL.Integration.Policy.Component.AC_UE_REVIEW_030.ClaimedFloatingResetKeepsCurrentXYAndSnapsToGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentClaimedResetTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	AActor* ClaimHost = Rig.Host;
	USkeletalMeshComponent* AuthoredMesh = NewObject<USkeletalMeshComponent>(ClaimHost);
	AuthoredMesh->SetSkeletalMesh(Asset->RobotMesh);
	ClaimHost->AddInstanceComponent(AuthoredMesh);
	USceneComponent* ClaimRoot = NewObject<USceneComponent>(ClaimHost, TEXT("DefaultSceneRoot"));
	ClaimHost->SetRootComponent(ClaimRoot);
	ClaimRoot->RegisterComponent();
	AuthoredMesh->SetupAttachment(ClaimRoot);
	AuthoredMesh->RegisterComponent();

	// Measure the start clearance from the authored mesh location with the same
	// WorldStatic query the component uses at Start.
	const FVector AuthoredMeshLocation = AuthoredMesh->GetComponentLocation();
	FCollisionQueryParams ClearanceParams(FCollisionQueryParams::DefaultQueryParam);
	FHitResult StartClearanceHit;
	const bool bStartClearance = Rig.World()->LineTraceSingleByObjectType(
		StartClearanceHit,
		AuthoredMeshLocation + FVector::UpVector * 10000.0,
		AuthoredMeshLocation - FVector::UpVector * 10000.0,
		FCollisionObjectQueryParams(ECC_WorldStatic),
		ClearanceParams);
	TestTrue(TEXT("start clearance query hits WorldStatic ground"), bStartClearance);
	const double StartClearanceCm = AuthoredMeshLocation.Z - StartClearanceHit.ImpactPoint.Z;

	Rig.Component->bClaimOwnerMesh = true;
	Rig.Component->OwnerMesh = AuthoredMesh;
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}

	TickPolicyWorld(Rig.World(), 0.010f);
	FTransform StartPose;
	TestTrue(TEXT("claimed robot transform is available after bootstrap"),
		Rig.Component->GetRobotTransform(StartPose));
	const FTransform OwnerTransform = ClaimHost->GetActorTransform();

	// The robot "walked away" from the authored placement.
	AuthoredMesh->SetWorldLocation(FVector(200.0, 100.0, 25.0), false, nullptr, ETeleportType::TeleportPhysics);
	TestTrue(TEXT("claimed reference-pose reset succeeds away from the owner"),
		Rig.Component->ResetToReferencePose());
	FTransform ResetPose;
	TestTrue(TEXT("claimed robot transform is available after the reset"),
		Rig.Component->GetRobotTransform(ResetPose));
	TestTrue(TEXT("claimed floating root keeps its current XY"),
		FMath::Abs(ResetPose.GetLocation().X - 200.0) < 2.0
		&& FMath::Abs(ResetPose.GetLocation().Y - 100.0) < 2.0);
	FHitResult ResetGroundHit;
	const bool bResetGround = Rig.World()->LineTraceSingleByObjectType(
		ResetGroundHit,
		ResetPose.GetLocation() + FVector::UpVector * 10000.0,
		ResetPose.GetLocation() - FVector::UpVector * 10000.0,
		FCollisionObjectQueryParams(ECC_WorldStatic),
		ClearanceParams);
	TestTrue(TEXT("reset ground query hits WorldStatic ground"), bResetGround);
	TestTrue(TEXT("claimed floating root snaps to the current ground plus the start clearance"),
		FMath::Abs(ResetPose.GetLocation().Z - (ResetGroundHit.ImpactPoint.Z + StartClearanceCm)) < 2.0);
	TestTrue(TEXT("the claimed owner transform is not rewritten"),
		ClaimHost->GetActorTransform().Equals(OwnerTransform, 1.0e-3));
	TestTrue(TEXT("pose reset zeroes the root velocity"),
		AuthoredMesh->GetPhysicsLinearVelocity().SizeSquared() < 1.0);

	Rig.Component->StopPolicy();
	AddInfo(TEXT("[VERIFY] AC_UE_REVIEW_030: claimed reset keeps current XY/yaw on current ground without moving the owner"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentControlledMeshSyncTest,
	"UERL.Integration.Policy.Component.AC_UE_REVIEW_031.TickSyncsOnlyTheControlledMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentControlledMeshSyncTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	AActor* ClaimHost = Rig.Host;
	// The uncontrolled mesh is created first so a class-wide lookup would find it.
	USkeletalMeshComponent* OtherMesh = NewObject<USkeletalMeshComponent>(ClaimHost);
	OtherMesh->SetSkeletalMesh(Asset->RobotMesh);
	ClaimHost->AddInstanceComponent(OtherMesh);
	USceneComponent* ClaimRoot = NewObject<USceneComponent>(ClaimHost, TEXT("DefaultSceneRoot"));
	ClaimHost->SetRootComponent(ClaimRoot);
	ClaimRoot->RegisterComponent();
	OtherMesh->SetupAttachment(ClaimRoot);
	OtherMesh->RegisterComponent();
	OtherMesh->SetWorldLocation(FVector(0.0, 0.0, 300.0));
	const FTransform OtherMeshTransform = OtherMesh->GetComponentTransform();

	USkeletalMeshComponent* AuthoredMesh = NewObject<USkeletalMeshComponent>(ClaimHost);
	AuthoredMesh->SetSkeletalMesh(Asset->RobotMesh);
	ClaimHost->AddInstanceComponent(AuthoredMesh);
	AuthoredMesh->SetupAttachment(ClaimRoot);
	AuthoredMesh->RegisterComponent();

	Rig.Component->bClaimOwnerMesh = true;
	Rig.Component->OwnerMesh = AuthoredMesh;
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}

	TickPolicyWorld(Rig.World(), 0.010f);
	TickPolicyWorld(Rig.World(), 0.010f);
	FTransform RobotTransform;
	TestTrue(TEXT("robot transform is available while running"),
		Rig.Component->GetRobotTransform(RobotTransform));
	TestTrue(TEXT("the reported robot transform tracks the claimed mesh"),
		RobotTransform.GetLocation().Equals(AuthoredMesh->GetComponentLocation(), 1.0f));
	TestTrue(TEXT("the uncontrolled mesh is never synced or moved"),
		OtherMesh->GetComponentTransform().Equals(OtherMeshTransform, 1.0e-3f));

	Rig.Component->StopPolicy();
	AddInfo(TEXT("[VERIFY] AC_UE_REVIEW_031: ticks sync the controlled mesh only, even when another mesh is found first"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentClaimReleaseTest,
	"UERL.Integration.Policy.Component.AC_UE_REVIEW_032.ClaimReleaseRestoresAttachmentAndDriveState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentClaimReleaseTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	AActor* ClaimHost = Rig.Host;
	USkeletalMeshComponent* AuthoredMesh = NewObject<USkeletalMeshComponent>(ClaimHost);
	AuthoredMesh->SetSkeletalMesh(Asset->RobotMesh);
	ClaimHost->AddInstanceComponent(AuthoredMesh);
	USceneComponent* ClaimRoot = NewObject<USceneComponent>(ClaimHost, TEXT("DefaultSceneRoot"));
	ClaimHost->SetRootComponent(ClaimRoot);
	ClaimRoot->RegisterComponent();
	AuthoredMesh->SetupAttachment(ClaimRoot);
	AuthoredMesh->RegisterComponent();
	const FTransform AuthoredRelativeTransform = AuthoredMesh->GetRelativeTransform();

	Rig.Component->bClaimOwnerMesh = true;
	Rig.Component->OwnerMesh = AuthoredMesh;
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}
	TickPolicyWorld(Rig.World(), 0.010f);
	TestNull(TEXT("claim detaches the floating mesh from its kinematic root"),
		AuthoredMesh->GetAttachParent());
	TestTrue(TEXT("claim drives the floating mesh with physics"),
		AuthoredMesh->IsSimulatingPhysics());

	Rig.Component->StopPolicy();
	TestTrue(TEXT("release re-attaches the mesh to its original parent"),
		AuthoredMesh->GetAttachParent() == ClaimRoot);
	TestFalse(TEXT("release restores the authored drive state"),
		AuthoredMesh->IsSimulatingPhysics());
	TestTrue(TEXT("release restores the authored relative transform"),
		AuthoredMesh->GetRelativeTransform().Equals(AuthoredRelativeTransform, 1.0e-3f));

	TestTrue(TEXT("the restored actor can be claimed again"), Rig.Component->StartPolicy());
	Rig.Component->StopPolicy();
	AddInfo(TEXT("[VERIFY] AC_UE_REVIEW_032: release restores attachment, drive state and transform so the authored actor stays reusable"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentHostFaultFallbackRecoveryTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_016.HostFaultFallbackStopsOwnedRobotAndRestarts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentHostFaultFallbackRecoveryTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset)
	{
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}

	AActor* PhysicsProbe = SpawnDynamicProbe(*Rig.World(), FVector(500.0, 0.0, 1000.0));
	TestNotNull(TEXT("unrelated Chaos probe was created"), PhysicsProbe);
	if (!PhysicsProbe)
	{
		Rig.Component->StopPolicy();
		return false;
	}
	UStaticMeshComponent* PhysicsProbeMesh = PhysicsProbe->FindComponentByClass<UStaticMeshComponent>();
	TestNotNull(TEXT("unrelated Chaos probe simulates"), PhysicsProbeMesh);
	if (!PhysicsProbeMesh || !PhysicsProbeMesh->IsSimulatingPhysics())
	{
		Rig.Component->StopPolicy();
		return false;
	}

	UUERLPolicyComponentTestEventRecorder* Recorder = NewObject<UUERLPolicyComponentTestEventRecorder>();
	Recorder->FaultFallbackComponent = Rig.Component;
	BindRecorderEvent(
		Rig.Component->OnPolicyFault,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnFaultStopPolicy));
	TickPolicyWorld(Rig.World(), 0.010f);
	const double ProbeZBeforeFallback = PhysicsProbe->GetActorLocation().Z;

	// An authored host can choose to tear down a runtime-owned robot on fault.
	Rig.Ground->Destroy();
	TestFalse(TEXT("missing reset ground raises a policy fault"), Rig.Component->ResetToReferencePose());
	TestEqual(TEXT("host fallback observes exactly one policy fault"), Recorder->FaultCount, 1);
	TestTrue(TEXT("fault reason identifies the missing ground"), Recorder->LastFaultReason.Contains(TEXT("ground")));
	TestFalse(TEXT("host fallback stops inference"), Rig.Component->IsRunning());
	FTransform RemovedRobot;
	TestFalse(TEXT("StopPolicy releases the spawned robot owned by this component"),
		Rig.Component->GetRobotTransform(RemovedRobot));
	TestFalse(TEXT("host fallback does not pause the game world"), Rig.World()->IsPaused());

	TickPolicyWorld(Rig.World(), 0.020f);
	TestTrue(TEXT("unrelated Chaos simulation continues after the fallback"),
		PhysicsProbeMesh->IsSimulatingPhysics()
		&& PhysicsProbe->GetActorLocation().Z < ProbeZBeforeFallback - 0.05);

	Rig.Ground = SpawnGround(*Rig.World());
	TestNotNull(TEXT("host restored the missing static ground"), Rig.Ground);
	if (!Rig.Ground)
	{
		return false;
	}
	TestTrue(TEXT("host restart succeeds after restoring the cause and re-latching commands"),
		StartWithZeroCommand(*this, Rig.Component));
	FTransform RestartedRobot;
	TestTrue(TEXT("restart creates a live robot at the target ground"),
		Rig.Component->GetRobotTransform(RestartedRobot));
	TickPolicyWorld(Rig.World(), 0.010f);
	TestTrue(TEXT("restart consumes a fresh bootstrap control window"),
		Rig.Component->GetLastControlTiming().ObservationDtSeconds > 0.0);

	Rig.Component->StopPolicy();
	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_COMPONENT_016: an OnPolicyFault host fallback tears down its owned Robot, "
		"leaves unrelated Chaos simulation live, and restarts after ground/commands recover"));
	return true;
}

#endif
