#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TargetPoint.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PreviewScene.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/StringConv.h"
#include "UObject/UnrealType.h"
#include "UERLPolicyArtifactAsset.h"
#include "UERLPolicyComponent.h"
#include "UERLPolicyComponentTestEvents.h"
#include "UERLPolicyNetwork.h"
#include "UERLPolicyPlanRuntime.h"
#include "UERLPolicyTraceRecorder.h"

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
		GET_MEMBER_NAME_CHECKED(UUERLPolicyComponent, OnControlStepCompleted),
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentSpawnedResetIgnoresHostCollisionTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_017.SpawnedPoseResetIgnoresHostWorldStaticCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentSpawnedResetIgnoresHostCollisionTest::RunTest(const FString& Parameters)
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

	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	TestNotNull(TEXT("host collision shell mesh loads"), Cube);
	if (!Cube)
	{
		return false;
	}
	UStaticMeshComponent* OwnerShell = NewObject<UStaticMeshComponent>(Rig.Host);
	Rig.Host->AddInstanceComponent(OwnerShell);
	OwnerShell->SetStaticMesh(Cube);
	OwnerShell->SetMobility(EComponentMobility::Static);
	OwnerShell->SetCollisionObjectType(ECC_WorldStatic);
	OwnerShell->SetCollisionResponseToAllChannels(ECR_Block);
	OwnerShell->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	OwnerShell->SetWorldLocation(FVector(0.0, 0.0, 5.0));
	OwnerShell->SetWorldScale3D(FVector(1.0, 1.0, 0.2));
	OwnerShell->RegisterComponent();

	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}
	TickPolicyWorld(Rig.World(), 0.010f);

	TestTrue(TEXT("spawned robot resets with an overlapping host WorldStatic shell"),
		Rig.Component->ResetToReferencePose());
	FTransform ResetPose;
	TestTrue(TEXT("spawned robot transform remains available after reset"),
		Rig.Component->GetRobotTransform(ResetPose));
	TestTrue(TEXT("reset selects the ground beneath the host instead of its collision shell"),
		FMath::Abs(ResetPose.GetLocation().Z - Rig.Host->GetActorLocation().Z) < 2.0);

	Rig.Component->StopPolicy();
	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_COMPONENT_017: spawned pose reset ignores its host Owner's WorldStatic collision and uses the ground below"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentStaleCommandHostFallbackTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_018.HostStopsAndRestartsOnStaleCommand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentStaleCommandHostFallbackTest::RunTest(const FString& Parameters)
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
	Recorder->StaleFallbackComponent = Rig.Component;
	BindRecorderEvent(
		Rig.Component->OnCommandStale,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnStaleStopPolicy));
	BindRecorderEvent(
		Rig.Component->OnPolicyFault,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnFault));

	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("command below the staleness threshold keeps running"), Recorder->StaleCount, 0);
	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("host receives one stale-command event"), Recorder->StaleCount, 1);
	TestEqual(TEXT("stale response identifies the velocity channel"), Recorder->LastChannel, FName(TEXT("velocity")));
	TestFalse(TEXT("host StopPolicy fallback stops inference"), Rig.Component->IsRunning());
	TestEqual(TEXT("intentional stale fallback does not create a policy fault"), Recorder->FaultCount, 0);

	for (const FUERLPolicyCommandChannelInfo& Channel : Rig.Component->GetRequiredCommandChannels())
	{
		TArray<float> Zeros;
		Zeros.SetNumZeroed(Channel.Width);
		TestTrue(TEXT("host re-latches each required channel before restart"),
			Rig.Component->SetCommand(Channel.Name, Zeros));
	}
	TestTrue(TEXT("host explicitly restarts after its stale-command fallback"), Rig.Component->StartPolicy());
	TickPolicyWorld(Rig.World(), 0.010f);
	TestTrue(TEXT("restarted policy remains active before commands go stale again"), Rig.Component->IsRunning());
	TestEqual(TEXT("re-latched commands rearm the stale event"), Recorder->StaleCount, 1);
	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("stale host fallback triggers again after the re-latched command ages"), Recorder->StaleCount, 2);
	TestFalse(TEXT("second stale event also stops inference"), Rig.Component->IsRunning());

	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_COMPONENT_018: a host StopPolicy stale-command fallback re-latches commands and explicitly restarts"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentControlFrameDiagnosticsTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_019.ControlFrameSnapshotAlignsInputsActionsAndClocks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentControlFrameDiagnosticsTest::RunTest(const FString& Parameters)
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
	FUERLPolicyArtifact ParsedArtifact;
	FString OracleError;
	if (!Asset->LoadArtifact(ParsedArtifact, OracleError))
	{
		AddError(FString::Printf(TEXT("load snapshot replay artifact: %s"), *OracleError));
		return false;
	}
	TMap<FName, int32> AvailableCommandWidths;
	for (const FUERLPolicyCommandChannelInfo& Channel : Rig.Component->GetRequiredCommandChannels())
	{
		AvailableCommandWidths.Add(Channel.Name, Channel.Width);
	}
	const TArray<float> FirstCommand = {0.4f, 0.0f, 0.0f};
	TestTrue(TEXT("first training-distribution command is latched before its frame"),
		Rig.Component->SetCommand(TEXT("velocity"), FirstCommand));

	UUERLPolicyComponentTestEventRecorder* Recorder = NewObject<UUERLPolicyComponentTestEventRecorder>();
	BindRecorderEvent(
		Rig.Component->OnControlStepCompleted,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnControlFrameCompleted));
	TickPolicyWorld(Rig.World(), 0.005f);

	TestEqual(TEXT("first completed physics control step emits one diagnostic frame"), Recorder->ControlFrameCount, 1);
	if (!Recorder->ControlFrames.IsValidIndex(0))
	{
		AddError(TEXT("the first control-frame callback did not provide a snapshot"));
		return false;
	}
	const FUERLPolicyControlFrameSnapshot FirstFrame = Recorder->ControlFrames[0];
	TestTrue(TEXT("first decision snapshot is explicitly marked bootstrap"), FirstFrame.bBootstrap);
	TArray<FUERLFieldDescriptor> AvailableStateFields;
	for (const FUERLPolicyStateFieldSample& Field : FirstFrame.RawStateFields)
	{
		FUERLFieldDescriptor& Descriptor = AvailableStateFields.AddDefaulted_GetRef();
		Descriptor.Name = Field.Name;
		Descriptor.Shape.Add(Field.Width);
		Descriptor.Width = Field.Width;
	}
	FUERLPlanRuntime ObservationOracle;
	if (!ObservationOracle.Compile(
		ParsedArtifact.ObservationPlan(), AvailableStateFields, AvailableCommandWidths, OracleError))
	{
		AddError(FString::Printf(TEXT("compile snapshot observation replay: %s"), *OracleError));
		return false;
	}
	FUERLPolicyNetwork ActionOracle;
	if (!ActionOracle.Build(ParsedArtifact, OracleError))
	{
		AddError(FString::Printf(TEXT("build snapshot action replay: %s"), *OracleError));
		return false;
	}
	auto VerifySnapshotReplay = [this, &ObservationOracle, &ActionOracle](
		const FUERLPolicyControlFrameSnapshot& Frame,
		const TCHAR* Label) -> bool
	{
		auto CheckFinite = [this, Label](const TCHAR* Name, const TArray<float>& Values) -> bool
		{
			for (int32 Index = 0; Index < Values.Num(); ++Index)
			{
				if (!FMath::IsFinite(Values[Index]))
				{
					AddError(FString::Printf(TEXT("%s %s[%d] is not finite"), Label, Name, Index));
					return false;
				}
			}
			return true;
		};
		if (!CheckFinite(TEXT("raw state"), Frame.RawState)
			|| !CheckFinite(TEXT("observation"), Frame.Observation)
			|| !CheckFinite(TEXT("previous action"), Frame.PreviousAction)
			|| !CheckFinite(TEXT("action"), Frame.Action)
			|| !CheckFinite(TEXT("actuator targets"), Frame.ActuatorTargets))
		{
			return false;
		}
		FUERLPlanInputs Inputs;
		Inputs.RawState = Frame.RawState;
		Inputs.PreviousAction = Frame.PreviousAction;
		Inputs.ControlFrameDtSeconds = static_cast<float>(Frame.ObservationDtSeconds);
		for (const FUERLPolicyCommandSample& Command : Frame.Commands)
		{
			if (!FMath::IsFinite(Command.AgeSeconds) || Command.AgeSeconds < 0.0)
			{
				AddError(FString::Printf(TEXT("%s command '%s' has an invalid age"),
					Label, *Command.Channel.ToString()));
				return false;
			}
			if (!CheckFinite(*Command.Channel.ToString(), Command.Values))
			{
				return false;
			}
			Inputs.Commands.Add(Command.Channel, Command.Values);
		}
		FString ReplayError;
		TArray<float> ReplayedObservation;
		if (!ObservationOracle.Execute(Inputs, ReplayedObservation, ReplayError))
		{
			AddError(FString::Printf(TEXT("%s observation replay failed: %s"), Label, *ReplayError));
			return false;
		}
		bool bMatches = true;
		if (ReplayedObservation.Num() != Frame.Observation.Num())
		{
			AddError(FString::Printf(TEXT("%s replayed observation width %d != snapshot width %d"),
				Label, ReplayedObservation.Num(), Frame.Observation.Num()));
			return false;
		}
		if (!CheckFinite(TEXT("replayed observation"), ReplayedObservation))
		{
			return false;
		}
		for (int32 Index = 0; Index < ReplayedObservation.Num(); ++Index)
		{
			if (FMath::Abs(ReplayedObservation[Index] - Frame.Observation[Index]) > 2.0e-5f)
			{
				AddError(FString::Printf(TEXT("%s observation[%d] does not match its captured state/history/command/dt"),
					Label, Index));
				bMatches = false;
			}
		}

		TArray<float> ReplayedAction;
		if (!ActionOracle.Evaluate(Frame.Observation, ReplayedAction, ReplayError))
		{
			AddError(FString::Printf(TEXT("%s action replay failed: %s"), Label, *ReplayError));
			return false;
		}
		if (ReplayedAction.Num() != Frame.Action.Num())
		{
			AddError(FString::Printf(TEXT("%s replayed action width %d != snapshot width %d"),
				Label, ReplayedAction.Num(), Frame.Action.Num()));
			return false;
		}
		if (!CheckFinite(TEXT("replayed action"), ReplayedAction))
		{
			return false;
		}
		for (int32 Index = 0; Index < ReplayedAction.Num(); ++Index)
		{
			if (FMath::Abs(ReplayedAction[Index] - Frame.Action[Index]) > 2.0e-5f)
			{
				AddError(FString::Printf(TEXT("%s action[%d] does not match inference on its captured observation"),
					Label, Index));
				bMatches = false;
			}
		}
		return bMatches;
	};
	if (!VerifySnapshotReplay(FirstFrame, TEXT("first frame")))
	{
		return false;
	}
	TestEqual(TEXT("first diagnostic frame has sequence one"), FirstFrame.Sequence, int64(1));
	TestTrue(TEXT("diagnostic frame identifies a completed solver frame"), FirstFrame.SolverFrame >= 0);
	TestTrue(TEXT("diagnostic frame includes the completed solver time"),
		FMath::IsFinite(FirstFrame.SolverTimeSeconds) && FirstFrame.SolverTimeSeconds > 0.0);
	TestTrue(TEXT("game time is aligned with the completed control frame"),
		FMath::IsFinite(FirstFrame.GameElapsedSeconds) && FirstFrame.GameElapsedSeconds > 0.0);
	TestTrue(TEXT("physics time is aligned with the completed control frame"),
		FMath::IsFinite(FirstFrame.PhysicsElapsedSeconds) && FirstFrame.PhysicsElapsedSeconds > 0.0);
	TestTrue(TEXT("bootstrap observation uses the artifact DtMin"),
		FMath::Abs(FirstFrame.ObservationDtSeconds - 0.005) < 2.0e-3);
	TestTrue(TEXT("solver denominator reports the completed substep dt"),
		FMath::Abs(FirstFrame.LastSolverStepSeconds - 0.005) < 1.0e-3);
	TestTrue(TEXT("first frame begins with cleared previous-action history"),
		!FirstFrame.PreviousAction.ContainsByPredicate([](float Value) { return FMath::Abs(Value) > 1.0e-6f; }));
	const FUERLPolicyCommandSample* FirstVelocity = FirstFrame.Commands.FindByPredicate(
		[](const FUERLPolicyCommandSample& Candidate)
		{
			return Candidate.Channel == TEXT("velocity");
		});
	TestNotNull(TEXT("first frame records its latched velocity command"), FirstVelocity);
	if (FirstVelocity)
	{
		TestTrue(TEXT("first frame records the exact forward command"), FirstVelocity->Values == FirstCommand);
	}
	TestTrue(TEXT("first action has the artifact's output width"), FirstFrame.Action.Num() == ActionOracle.OutputWidth());

	const TArray<float> SecondCommand = {0.45f, 0.0f, 0.25f};
	TestTrue(TEXT("second training-distribution command is latched before its frame"),
		Rig.Component->SetCommand(TEXT("velocity"), SecondCommand));
	TickPolicyWorld(Rig.World(), 0.005f);
	TestEqual(TEXT("second completed window emits a second frame"), Recorder->ControlFrameCount, 2);
	if (!Recorder->ControlFrames.IsValidIndex(1))
	{
		AddError(TEXT("the second control-frame callback did not provide a snapshot"));
		return false;
	}
	const FUERLPolicyControlFrameSnapshot SecondFrame = Recorder->ControlFrames[1];
	TestFalse(TEXT("second decision snapshot is not a bootstrap"), SecondFrame.bBootstrap);
	if (!VerifySnapshotReplay(SecondFrame, TEXT("second frame")))
	{
		return false;
	}
	TestEqual(TEXT("diagnostic sequence advances once per policy step"), SecondFrame.Sequence, int64(2));
	TestTrue(TEXT("second frame consumes the first frame raw action as previous_action"),
		SecondFrame.PreviousAction == FirstFrame.Action);
	const FUERLPolicyCommandSample* SecondVelocity = SecondFrame.Commands.FindByPredicate(
		[](const FUERLPolicyCommandSample& Candidate)
		{
			return Candidate.Channel == TEXT("velocity");
		});
	TestNotNull(TEXT("second frame records its own latched velocity command"), SecondVelocity);
	if (SecondVelocity && FirstVelocity)
	{
		TestTrue(TEXT("second frame does not repeat the previous frame's command"),
			SecondVelocity->Values == SecondCommand && SecondVelocity->Values != FirstVelocity->Values);
	}
	TestTrue(TEXT("second action is captured at the second command/history boundary"),
		SecondFrame.Action.Num() == FirstFrame.Action.Num());
	TestTrue(TEXT("second five millisecond control window keeps its own timing"),
		FMath::Abs(SecondFrame.ObservationDtSeconds - 0.005) < 2.0e-3
		&& FMath::Abs(SecondFrame.GameElapsedSeconds - 0.005) < 2.0e-3
		&& FMath::Abs(SecondFrame.PhysicsElapsedSeconds - 0.005) < 2.0e-3
		&& FMath::Abs((SecondFrame.SolverTimeSeconds - FirstFrame.SolverTimeSeconds) - 0.005) < 2.0e-3
		&& SecondFrame.SolverFrame > FirstFrame.SolverFrame);

	const TArray<float> ThirdCommand = {0.5f, 0.0f, -0.25f};
	TestTrue(TEXT("third training-distribution command is latched before its frame"),
		Rig.Component->SetCommand(TEXT("velocity"), ThirdCommand));
	TickPolicyWorld(Rig.World(), 0.010f);
	TestEqual(TEXT("two completed solver substeps emit one third policy frame"), Recorder->ControlFrameCount, 3);
	if (!Recorder->ControlFrames.IsValidIndex(2))
	{
		AddError(TEXT("the third control-frame callback did not provide a snapshot"));
		return false;
	}
	const FUERLPolicyControlFrameSnapshot ThirdFrame = Recorder->ControlFrames[2];
	TestFalse(TEXT("third decision snapshot remains in the current episode"), ThirdFrame.bBootstrap);
	if (!VerifySnapshotReplay(ThirdFrame, TEXT("third frame")))
	{
		return false;
	}
	TestEqual(TEXT("diagnostic sequence remains monotonic across variable windows"), ThirdFrame.Sequence, int64(3));
	TestTrue(TEXT("ten millisecond observation tracks its accumulated physical window"),
		FMath::Abs(ThirdFrame.PhysicsElapsedSeconds - 0.010) < 2.0e-3
		&& FMath::Abs(ThirdFrame.GameElapsedSeconds - 0.010) < 2.0e-3
		&& FMath::Abs(ThirdFrame.ObservationDtSeconds - 0.010) < 2.0e-3
		&& FMath::Abs((ThirdFrame.SolverTimeSeconds - SecondFrame.SolverTimeSeconds) - 0.010) < 2.0e-3
		&& ThirdFrame.SolverFrame > SecondFrame.SolverFrame
		&& FMath::Abs(ThirdFrame.LastSolverStepSeconds - 0.005) < 1.0e-3);
	TestTrue(TEXT("third frame consumes the second frame raw action as previous_action"),
		ThirdFrame.PreviousAction == SecondFrame.Action);
	const FUERLPolicyCommandSample* ThirdVelocity = ThirdFrame.Commands.FindByPredicate(
		[](const FUERLPolicyCommandSample& Candidate)
		{
			return Candidate.Channel == TEXT("velocity");
		});
	TestNotNull(TEXT("third frame records its own latched velocity command"), ThirdVelocity);
	if (ThirdVelocity)
	{
		TestTrue(TEXT("third frame records the changed yaw command"), ThirdVelocity->Values == ThirdCommand);
	}

	int32 RawStateWidth = 0;
	for (const FUERLPolicyStateFieldSample& Field : FirstFrame.RawStateFields)
	{
		TestTrue(TEXT("raw state field metadata is named and has a positive width"),
			!Field.Name.IsNone() && Field.Width > 0);
		RawStateWidth += Field.Width;
	}
	TestEqual(TEXT("raw state names and widths cover the packed raw state"), RawStateWidth, FirstFrame.RawState.Num());
	TestTrue(TEXT("network input, previous action, policy action, and actuator targets are captured"),
		FirstFrame.Observation.Num() > 0
		&& FirstFrame.PreviousAction.Num() == FirstFrame.Action.Num()
		&& FirstFrame.Action.Num() > 0
		&& FirstFrame.ActuatorTargets.Num() > 0);
	TestEqual(TEXT("snapshot command count matches the artifact contract"),
		FirstFrame.Commands.Num(), Rig.Component->GetRequiredCommandChannels().Num());
	for (const FUERLPolicyCommandChannelInfo& Required : Rig.Component->GetRequiredCommandChannels())
	{
		const FUERLPolicyCommandSample* Command = FirstFrame.Commands.FindByPredicate(
			[&Required](const FUERLPolicyCommandSample& Candidate)
			{
				return Candidate.Channel == Required.Name;
			});
		TestNotNull(TEXT("snapshot preserves each required command name"), Command);
		if (!Command)
		{
			continue;
		}
		TestEqual(TEXT("snapshot command width matches the artifact contract"),
			Command->Values.Num(), Required.Width);
		TestTrue(TEXT("latched command age is finite and non-negative"),
			FMath::IsFinite(Command->AgeSeconds) && Command->AgeSeconds >= 0.0);
	}

	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_COMPONENT_019: each event snapshot aligns commands, raw state, network input, previous action, action, actuator targets, and solver timing"));
	Rig.Component->StopPolicy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentControlFrameCallbackLifecycleTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_020.ControlFrameCallbacksStopOrResetWithoutReusingTheOldWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentControlFrameCallbackLifecycleTest::RunTest(const FString& Parameters)
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

	UUERLPolicyComponentTestEventRecorder* Recorder = NewObject<UUERLPolicyComponentTestEventRecorder>();
	Recorder->ControlFrameCallbackComponent = Rig.Component;
	Recorder->ControlFrameCallbackMode = EUERLPolicyTestFrameCallbackMode::Stop;
	BindRecorderEvent(
		Rig.Component->OnControlStepCompleted,
		Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnControlFrameCompleted));

	TickPolicyWorld(Rig.World(), 0.005f);
	TestEqual(TEXT("StopPolicy from the frame callback observes one completed frame"), Recorder->ControlFrameCount, 1);
	TestTrue(TEXT("StopPolicy from the event callback stops inference"), Recorder->bControlFrameCallbackSucceeded);
	TestFalse(TEXT("no policy window remains active after callback stop"), Rig.Component->IsRunning());

	Recorder->ControlFrameCallbackMode = EUERLPolicyTestFrameCallbackMode::RecordOnly;
	for (const FUERLPolicyCommandChannelInfo& Channel : Rig.Component->GetRequiredCommandChannels())
	{
		TArray<float> Zeros;
		Zeros.SetNumZeroed(Channel.Width);
		TestTrue(TEXT("restart re-latches each required command channel"), Rig.Component->SetCommand(Channel.Name, Zeros));
	}
	TestTrue(TEXT("explicit restart succeeds after callback stop"), Rig.Component->StartPolicy());
	TickPolicyWorld(Rig.World(), 0.005f);
	TestEqual(TEXT("restarted run emits its own first completed frame"), Recorder->ControlFrameCount, 2);
	if (!Recorder->ControlFrames.IsValidIndex(1))
	{
		AddError(TEXT("the restarted control-frame callback did not provide a snapshot"));
		return false;
	}
	TestEqual(TEXT("component sequence remains monotonic across restart"), Recorder->ControlFrames[1].Sequence, int64(2));
	TestTrue(TEXT("explicit restart is marked as a bootstrap boundary"), Recorder->ControlFrames[1].bBootstrap);
	TestTrue(TEXT("restart starts with an artifact bootstrap-sized observation window"),
		FMath::Abs(Recorder->ControlFrames[1].ObservationDtSeconds - 0.005) < 2.0e-3);

	Recorder->ControlFrameCallbackMode = EUERLPolicyTestFrameCallbackMode::SoftReset;
	TickPolicyWorld(Rig.World(), 0.005f);
	TestEqual(TEXT("SoftReset callback receives its completed frame"), Recorder->ControlFrameCount, 3);
	TestTrue(TEXT("SoftReset from the event callback succeeds"), Recorder->bControlFrameCallbackSucceeded);
	TestTrue(TEXT("SoftReset from the event callback keeps policy armed"), Rig.Component->IsRunning());
	TestEqual(TEXT("callback reset clears the completed-step timing before returning"),
		Rig.Component->GetLastControlTiming().ObservationDtSeconds, 0.0);

	Recorder->ControlFrameCallbackMode = EUERLPolicyTestFrameCallbackMode::RecordOnly;
	TickPolicyWorld(Rig.World(), 0.005f);
	TestEqual(TEXT("post-reset control starts only a new completed frame"), Recorder->ControlFrameCount, 4);
	if (!Recorder->ControlFrames.IsValidIndex(3))
	{
		AddError(TEXT("the post-reset control-frame callback did not provide a snapshot"));
		return false;
	}
	const FUERLPolicyControlFrameSnapshot& ResetFrame = Recorder->ControlFrames[3];
	TestEqual(TEXT("post-reset frame keeps its monotonic component sequence"), ResetFrame.Sequence, int64(4));
	TestTrue(TEXT("SoftReset is marked as a new bootstrap boundary"), ResetFrame.bBootstrap);
	TestTrue(TEXT("post-reset frame uses a fresh bootstrap-sized dt"),
		FMath::Abs(ResetFrame.ObservationDtSeconds - 0.005) < 2.0e-3);
	TestTrue(TEXT("post-reset frame clears the prior action history"),
		!ResetFrame.PreviousAction.ContainsByPredicate([](float Value) { return FMath::Abs(Value) > 1.0e-6f; }));

	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_COMPONENT_020: StopPolicy, restart, and SoftReset from a frame callback do not reuse the interrupted control window"));
	Rig.Component->StopPolicy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyTraceRecorderYamlTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_021.ExplicitTraceWritesYamlAndResetBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyTraceRecorderYamlTest::RunTest(const FString& Parameters)
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

	UUERLPolicyTraceRecorder* Trace = NewObject<UUERLPolicyTraceRecorder>(Rig.Host);
	Rig.Host->AddInstanceComponent(Trace);
	Trace->PolicyComponent = Rig.Component;
	Trace->RegisterComponent();
	UUERLPolicyTraceRecorder* ConcurrentTrace = NewObject<UUERLPolicyTraceRecorder>(Rig.Host);
	Rig.Host->AddInstanceComponent(ConcurrentTrace);
	ConcurrentTrace->PolicyComponent = Rig.Component;
	ConcurrentTrace->RegisterComponent();
	const FString FileName = FString::Printf(
		TEXT("AC021_%s.yaml"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TestTrue(TEXT("explicit trace starts under Saved without replacing an existing file"), Trace->StartTrace(FileName, 7));
	TestTrue(TEXT("a concurrent writer can stage the same path without replacing it"),
		ConcurrentTrace->StartTrace(FileName, 8));
	if (!Trace->IsRecording())
	{
		AddError(FString::Printf(TEXT("StartTrace error: %s"), *Trace->GetLastError()));
		Rig.Component->StopPolicy();
		return false;
	}

	TickPolicyWorld(Rig.World(), 0.005f);
	TestTrue(TEXT("host reset clears the policy and arms a new bootstrap"), Rig.Component->SoftReset());
	TestTrue(TEXT("host records the matching episode boundary"), Trace->MarkEpisodeBoundary(1, TEXT("主机软重置")));
	Rig.Component->OnControlStepOverrun.Broadcast(0.04f, 0.04f, 0.035f);
	Rig.Component->OnCommandStale.Broadcast(FName(TEXT("velocity")), 0.04f);
	TickPolicyWorld(Rig.World(), 0.005f);
	TestTrue(TEXT("unannounced host restart arms a bootstrap frame"), Rig.Component->SoftReset());
	TickPolicyWorld(Rig.World(), 0.005f);
	TestTrue(TEXT("trace stops and flushes successfully"), Trace->StopTrace());
	TestFalse(TEXT("the concurrent writer cannot replace the completed trace"), ConcurrentTrace->StopTrace());
	TestFalse(TEXT("a failed finalization does not later report success"), ConcurrentTrace->StopTrace());

	const FString TracePath = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("UERLPolicyTraces"), FileName);
	FString Contents;
	TestTrue(TEXT("trace output exists"), FFileHelper::LoadFileToString(Contents, *TracePath));
	TArray<uint8> TraceBytes;
	TestTrue(TEXT("trace output is readable as raw bytes"), FFileHelper::LoadFileToArray(TraceBytes, *TracePath));
	TestTrue(TEXT("trace output does not have a UTF-8 BOM"),
		TraceBytes.Num() < 3 || TraceBytes[0] != 0xef || TraceBytes[1] != 0xbb || TraceBytes[2] != 0xbf);
	const FTCHARToUTF8 Utf8BoundaryReason(TEXT("主机软重置"));
	bool bContainsUtf8BoundaryReason = false;
	for (int32 ByteIndex = 0; ByteIndex + Utf8BoundaryReason.Length() <= TraceBytes.Num(); ++ByteIndex)
	{
		if (FMemory::Memcmp(
			TraceBytes.GetData() + ByteIndex,
			Utf8BoundaryReason.Get(),
			Utf8BoundaryReason.Length()) == 0)
		{
			bContainsUtf8BoundaryReason = true;
			break;
		}
	}
	TestTrue(TEXT("boundary reason is serialized as raw UTF-8"), bContainsUtf8BoundaryReason);
	TestTrue(TEXT("UTF-8 trace round-trips a non-ASCII boundary reason"),
		Contents.Contains(TEXT("reason: \"主机软重置\"")));
	TestTrue(TEXT("trace declares schema version and UE identity"),
		Contents.Contains(TEXT("schema_version: 1"))
		&& Contents.Contains(TEXT("side: ue"))
		&& Contents.Contains(Asset->Summary.TaskId.ToString())
		&& Contents.Contains(TEXT("policy_onnx_sha1: \""))
		&& Contents.Contains(TEXT("seed: 7")));
	TestTrue(TEXT("trace distinguishes initial bootstrap and host reset samples"),
		Contents.Contains(TEXT("phase: bootstrap_input"))
		&& Contents.Contains(TEXT("kind: boundary"))
		&& Contents.Contains(TEXT("episode_index: 1"))
		&& Contents.Contains(TEXT("phase: post_reset_input"))
		&& Contents.Contains(TEXT("event: control_step_overrun"))
		&& Contents.Contains(TEXT("event: command_stale"))
		&& Contents.Contains(TEXT("event: unmarked_bootstrap_boundary"))
		&& Contents.Contains(TEXT("episode_step: 0"))
		&& Contents.Contains(TEXT("episode_step: 1"))
		&& Contents.Contains(TEXT("sequence: 2")));
	TestTrue(TEXT("unmarked restart event is linked to its exact bootstrap decision"),
		Contents.Contains(TEXT("event: unmarked_bootstrap_boundary\n    sequence: 3\n    episode_index: 1\n    episode_step: 1")));
	TestTrue(TEXT("trace contains aligned clock and policy input/output fields"),
		Contents.Contains(TEXT("solver_time_s:"))
		&& Contents.Contains(TEXT("observation_dt_s:"))
		&& Contents.Contains(TEXT("raw_state_fields:"))
		&& Contents.Contains(TEXT("previous_action:"))
		&& Contents.Contains(TEXT("actuator_targets:")));
	UUERLPolicyTraceRecorder* ContinuationTrace = NewObject<UUERLPolicyTraceRecorder>(Rig.Host);
	Rig.Host->AddInstanceComponent(ContinuationTrace);
	ContinuationTrace->PolicyComponent = Rig.Component;
	ContinuationTrace->RegisterComponent();
	const FString ContinuationFile = FString::Printf(
		TEXT("AC021_continuation_%s.yaml"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TestTrue(TEXT("a fresh trace can start on a component with an existing sequence"),
		ContinuationTrace->StartTrace(ContinuationFile, 7));
	Rig.Component->OnControlStepOverrun.Broadcast(0.04f, 0.04f, 0.035f);
	TickPolicyWorld(Rig.World(), 0.005f);
	TestTrue(TEXT("continuation trace flushes"), ContinuationTrace->StopTrace());
	const FString ContinuationPath = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("UERLPolicyTraces"), ContinuationFile);
	FString ContinuationContents;
	TestTrue(TEXT("continuation trace output exists"),
		FFileHelper::LoadFileToString(ContinuationContents, *ContinuationPath));
	TestTrue(TEXT("event sequence inherits the component's monotonic sequence baseline"),
		ContinuationContents.Contains(TEXT("event: control_step_overrun\n    sequence: 4"))
		&& ContinuationContents.Contains(TEXT("kind: frame\n    sequence: 4")));
	auto VerifyCallbackBoundaryOrdering = [this, &Rig](bool bTraceFirst, const FString& Label)
	{
		UUERLPolicyTraceRecorder* OrderedTrace = NewObject<UUERLPolicyTraceRecorder>(Rig.Host);
		Rig.Host->AddInstanceComponent(OrderedTrace);
		OrderedTrace->PolicyComponent = Rig.Component;
		OrderedTrace->RegisterComponent();
		UUERLPolicyComponentTestEventRecorder* HostCallback =
			NewObject<UUERLPolicyComponentTestEventRecorder>(Rig.Host);
		HostCallback->ControlFrameCallbackComponent = Rig.Component;
		HostCallback->ControlFrameTraceRecorder = OrderedTrace;
		HostCallback->ControlFrameCallbackMode =
			EUERLPolicyTestFrameCallbackMode::SoftResetAndMarkBoundary;
		HostCallback->ControlFrameEpisodeIndex = 1;
		HostCallback->ControlFrameBoundaryReason = Label;
		const FString OrderedFileName = FString::Printf(
			TEXT("AC021_order_%s_%s.yaml"),
			bTraceFirst ? TEXT("trace_first") : TEXT("host_first"),
			*FGuid::NewGuid().ToString(EGuidFormats::Digits));
		if (bTraceFirst)
		{
			this->TestTrue(*FString::Printf(TEXT("%s trace starts before the host callback"), *Label),
				OrderedTrace->StartTrace(OrderedFileName, 7));
			BindRecorderEvent(
				Rig.Component->OnControlStepCompleted,
				HostCallback,
				GET_FUNCTION_NAME_CHECKED(
					UUERLPolicyComponentTestEventRecorder, OnControlFrameCompleted));
		}
		else
		{
			BindRecorderEvent(
				Rig.Component->OnControlStepCompleted,
				HostCallback,
				GET_FUNCTION_NAME_CHECKED(
					UUERLPolicyComponentTestEventRecorder, OnControlFrameCompleted));
			this->TestTrue(*FString::Printf(TEXT("%s trace starts after the host callback"), *Label),
				OrderedTrace->StartTrace(OrderedFileName, 7));
		}
		for (int32 Tick = 0; Tick < 32 && !HostCallback->bControlFrameCallbackSucceeded; ++Tick)
		{
			TickPolicyWorld(Rig.World(), 0.005f);
		}
		this->TestTrue(*FString::Printf(TEXT("%s callback resets and marks the boundary"), *Label),
			HostCallback->bControlFrameCallbackSucceeded);
		const int64 BeforeBootstrapSequence = HostCallback->LastControlFrame.Sequence;
		for (int32 Tick = 0; Tick < 32
			&& Rig.Component->GetControlFrameSequence() <= BeforeBootstrapSequence; ++Tick)
		{
			TickPolicyWorld(Rig.World(), 0.005f);
		}
		this->TestTrue(*FString::Printf(TEXT("%s emits a later bootstrap frame"), *Label),
			Rig.Component->GetControlFrameSequence() > BeforeBootstrapSequence);
		const int64 BootstrapSequence = BeforeBootstrapSequence + 1;
		this->TestTrue(*FString::Printf(TEXT("%s trace stops"), *Label), OrderedTrace->StopTrace());
		const FString OrderedPath = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("UERLPolicyTraces"), OrderedFileName);
		FString OrderedContents;
		this->TestTrue(*FString::Printf(TEXT("%s trace output exists"), *Label),
			FFileHelper::LoadFileToString(OrderedContents, *OrderedPath));
		const FString OldFrame = FString::Printf(
			TEXT("kind: frame\n    sequence: %lld\n    episode_index: 0\n"),
			static_cast<long long>(BeforeBootstrapSequence));
		const FString Boundary = FString::Printf(
			TEXT("kind: boundary\n    sequence: %lld\n    episode_index: 1\n    reason: \"%s\""),
			static_cast<long long>(BootstrapSequence), *Label);
		const FString NewFrame = FString::Printf(
			TEXT("kind: frame\n    sequence: %lld\n    episode_index: 1\n    episode_step: 0\n    phase: post_reset_input"),
			static_cast<long long>(BootstrapSequence));
		this->TestTrue(*FString::Printf(TEXT("%s retains the callback frame in the old episode"), *Label),
			OrderedContents.Contains(OldFrame));
		this->TestTrue(*FString::Printf(TEXT("%s marks the next sequence as the new episode"), *Label),
			OrderedContents.Contains(Boundary) && OrderedContents.Contains(NewFrame));
		IFileManager::Get().Delete(*OrderedPath, false, true, true);
	};
	VerifyCallbackBoundaryOrdering(false, TEXT("host_before_trace"));
	VerifyCallbackBoundaryOrdering(true, TEXT("trace_before_host"));
	Rig.Component->StopPolicy();
	UUERLPolicyTraceRecorder* ExistingTrace = NewObject<UUERLPolicyTraceRecorder>(Rig.Host);
	Rig.Host->AddInstanceComponent(ExistingTrace);
	ExistingTrace->PolicyComponent = Rig.Component;
	ExistingTrace->RegisterComponent();
	TestFalse(TEXT("a finalized trace path is rejected on a later start"), ExistingTrace->StartTrace(FileName, 9));
	TestTrue(TEXT("the retained file still belongs to the original writer"), Contents.Contains(TEXT("seed: 7")));
	UUERLPolicyTraceRecorder* AbandonedTrace = NewObject<UUERLPolicyTraceRecorder>(Rig.Host);
	AbandonedTrace->PolicyComponent = Rig.Component;
	const FString AbandonedFile = FString::Printf(
		TEXT("AC021_incomplete_%s.yaml"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TestTrue(TEXT("a second trace can be staged for incomplete-stop coverage"),
		AbandonedTrace->StartTrace(AbandonedFile, 7));
	AbandonedTrace->EndPlay(EEndPlayReason::Destroyed);
	const FString AbandonedPath = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("UERLPolicyTraces"), AbandonedFile);
	FString AbandonedContents;
	TestTrue(TEXT("EndPlay still persists an abandoned trace for diagnosis"),
		FFileHelper::LoadFileToString(AbandonedContents, *AbandonedPath));
	TestTrue(TEXT("EndPlay labels the trace incomplete without an explicit StopTrace"),
		AbandonedContents.Contains(TEXT("status: incomplete")));
	IFileManager::Get().Delete(*AbandonedPath, false, true, true);
	IFileManager::Get().Delete(*TracePath, false, true);
	IFileManager::Get().Delete(*ContinuationPath, false, true);
	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_COMPONENT_021: opt-in UE YAML trace records aligned policy frames and explicit reset boundaries"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyTraceRecorderEmptyCommandsTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_022.EmptyCommandTraceSerializesAsSequence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyTraceRecorderEmptyCommandsTest::RunTest(const FString& Parameters)
{
	FString Error;
	UUERLPolicyArtifactAsset* Asset = MakeTransientAsset(Error);
	if (!Asset)
	{
		AddError(Error);
		return false;
	}
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset))
	{
		return false;
	}
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	TestEqual(TEXT("CartPole controller has no command channels"),
		Rig.Component->GetRequiredCommandChannels().Num(), 0);
	if (!StartWithZeroCommand(*this, Rig.Component))
	{
		return false;
	}
	UUERLPolicyTraceRecorder* Trace = NewObject<UUERLPolicyTraceRecorder>(Rig.Host);
	Rig.Host->AddInstanceComponent(Trace);
	Trace->PolicyComponent = Rig.Component;
	Trace->RegisterComponent();
	const FString FileName = FString::Printf(
		TEXT("AC022_empty_commands_%s.yaml"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	TestTrue(TEXT("CartPole trace starts"), Trace->StartTrace(FileName, 0));
	const int64 InitialSequence = Rig.Component->GetControlFrameSequence();
	for (int32 Tick = 0; Tick < 32
		&& Rig.Component->GetControlFrameSequence() <= InitialSequence; ++Tick)
	{
		TickPolicyWorld(Rig.World(), 0.005f);
	}
	TestTrue(TEXT("CartPole emits a policy frame"),
		Rig.Component->GetControlFrameSequence() > InitialSequence);
	TestTrue(TEXT("CartPole trace stops"), Trace->StopTrace());
	const FString TracePath = FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("UERLPolicyTraces"), FileName);
	FString Contents;
	TestTrue(TEXT("CartPole trace output exists"), FFileHelper::LoadFileToString(Contents, *TracePath));
	TestTrue(TEXT("no-command trace serializes commands as an empty YAML sequence"),
		Contents.Contains(TEXT("commands: []")));
	IFileManager::Get().Delete(*TracePath, false, true, true);
	Rig.Component->StopPolicy();
	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_COMPONENT_022: CartPole trace with no command channels writes commands: []"));
	return true;
}

#endif
