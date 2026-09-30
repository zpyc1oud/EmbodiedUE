#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "PreviewScene.h"

#include "Misc/Paths.h"
#include "UERLPolicyController.h"
#include "UERLSkeletalMeshRobotRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr TCHAR CartPoleRobotAsset[] = TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole");

	bool ResolveControllerCorpusPath(FString& OutPath, FString& OutError)
	{
		TArray<FString> Candidates;
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT(".."),
			TEXT("tests"),
			TEXT("parity"),
			TEXT("cases"),
			TEXT("controller"),
			TEXT("cartpole_controller.uerlpol2"))));
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(),
			TEXT("tests"),
			TEXT("parity"),
			TEXT("cases"),
			TEXT("controller"),
			TEXT("cartpole_controller.uerlpol2"))));

		for (const FString& Candidate : Candidates)
		{
			if (FPaths::FileExists(Candidate))
			{
				OutPath = Candidate;
				return true;
			}
		}

		OutError = TEXT("controller corpus artifact not found; tried:");
		for (const FString& Candidate : Candidates)
		{
			OutError += FString::Printf(TEXT("\n  %s"), *Candidate);
		}
		return false;
	}

	AActor* SpawnGround(UWorld& World)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube)
		{
			return nullptr;
		}
		AStaticMeshActor* Ground = World.SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), FVector(0.0, 0.0, -50.0), FRotator::ZeroRotator);
		if (!Ground)
		{
			return nullptr;
		}
		UStaticMeshComponent* Component = Ground->GetStaticMeshComponent();
		Component->SetStaticMesh(Cube);
		Component->SetMobility(EComponentMobility::Static);
		Component->SetCollisionObjectType(ECC_WorldStatic);
		Component->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetWorldScale3D(FVector(10.0, 10.0, 0.5));
		return Ground;
	}

	/**
	 * Structural proof that the caller cannot enumerate actuators/observations:
	 * FUERLPolicyControllerConfig has only ArtifactPath / AssetPath / placement.
	 * sizeof comparison against a type that *would* carry those lists catches
	 * accidental reintroduction of caller-enumerated fields.
	 */
	struct FForbiddenCallerEnumeratedConfig
	{
		FString ArtifactPath;
		FString AssetPath;
		FVector GroundOrigin = FVector::ZeroVector;
		FVector GroundNormal = FVector::UpVector;
		TArray<TWeakObjectPtr<AActor>> TerrainQueryActors;
		double InitialRootHeightMeters = 0.0;
		bool bClaimAuthoredActor = false;
		USkeletalMeshComponent* ClaimedMesh = nullptr;
		FTransform PlacementTransform = FTransform::Identity;
		bool bHasPlacementTransform = false;
		TArray<FUERLSkeletalMeshRuntimeActuator> Actuators;
		TArray<FUERLSkeletalMeshRuntimeObservation> Observations;
	};

	static_assert(
		sizeof(FUERLPolicyControllerConfig) < sizeof(FForbiddenCallerEnumeratedConfig),
		"FUERLPolicyControllerConfig must not grow caller-enumerated actuator/observation lists");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyControllerInitializeFromArtifactTest,
	"UERL.Integration.Policy.Controller.AC_UE_INT_POLICY_001.InitializeDerivesRuntimeFromArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyControllerInitializeFromArtifactTest::RunTest(const FString& Parameters)
{
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLPolicy"));
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLRobot"));

	FString ArtifactPath;
	FString Error;
	TestTrue(TEXT("resolve controller corpus artifact"), ResolveControllerCorpusPath(ArtifactPath, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = PreviewScene->GetWorld();
	TestNotNull(TEXT("controller test world created"), World);
	if (!World)
	{
		return false;
	}
	AActor* Ground = SpawnGround(*World);
	TestNotNull(TEXT("WorldStatic ground created"), Ground);
	if (!Ground)
	{
		return false;
	}

	// Arrange — caller supplies only artifact path + asset path (+ placement).
	FUERLPolicyControllerConfig Config;
	Config.ArtifactPath = ArtifactPath;
	Config.AssetPath = CartPoleRobotAsset;
	Config.GroundOrigin = FVector(0.0, 0.0, 0.0);
	Config.GroundNormal = FVector::UpVector;
	Config.TerrainQueryActors.Add(Ground);
	Config.InitialRootHeightMeters = 0.05;

	FUERLPolicyController Controller;
	TestTrue(TEXT("Initialize succeeds from artifact-derived runtime"), Controller.Initialize(*World, Config, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("controller is initialized"), Controller.IsInitialized());

	// Assert — RequiredCommands match artifact command ops (velocity width 1).
	const TArray<FUERLPolicyCommandChannel>& Required = Controller.RequiredCommands();
	TestEqual(TEXT("RequiredCommands count"), Required.Num(), 1);
	if (Required.Num() == 1)
	{
		TestEqual(TEXT("RequiredCommands[0].Name"), Required[0].Name, FName(TEXT("velocity")));
		TestEqual(TEXT("RequiredCommands[0].Width"), Required[0].Width, 1);
	}

	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_POLICY_001: Initialize derived observations/actuators from "
		"artifact plans + robot_runtime; caller config has no enumeration fields"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyControllerStepFiniteAndCommandsTest,
	"UERL.Integration.Policy.Controller.AC_UE_INT_POLICY_002.StepFiniteAndRequiredCommands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyControllerStepFiniteAndCommandsTest::RunTest(const FString& Parameters)
{
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLPolicy"));
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLRobot"));

	FString ArtifactPath;
	FString Error;
	TestTrue(TEXT("resolve controller corpus artifact"), ResolveControllerCorpusPath(ArtifactPath, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = PreviewScene->GetWorld();
	TestNotNull(TEXT("controller step world created"), World);
	if (!World)
	{
		return false;
	}
	AActor* Ground = SpawnGround(*World);
	TestNotNull(TEXT("WorldStatic ground created"), Ground);
	if (!Ground)
	{
		return false;
	}

	FUERLPolicyControllerConfig Config;
	Config.ArtifactPath = ArtifactPath;
	Config.AssetPath = CartPoleRobotAsset;
	Config.GroundOrigin = FVector(0.0, 0.0, 0.0);
	Config.GroundNormal = FVector::UpVector;
	Config.TerrainQueryActors.Add(Ground);
	Config.InitialRootHeightMeters = 0.05;

	FUERLPolicyController Controller;
	TestTrue(TEXT("Initialize succeeds"), Controller.Initialize(*World, Config, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	const TArray<FUERLPolicyCommandChannel>& Required = Controller.RequiredCommands();
	TestEqual(TEXT("RequiredCommands match artifact"), Required.Num(), 1);
	TestTrue(
		TEXT("RequiredCommands reports velocity/1"),
		Required.Num() == 1 && Required[0].Name == FName(TEXT("velocity")) && Required[0].Width == 1);

	FUERLPolicyCommands Commands;
	const float VelocityValues[] = {0.0f};
	Commands.Set(TEXT("velocity"), VelocityValues);
	const FUERLControlTiming Timing{0.0166667, 0.0083333};

	for (int32 StepIndex = 0; StepIndex < 5; ++StepIndex)
	{
		TestTrue(
			*FString::Printf(TEXT("Step %d succeeds with finite state"), StepIndex),
			Controller.Step(Commands, Timing, Error));
		if (!Error.IsEmpty())
		{
			AddError(Error);
			return false;
		}
	}
	TestTrue(
		TEXT("controller records the current observation and solver-step timing"),
		FMath::IsNearlyEqual(Controller.LastControlTiming().ObservationDtSeconds, Timing.ObservationDtSeconds, 1.0e-9)
			&& FMath::IsNearlyEqual(Controller.LastControlTiming().LastSolverStepSeconds, Timing.LastSolverStepSeconds, 1.0e-9));

	// Missing command channel must error (no silent zero-fill).
	FUERLPolicyCommands EmptyCommands;
	TestFalse(TEXT("Step rejects missing command channel"), Controller.Step(EmptyCommands, Timing, Error));
	TestTrue(TEXT("missing-channel error names velocity"), Error.Contains(TEXT("velocity")));

	// Wrong width must error.
	FUERLPolicyCommands WideCommands;
	const float WideValues[] = {0.0f, 1.0f};
	WideCommands.Set(TEXT("velocity"), WideValues);
	TestFalse(TEXT("Step rejects wrong command width"), Controller.Step(WideCommands, Timing, Error));
	TestTrue(TEXT("wrong-width error mentions width"), Error.Contains(TEXT("width")));
	FUERLControlTiming InvalidTiming = Timing;
	InvalidTiming.LastSolverStepSeconds = 0.0;
	TestFalse(TEXT("Step rejects missing solver-step timing"), Controller.Step(Commands, InvalidTiming, Error));
	TestTrue(TEXT("invalid timing error is diagnostic"), Error.Contains(TEXT("timing")));

	Controller.Reset();
	TestTrue(TEXT("Reset leaves controller initialized"), Controller.IsInitialized());

	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_POLICY_002: control steps stay finite-path; RequiredCommands "
		"match artifact; missing/wrong-width commands fail"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyControllerPhantomXPursuitLoopTest,
	"UERL.Integration.Policy.Controller.AC_UE_INT_POLICY_003.PhantomXPursuitControlLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyControllerPhantomXPursuitLoopTest::RunTest(const FString& Parameters)
{
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLPolicy"));
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLRobot"));

	const FString ArtifactPath = FPaths::ConvertRelativePathToFull(
		FPaths::ProjectContentDir() / TEXT("UERLHost/Policies/PhantomXContinuousSmooth.uerlpol2"));
	TestTrue(TEXT("PhantomX deploy artifact exists"), FPaths::FileExists(ArtifactPath));
	if (!FPaths::FileExists(ArtifactPath))
	{
		return false;
	}

	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = PreviewScene->GetWorld();
	TestNotNull(TEXT("pursuit controller test world created"), World);
	if (!World)
	{
		return false;
	}
	AActor* Ground = SpawnGround(*World);
	TestNotNull(TEXT("WorldStatic ground created"), Ground);
	if (!Ground)
	{
		return false;
	}

	// Arrange — same host-facing config surface as the deployment component.
	FUERLPolicyControllerConfig Config;
	Config.ArtifactPath = ArtifactPath;
	Config.AssetPath = TEXT("/Game/Robots/PhantomX/SK_PhantomX.SK_PhantomX");
	Config.GroundOrigin = FVector(0.0, 0.0, 0.0);
	Config.GroundNormal = FVector::UpVector;
	Config.TerrainQueryActors.Add(Ground);
	Config.InitialRootHeightMeters = 0.18;
	Config.bClaimAuthoredActor = false;

	FString Error;
	FUERLPolicyController Controller;
	TestTrue(TEXT("Initialize succeeds for PhantomX deploy artifact"), Controller.Initialize(*World, Config, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	const TArray<FUERLPolicyCommandChannel>& Required = Controller.RequiredCommands();
	TestEqual(TEXT("RequiredCommands count (subsystem velocity channel)"), Required.Num(), 1);
	TestTrue(
		TEXT("RequiredCommands reports velocity/3 as subsystem provides"),
		Required.Num() == 1 && Required[0].Name == FName(TEXT("velocity")) && Required[0].Width == 3);

	// Act — host supplies pursuit commands (zeros stand in for no player pawn).
	FUERLPolicyCommands Commands;
	const float VelocityValues[] = {0.0f, 0.0f, 0.0f};
	Commands.Set(TEXT("velocity"), VelocityValues);
	const FUERLControlTiming Timing{0.02, 0.0083333};

	for (int32 StepIndex = 0; StepIndex < 5; ++StepIndex)
	{
		TestTrue(
			*FString::Printf(TEXT("PhantomX Step %d succeeds with finite state"), StepIndex),
			Controller.Step(Commands, Timing, Error));
		if (!Error.IsEmpty())
		{
			AddError(Error);
			return false;
		}
	}

	AddInfo(TEXT(
		"[VERIFY] AC_UE_INT_POLICY_003: pursuit demo control loop starts, runs steps with "
		"finite state, and velocity command channel is host-supplied. Visual chase/no-fall "
		"on Stylized_Egypt_Demo is not asserted here."));
	return true;
}

#endif
