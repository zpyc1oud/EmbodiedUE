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
#include "PhysicsEngine/BodyInstance.h"
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

#include "UERLPolicyComponentTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyComponentClaimedResetTest,
	"UERL.Integration.Policy.Component.AC_UE_REVIEW_030.ClaimedFloatingResetKeepsCurrentXYAndSnapsToGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyComponentClaimedResetTest::RunTest(const FString& Parameters)
{
	using namespace UERLPolicyComponentTestsPrivate;
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
	ClaimHost->SetActorRotation(FRotator(0.0f, 37.0f, 0.0f));
	AuthoredMesh->SetRelativeLocation(FVector(20.0, -10.0, 15.0));
	const FTransform OwnerBeforeStart = ClaimHost->GetActorTransform();
	const FTransform RootInMesh = AuthoredMesh->GetSocketTransform(TEXT("base_link"), RTS_Component);

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

	FBodyInstance* AuthoredRootBody = AuthoredMesh->GetBodyInstance(TEXT("base_link"));
	TestNotNull(TEXT("claimed floating root physics body exists"), AuthoredRootBody);
	if (!AuthoredRootBody)
	{
		return false;
	}
	FTransform InitialPose;
	TestTrue(TEXT("claimed robot transform is available immediately after Start"),
		Rig.Component->GetRobotTransform(InitialPose));
	TestTrue(TEXT("Start preserves authored mesh position without adding clearance twice"),
		InitialPose.GetLocation().Equals(AuthoredMeshLocation, 1.0e-3));
	TestTrue(TEXT("Start places the physical root at the mesh reference offset, without adding its mount twice"),
		AuthoredRootBody->GetUnrealWorldTransform().Equals(RootInMesh * InitialPose, 1.0e-3));
	TestTrue(TEXT("Start preserves the claimed owner transform"),
		ClaimHost->GetActorTransform().Equals(OwnerBeforeStart, 1.0e-3));

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
	TestTrue(TEXT("pose reset places the physical root in the reported mesh frame"),
		AuthoredRootBody->GetUnrealWorldTransform().Equals(RootInMesh * ResetPose, 1.0e-3));
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
	using namespace UERLPolicyComponentTestsPrivate;
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
	using namespace UERLPolicyComponentTestsPrivate;
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
	using namespace UERLPolicyComponentTestsPrivate;
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
	using namespace UERLPolicyComponentTestsPrivate;
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
	using namespace UERLPolicyComponentTestsPrivate;
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
	using namespace UERLPolicyComponentTestsPrivate;
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
	Rig.Component->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);

	TestEqual(TEXT("reset-input decision emits one diagnostic frame"), Recorder->ControlFrameCount, 1);
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
	TestTrue(TEXT("diagnostic frame identifies the current reset solver frame"), FirstFrame.SolverFrame >= 0);
	TestTrue(TEXT("diagnostic frame includes the current reset solver time"),
		FMath::IsFinite(FirstFrame.SolverTimeSeconds) && FirstFrame.SolverTimeSeconds >= 0.0);
	TestTrue(TEXT("bootstrap consumes no game-time window"),
		FMath::IsFinite(FirstFrame.GameElapsedSeconds) && FirstFrame.GameElapsedSeconds == 0.0);
	TestTrue(TEXT("bootstrap consumes no physics-time window"),
		FMath::IsFinite(FirstFrame.PhysicsElapsedSeconds) && FirstFrame.PhysicsElapsedSeconds == 0.0);
	TestTrue(TEXT("bootstrap observation uses the artifact DtMin"),
		FMath::Abs(FirstFrame.ObservationDtSeconds - 0.005) < 2.0e-3);
	TestTrue(TEXT("bootstrap has no completed solver denominator"),
		FirstFrame.LastSolverStepSeconds == 0.0);
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
	using namespace UERLPolicyComponentTestsPrivate;
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

	// Prime reset input so Stop/SoftReset callbacks below exercise completed physics.
	Rig.Component->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
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
	Rig.Component->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
	TestEqual(TEXT("restarted run emits its own first completed frame"), Recorder->ControlFrameCount, 2);
	if (!Recorder->ControlFrames.IsValidIndex(1))
	{
		AddError(TEXT("the restarted control-frame callback did not provide a snapshot"));
		return false;
	}
	TestEqual(TEXT("component sequence remains monotonic across restart"), Recorder->ControlFrames[1].Sequence, int64(3));
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
	Rig.Component->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
	TestEqual(TEXT("post-reset control starts only a new completed frame"), Recorder->ControlFrameCount, 4);
	if (!Recorder->ControlFrames.IsValidIndex(3))
	{
		AddError(TEXT("the post-reset control-frame callback did not provide a snapshot"));
		return false;
	}
	const FUERLPolicyControlFrameSnapshot& ResetFrame = Recorder->ControlFrames[3];
	TestEqual(TEXT("post-reset frame keeps its monotonic component sequence"), ResetFrame.Sequence, int64(5));
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
	using namespace UERLPolicyComponentTestsPrivate;
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

	Rig.Component->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
	TickPolicyWorld(Rig.World(), 0.005f);
	TestTrue(TEXT("host reset clears the policy and arms a new bootstrap"), Rig.Component->SoftReset());
	TestTrue(TEXT("host records the matching episode boundary"), Trace->MarkEpisodeBoundary(1, TEXT("主机软重置")));
	Rig.Component->OnControlStepOverrun.Broadcast(0.04f, 0.04f, 0.035f);
	Rig.Component->OnCommandStale.Broadcast(FName(TEXT("velocity")), 0.04f);
	Rig.Component->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
	TickPolicyWorld(Rig.World(), 0.005f);
	TestTrue(TEXT("unannounced host restart arms a bootstrap frame"), Rig.Component->SoftReset());
	Rig.Component->TickComponent(0.0f, ELevelTick::LEVELTICK_All, nullptr);
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
		Contents.Contains(TEXT("event: unmarked_bootstrap_boundary\n    sequence: 5\n    episode_index: 1\n    episode_step: 2")));
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
		ContinuationContents.Contains(TEXT("event: control_step_overrun\n    sequence: 6"))
		&& ContinuationContents.Contains(TEXT("kind: frame\n    sequence: 6")));
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
	FString RetainedContents;
	TestTrue(TEXT("the retained trace is readable after a refused replacement"),
		FFileHelper::LoadFileToString(RetainedContents, *TracePath));
	TestEqual(TEXT("refusing a replacement preserves the finalized trace"), RetainedContents, Contents);
	UUERLPolicyTraceRecorder* AbandonedTrace = NewObject<UUERLPolicyTraceRecorder>(Rig.Host);
	Rig.Host->AddInstanceComponent(AbandonedTrace);
	AbandonedTrace->PolicyComponent = Rig.Component;
	AbandonedTrace->RegisterComponent();
	if (!AbandonedTrace->HasBegunPlay())
	{
		AbandonedTrace->BeginPlay();
	}
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
	using namespace UERLPolicyComponentTestsPrivate;
	FString Error;
	UUERLPolicyArtifactAsset* Asset = MakeTransientAsset(Error, TEXT("cartpole_no_commands.uerlpol2"));
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyBootstrapBeforePhysicsTest,
	"UERL.Integration.Policy.Component.AC_UE_INT_COMPONENT_031.BootstrapObservesResetBeforePhysics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyBootstrapBeforePhysicsTest::RunTest(const FString& Parameters)
{
	using namespace UERLPolicyComponentTestsPrivate;
	UUERLPolicyArtifactAsset* Asset = RequirePhantomXAsset(*this);
	if (!Asset) { return false; }
	FComponentTestRig Rig;
	if (!Rig.Build(*this, Asset)) { return false; }
	FDeployPhysicsSettingsGuard Guard(0.005f, 10);
	UUERLPolicyComponentTestEventRecorder* Recorder = NewObject<UUERLPolicyComponentTestEventRecorder>();
	BindRecorderEvent(Rig.Component->OnControlStepCompleted, Recorder,
		GET_FUNCTION_NAME_CHECKED(UUERLPolicyComponentTestEventRecorder, OnControlFrameCompleted));
	if (!StartWithZeroCommand(*this, Rig.Component)) { return false; }
	FUERLSolverClockSnapshot InitialClock;
	FString Error;
	TestTrue(TEXT("initial solver clock is readable"), ReadUERLSolverClock(*Rig.World(), InitialClock, Error));
	TestEqual(TEXT("Start does not infer synchronously inside caller callbacks"), Recorder->ControlFrameCount, 0);
	TickPolicyWorld(Rig.World(), 0.020f);
	TestEqual(TEXT("first world tick infers once before physics and once after its completed window"), Recorder->ControlFrameCount, 2);
	if (Recorder->ControlFrames.Num() < 2) { return false; }
	const FUERLPolicyControlFrameSnapshot& Bootstrap = Recorder->ControlFrames[0];
	const FUERLPolicyControlFrameSnapshot& Completed = Recorder->ControlFrames[1];
	TestTrue(TEXT("first decision is explicitly bootstrap"), Bootstrap.bBootstrap);
	TestEqual(TEXT("bootstrap consumes reset state before a new solver frame"), Bootstrap.SolverFrame, InitialClock.Frame);
	TestTrue(TEXT("bootstrap has no fabricated completed physics window"), Bootstrap.PhysicsElapsedSeconds == 0.0);
	TestTrue(TEXT("bootstrap has no fabricated impulse denominator"), Bootstrap.LastSolverStepSeconds == 0.0);
	TestFalse(TEXT("second decision is a completed-window input"), Completed.bBootstrap);
	TestTrue(TEXT("initial action drives the first full physics window"), Completed.SolverFrame > Bootstrap.SolverFrame);
	TestTrue(TEXT("first completed window retains all 20ms"), FMath::Abs(Completed.PhysicsElapsedSeconds - 0.020) < 1.e-6);
	TestTrue(TEXT("first completed window reads the actual last solver dt"), FMath::Abs(Completed.LastSolverStepSeconds - 0.005) < 1.e-6);
	Recorder->ControlFrameCallbackComponent = Rig.Component;
	Recorder->ControlFrameCallbackMode = EUERLPolicyTestFrameCallbackMode::SoftReset;
	TickPolicyWorld(Rig.World(), 0.020f);
	TestEqual(TEXT("post-physics callback reset does not infer again in the same tick"), Recorder->ControlFrameCount, 3);
	TestTrue(TEXT("callback reset arms a future pre-physics bootstrap"), Rig.Component->IsBootstrapPending());
	TestEqual(TEXT("callback reset leaves no consumed control timing"), Rig.Component->GetLastControlTiming().ObservationDtSeconds, 0.0);
	Recorder->ControlFrameCallbackMode = EUERLPolicyTestFrameCallbackMode::RecordOnly;
	TickPolicyWorld(Rig.World(), 0.020f);
	TestEqual(TEXT("next world tick emits a reset input and its completed successor"), Recorder->ControlFrameCount, 5);
	if (Recorder->ControlFrames.Num() >= 5)
	{
		TestTrue(TEXT("new bootstrap clears previous action history"), Recorder->ControlFrames[3].bBootstrap
			&& !Recorder->ControlFrames[3].PreviousAction.ContainsByPredicate([](float V) { return V != 0.0f; }));
		TestFalse(TEXT("new successor is a completed window"), Recorder->ControlFrames[4].bBootstrap);
	}
	Rig.Component->StopPolicy();
	return true;
}

#endif
