#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#include "Chaos/RigidParticles.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsEngine/PhysicsConstraintComponent.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PreviewScene.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/GarbageCollection.h"

#include "UERLBatchBinding.h"
#include "UERLGenericRobotCommandApplier.h"
#include "UERLGenericRobotKinematics.h"
#include "UERLGenericRobotProvider.h"
#include "UERLRegistry.h"
#include "UERLSkeletalMeshRobotRuntime.h"
#include "UERLSlotCollisionPlan.h"

#include "UERLGenericRobotTestFixtures.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyGroundSamplingTest,
	"UERL.Integration.Policy.Ground.AC_UE_INT_POLICY_GROUND_001_005.RootRelativeCaching",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyGroundSamplingTest::RunTest(const FString& Parameters)
{
	using namespace UERLGenericRobotTestsPrivate;
	TSharedRef<IUERLRobotFactory> Factory = UERLGenericRobot::MakeFactory();
	FString Error;
	FUERLProviderConfig Input;
	Input.AssetPath = CartPoleRobotAsset;
	FUERLProviderConfig Effective;
	if (!Factory->ValidateConfig(Input, Effective, Error))
	{
		AddError(Error);
		return false;
	}
	const FUERLRobotTopology& ReflectedTopology = Factory->Describe().Topology;
	const int32 CartJointIndex = ReflectedTopology.Joints.IndexOfByPredicate(
		[](const FUERLJointTopology& Joint)
		{
			return Joint.CoordinateType == EUERLJointCoordinateType::Prismatic;
		});
	TestTrue(TEXT("ground test exposes a scalar prismatic actuator"), CartJointIndex != INDEX_NONE);
	if (CartJointIndex == INDEX_NONE)
	{
		return false;
	}
	const FUERLJointTopology& CartJoint = ReflectedTopology.Joints[CartJointIndex];
	Input.Actuators.Add(FUERLActuatorConfig{
		0, CartJointIndex, CartJoint.Name, TEXT("prismatic"), TEXT("linear_x"), TEXT("N"), TEXT("effort"),
		0.0, 1.0, 100.0, 0.0 });
	if (!Factory->ValidateConfig(Input, Effective, Error))
	{
		AddError(Error);
		return false;
	}
	const FUERLRobotDescriptor& Descriptor = Factory->Describe();
	const FUERLRobotTopology& Topology = Descriptor.Topology;
	const FUERLFieldDescriptor* TerrainField = Descriptor.StateFields.FindByPredicate(
		[&Topology](const FUERLFieldDescriptor& Field)
		{
			return Field.Observation.Type == EUERLObservationType::TerrainHeight
				&& Field.Observation.BodyIndex == Topology.RootBodyIndex;
		});
	const FUERLFieldDescriptor* RootClearanceField = Descriptor.StateFields.FindByPredicate(
		[&Topology](const FUERLFieldDescriptor& Field)
		{
			return Field.Observation.Type == EUERLObservationType::GroundClearance
				&& Field.Observation.BodyIndex == Topology.RootBodyIndex;
		});
	const int32 CartBodyIndex = Topology.BodyNames.IndexOfByKey(FName(TEXT("cart")));
	const FUERLFieldDescriptor* CartClearanceField = Descriptor.StateFields.FindByPredicate(
		[CartBodyIndex](const FUERLFieldDescriptor& Field)
		{
			return CartBodyIndex != INDEX_NONE
				&& Field.Observation.Type == EUERLObservationType::GroundClearance
				&& Field.Observation.BodyIndex == CartBodyIndex;
		});
	const FUERLFieldDescriptor* RootPoseField = Descriptor.StateFields.FindByPredicate(
		[&Topology](const FUERLFieldDescriptor& Field)
		{
			return Field.Observation.Type == EUERLObservationType::BodyPose
				&& Field.Observation.BodyIndex == Topology.RootBodyIndex;
		});
	TestNotNull(TEXT("ground test publishes root terrain height"), TerrainField);
	TestNotNull(TEXT("ground test publishes root clearance"), RootClearanceField);
	TestNotNull(TEXT("ground test publishes cart clearance"), CartClearanceField);
	TestNotNull(TEXT("ground test publishes root pose"), RootPoseField);
	if (!TerrainField || !RootClearanceField || !CartClearanceField || !RootPoseField)
	{
		return false;
	}

	const TArray<FUERLFieldDescriptor> SelectedStateFields = {
		*TerrainField, *RootClearanceField, *CartClearanceField, *RootPoseField };
	TUniquePtr<IUERLRobot> Robot = Factory->Create(Effective);
	TestTrue(TEXT("ground test prepares selected State plan"),
		Robot->PrepareState(SelectedStateFields, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}
	FUERLBatchSchema Schema;
	int32 StateColumn = 0;
	for (const FUERLFieldDescriptor& Field : SelectedStateFields)
	{
		Schema.StateFields.Add(FUERLBatchFieldBinding{ Field, StateColumn });
		StateColumn += Field.Width;
	}
	Schema.StateWidth = StateColumn;
	Schema.ActionFields.Add(FUERLBatchFieldBinding{ Descriptor.ActionFields[0], 0 });
	Schema.ActionWidth = Descriptor.ActionFields[0].Width;
	FUERLBatchBinding Binding;
	TestTrue(TEXT("ground test schema compiles with the provider action contract"),
		Binding.Compile(Schema, Descriptor.ActionFields, Descriptor.StateFields, Error));
	if (!Binding.IsCompiled())
	{
		AddError(Error);
		return false;
	}
	const FUERLBatchFieldBinding* TerrainBinding = Binding.FindState(TerrainField->Name);
	const FUERLBatchFieldBinding* RootClearanceBinding = Binding.FindState(RootClearanceField->Name);
	const FUERLBatchFieldBinding* CartClearanceBinding = Binding.FindState(CartClearanceField->Name);
	const FUERLBatchFieldBinding* RootPoseBinding = Binding.FindState(RootPoseField->Name);
	if (!TerrainBinding || !RootClearanceBinding || !CartClearanceBinding || !RootPoseBinding)
	{
		AddError(TEXT("ground test selected State bindings are incomplete"));
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
	TestNotNull(TEXT("ground sampling world created"), World);
	if (!World)
	{
		return false;
	}

	const FVector TrainingOrigin(-1000.0, 0.0, 100.0);
	const FVector DeploymentOrigin(1000.0, 0.0, 100.0);
	AActor* TrainingBase = SpawnWorldStaticBox(*World, FVector(-1000.0, 0.0, -25.0), FVector(10.0, 10.0, 0.5));
	AActor* TrainingStep = SpawnWorldStaticBox(*World, FVector(-925.0, 0.0, 25.0), FVector(3.0, 10.0, 0.5));
	AActor* DeploymentBase = SpawnWorldStaticBox(*World, FVector(1000.0, 0.0, -25.0), FVector(10.0, 10.0, 0.5));
	AActor* DeploymentStep = SpawnWorldStaticBox(*World, FVector(1075.0, 0.0, 25.0), FVector(3.0, 10.0, 0.5));
	TestNotNull(TEXT("training base fixture created"), TrainingBase);
	TestNotNull(TEXT("training step fixture created"), TrainingStep);
	TestNotNull(TEXT("deployment base fixture created"), DeploymentBase);
	TestNotNull(TEXT("deployment step fixture created"), DeploymentStep);
	if (!TrainingBase || !TrainingStep || !DeploymentBase || !DeploymentStep)
	{
		return false;
	}

	FUERLSlotCollisionPlan CollisionPlan;
	TestTrue(TEXT("shared-world ground collision plan compiles"), CollisionPlan.Compile(
		2, EUERLEnvironmentCollisionScope::SharedWorld, Error));
	TArray<FUERLSlotContext> Slots;
	Slots.SetNum(2);
	Slots[0].SlotId = 0;
	Slots[0].Origin = TrainingOrigin;
	Slots[0].GroundHeight = TrainingOrigin.Z;
	Slots[0].GroundNormal = FVector::UpVector;
	Slots[0].TerrainQueryActors = { TrainingBase, TrainingStep };
	Slots[0].TerrainQueryPurpose = EUERLTerrainQueryPurpose::TrainingOwned;
	Slots[0].CollisionProfile = CollisionPlan.Profile(0);
	Slots[1].SlotId = 1;
	Slots[1].Origin = DeploymentOrigin;
	Slots[1].GroundHeight = DeploymentOrigin.Z;
	Slots[1].GroundNormal = FVector(0.0, 1.0, 1.0).GetSafeNormal();
	Slots[1].TerrainQueryPurpose = EUERLTerrainQueryPurpose::DeploymentWorldStatic;
	Slots[1].CollisionProfile = CollisionPlan.Profile(1);
	TestTrue(TEXT("training and deployment Robots spawn into the shared World"),
		Robot->SpawnIntoSlots(*World, Slots, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	FUERLResetBatch InitialReset;
	for (const FUERLSlotContext& Slot : Slots)
	{
		FUERLResetRow& Row = InitialReset.Rows.AddDefaulted_GetRef();
		Row.SlotId = Slot.SlotId;
		Row.Origin = Slot.Origin;
		Row.GroundHeight = Slot.GroundHeight;
		Row.GroundNormal = Slot.GroundNormal;
	}
	TestTrue(TEXT("initial reset clears the two observation caches before sampling"),
		Robot->ResetSlots(InitialReset, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<float> StateData;
	StateData.SetNumZeroed(Schema.StateWidth * 2);
	FUERLNamedStateWriter Writer(Binding, { StateData.GetData(), 2, Schema.StateWidth });
	TestTrue(TEXT("training and deployment collect the initial terrain State"),
		Robot->TryCollectState({ 0, 1 }, Writer, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}
	const TArray<float> InitialState = StateData;
	bool bTerrainEqual = true;
	for (int32 Index = 0; Index < UERLTerrainHeightScanWidth; ++Index)
	{
		bTerrainEqual &= FMath::IsNearlyEqual(
			InitialState[TerrainBinding->Column + Index],
			InitialState[Schema.StateWidth + TerrainBinding->Column + Index],
			1.0e-4f);
	}
	TestTrue(TEXT("training and deployment emit the same root-relative 35-point scan"), bTerrainEqual);
	bool bDeploymentHasLowAndHighSamples = false;
	bool bDeploymentHasHighSample = false;
	bool bDeploymentHasLowSample = false;
	constexpr int32 StepSampleIndex = 12;
	for (int32 Index = 0; Index < UERLTerrainHeightScanWidth; ++Index)
	{
		const float Value = InitialState[Schema.StateWidth + TerrainBinding->Column + Index];
		bDeploymentHasHighSample |= Value > -0.75f;
		bDeploymentHasLowSample |= Value < -0.9f;
	}
	bDeploymentHasLowAndHighSamples = bDeploymentHasHighSample && bDeploymentHasLowSample;
	TestTrue(TEXT("the 35-point fixture observes a mixed-height step instead of all-zero terrain"),
		bDeploymentHasLowAndHighSamples);
	TestTrue(TEXT("deployment root pose preserves a non-horizontal world-up posture"),
		FMath::Abs(InitialState[Schema.StateWidth + RootPoseBinding->Column + 3]) > 1.0e-3f
			|| FMath::Abs(InitialState[Schema.StateWidth + RootPoseBinding->Column + 4]) > 1.0e-3f);

	TUniquePtr<IUERLRobot> FilteredRobot = Factory->Create(Effective);
	TestTrue(TEXT("training-filter Robot prepares the same State plan"),
		FilteredRobot->PrepareState(SelectedStateFields, Error));
	TArray<FUERLSlotContext> FilteredSlots;
	FilteredSlots.SetNum(1);
	FilteredSlots[0].SlotId = 0;
	FilteredSlots[0].Origin = TrainingOrigin;
	FilteredSlots[0].GroundHeight = TrainingOrigin.Z;
	FilteredSlots[0].GroundNormal = FVector::UpVector;
	FilteredSlots[0].TerrainQueryActors = { TrainingBase };
	FilteredSlots[0].TerrainQueryPurpose = EUERLTerrainQueryPurpose::TrainingOwned;
	FilteredSlots[0].CollisionProfile = CollisionPlan.Profile(0);
	TestTrue(TEXT("training-filter Robot spawns"),
		FilteredRobot->SpawnIntoSlots(*World, FilteredSlots, Error));
	FUERLResetBatch FilteredReset;
	FilteredReset.Rows.Add(FUERLResetRow{ 0, 0, TrainingOrigin, TrainingOrigin.Z, FVector::UpVector, {} });
	TestTrue(TEXT("training-filter Robot resets"), FilteredRobot->ResetSlots(FilteredReset, Error));
	TArray<float> FilteredState;
	FilteredState.SetNumZeroed(Schema.StateWidth);
	FUERLNamedStateWriter FilteredWriter(Binding, { FilteredState.GetData(), 1, Schema.StateWidth });
	TestTrue(TEXT("training owner filter collects its owned base"),
		FilteredRobot->TryCollectState({ 0 }, FilteredWriter, Error));
	TestTrue(TEXT("training owner filter excludes an unowned WorldStatic step"),
		FilteredState[TerrainBinding->Column + StepSampleIndex]
			< InitialState[Schema.StateWidth + TerrainBinding->Column + StepSampleIndex] - 0.2f);
	FilteredRobot->DestroySlots();

	TUniquePtr<IUERLRobot> EmptyTrainingRobot = Factory->Create(Effective);
	TestTrue(TEXT("empty-owner training Robot prepares"),
		EmptyTrainingRobot->PrepareState(SelectedStateFields, Error));
	FilteredSlots[0].TerrainQueryActors.Reset();
	TestTrue(TEXT("empty-owner training Robot spawns"),
		EmptyTrainingRobot->SpawnIntoSlots(*World, FilteredSlots, Error));
	TestTrue(TEXT("empty-owner training Robot resets"), EmptyTrainingRobot->ResetSlots(FilteredReset, Error));
	FilteredState.SetNumZeroed(Schema.StateWidth);
	TestFalse(TEXT("empty training owner list fails instead of widening the query"),
		EmptyTrainingRobot->TryCollectState({ 0 }, FilteredWriter, Error));
	TestTrue(TEXT("empty training owner error is explicit"), Error.Contains(TEXT("no training terrain owner")));
	EmptyTrainingRobot->DestroySlots();

	USkeletalMeshComponent* DeploymentComponent = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (USkeletalMeshComponent* Candidate = It->FindComponentByClass<USkeletalMeshComponent>())
		{
			if (Candidate->GetFName() == FName(TEXT("RobotSkeletalMesh"))
				&& Candidate->GetComponentLocation().X > 0.0)
			{
				DeploymentComponent = Candidate;
				break;
			}
		}
	}
	TestNotNull(TEXT("deployment Robot component is available for cache isolation"), DeploymentComponent);
	if (!DeploymentComponent)
	{
		Robot->DestroySlots();
		return false;
	}
	FBodyInstance* DeploymentCart = DeploymentComponent->GetBodyInstance(FName(TEXT("cart")));
	TestNotNull(TEXT("deployment cart body is available for cache isolation"), DeploymentCart);
	if (!DeploymentCart)
	{
		Robot->DestroySlots();
		return false;
	}
	const float InitialRootClearance = InitialState[Schema.StateWidth + RootClearanceBinding->Column];
	const float InitialCartClearance = InitialState[Schema.StateWidth + CartClearanceBinding->Column];
	DeploymentBase->Destroy();
	DeploymentStep->Destroy();
	FTransform RaisedCart = DeploymentCart->GetUnrealWorldTransform();
	RaisedCart.AddToTranslation(FVector(0.0, 0.0, 100.0));
	DeploymentCart->SetBodyTransform(RaisedCart, ETeleportType::TeleportPhysics, false);
	StateData.SetNumZeroed(Schema.StateWidth * 2);
	TestTrue(TEXT("lost ground uses the deployment cache and current body position"),
		Robot->TryCollectState({ 1 }, Writer, Error));
	const float CachedRootClearance = StateData[Schema.StateWidth + RootClearanceBinding->Column];
	const float ReprojectedCartClearance = StateData[Schema.StateWidth + CartClearanceBinding->Column];
	TestTrue(TEXT("root clearance cache stays tied to its own body"),
		FMath::IsNearlyEqual(CachedRootClearance, InitialRootClearance, 1.0e-3f));
	TestTrue(TEXT("cart clearance cache reprojects against the current cart body"),
		FMath::IsNearlyEqual(ReprojectedCartClearance, InitialCartClearance + 1.0f, 2.0e-2f));

	FUERLResetBatch NoGroundReset;
	NoGroundReset.Rows.Add(FUERLResetRow{ 1, 0, FVector(2000.0, 0.0, 100.0), 100.0, FVector::UpVector, {} });
	TestTrue(TEXT("deployment reset clears cached terrain hits"), Robot->ResetSlots(NoGroundReset, Error));
	StateData.SetNumZeroed(Schema.StateWidth * 2);
	TestFalse(TEXT("post-reset collection fails without an initial WorldStatic hit"),
		Robot->TryCollectState({ 1 }, Writer, Error));
	TestTrue(TEXT("missing initial hit returns a descriptive error"), Error.Contains(TEXT("initial hit")));

	Robot->DestroySlots();
	if (TrainingBase)
	{
		TrainingBase->Destroy();
	}
	if (TrainingStep)
	{
		TrainingStep->Destroy();
	}
	PreviewScene.Reset();
	AddInfo(TEXT(
		"[VERIFY] AC-UE-INT-POLICY-GROUND-001..005: root-relative 35-point terrain, explicit training/deployment filters, "
		"per-body cache reprojection, reset invalidation, and initial-hit errors"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotIdleDriveTargetTest,
	"UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_004.IdleRobotFollowsNewDriveTargets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotIdleDriveTargetTest::RunTest(const FString& Parameters)
{
	using namespace UERLGenericRobotTestsPrivate;
	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = PreviewScene->GetWorld();
	AActor* Ground = World ? SpawnGround(*World) : nullptr;
	TestNotNull(TEXT("idle-drive WorldStatic ground created"), Ground);
	if (!Ground)
	{
		return false;
	}

	// SpawnGround's 50 cm cube is centred at Z=-50, so its top face is Z=-25.
	FUERLSkeletalMeshRobotRuntimeConfig Config = PhantomXRuntimeConfig(FVector(0.0, 0.0, -25.0));
	Config.Observations.Add({ EUERLObservationType::JointPosition, FName(TEXT("thigh_rf")) });
	FUERLSkeletalMeshRobotRuntime Runtime;
	FString Error;
	if (!Runtime.Initialize(*World, Config, Error))
	{
		AddError(FString::Printf(TEXT("PhantomX runtime initialization failed: %s"), *Error));
		return false;
	}
	USkeletalMeshComponent* Mesh = FindSpawnedRobotMesh(*World);
	TestNotNull(TEXT("spawned PhantomX mesh is available"), Mesh);
	if (!Mesh)
	{
		return false;
	}

	TArray<float> Targets;
	for (const FUERLSkeletalMeshRuntimeActuator& Actuator : Config.Actuators)
	{
		Targets.Add(static_cast<float>(Actuator.DefaultPosition));
	}
	constexpr float PhysicsDt = 0.005f;
	constexpr int32 ControlDecimation = 7;
	// Four seconds of an unchanged standing command is far beyond any settling transient.
	for (int32 Frame = 0; Frame < 800; ++Frame)
	{
		if (Frame % ControlDecimation == 0 && !Runtime.ApplyActuatorTargets(Targets, Error))
		{
			AddError(Error);
			return false;
		}
		World->Tick(ELevelTick::LEVELTICK_All, PhysicsDt);
		++GFrameCounter;
	}
	int32 SleepingBodies = 0;
	for (const FBodyInstance* Body : Mesh->Bodies)
	{
		SleepingBodies += Body && Body->IsValidBodyInstance() && !Body->IsInstanceAwake() ? 1 : 0;
	}
	AddInfo(FString::Printf(TEXT("[VERIFY] idle PhantomX sleeping bodies=%d/%d after 4 s"), SleepingBodies, Mesh->Bodies.Num()));
	TestEqual(TEXT("an idle standing robot keeps every body awake"), SleepingBodies, 0);

	TArray<float> State;
	if (!Runtime.CollectState(State, Error) || State.Num() != 1)
	{
		AddError(Error);
		return false;
	}
	const float IdleThigh = State[0];
	for (int32 Index = 0; Index < Config.Actuators.Num(); ++Index)
	{
		if (Config.Actuators[Index].JointName.ToString().StartsWith(TEXT("thigh_")))
		{
			Targets[Index] += 0.5f;
		}
	}
	if (!Runtime.ApplyActuatorTargets(Targets, Error))
	{
		AddError(Error);
		return false;
	}
	for (int32 Frame = 0; Frame < 200; ++Frame)
	{
		World->Tick(ELevelTick::LEVELTICK_All, PhysicsDt);
		++GFrameCounter;
	}
	if (!Runtime.CollectState(State, Error) || State.Num() != 1)
	{
		AddError(Error);
		return false;
	}
	AddInfo(FString::Printf(TEXT("[VERIFY] thigh_rf idle=%.4f after new target=%.4f (target %.4f)"),
		IdleThigh, State[0], IdleThigh + 0.5f));
	TestTrue(TEXT("a changed drive target moves the idle joint toward it"), State[0] - IdleThigh > 0.2f);
	Runtime.Reset();

	AActor* ClaimHost = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FVector(300.0, 0.0, 0.0)));
	USkeletalMeshComponent* AuthoredMesh = NewObject<USkeletalMeshComponent>(ClaimHost);
	AuthoredMesh->SetSkeletalMesh(LoadObject<USkeletalMesh>(nullptr, PhantomXRobotAsset));
	ClaimHost->SetRootComponent(AuthoredMesh);
	AuthoredMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	AuthoredMesh->RegisterComponent();
	AuthoredMesh->SetSimulatePhysics(true);
	if (!TestEqual(TEXT("authored claim fixture has all physical bodies before capture"), AuthoredMesh->Bodies.Num(), 19)
		|| !TestEqual(TEXT("authored claim fixture has all joints before capture"), AuthoredMesh->Constraints.Num(), 18))
	{
		return false;
	}
	auto CountBodiesWithSleepType = [AuthoredMesh](Chaos::ESleepType SleepType)
	{
		int32 Count = 0;
		for (FBodyInstance* Body : AuthoredMesh->Bodies)
		{
			const FPhysicsActorHandle Handle = Body ? Body->GetPhysicsActorHandle() : nullptr;
			Count += Handle && Handle->GetGameThreadAPI().SleepType() == SleepType ? 1 : 0;
		}
		return Count;
	};
	TArray<FIntVector> AuthoredIterations;
	TArray<bool> AuthoredIterationOverrides;
	for (int32 Index = 0; Index < AuthoredMesh->Bodies.Num(); ++Index)
	{
		FBodyInstance* Body = AuthoredMesh->Bodies[Index];
		if (!Body) { AddError(TEXT("missing authored iteration body")); return false; }
		Body->SetPositionSolverIterationCount(Index % 2 == 0 ? 16 : 48);
		Body->SetVelocitySolverIterationCount(Index % 2 == 0 ? 2 : 12);
		Body->SetProjectionSolverIterationCount(Index % 2);
		Body->SetOverrideIterationCounts(Index % 3 != 0);
		AuthoredIterations.Add(FIntVector(Body->PositionSolverIterationCount,
			Body->VelocitySolverIterationCount, Body->ProjectionSolverIterationCount));
		AuthoredIterationOverrides.Add(Body->GetPositionSolverIterationCount() >= 0);
	}
	TArray<bool> AuthoredProjection;
	for (int32 Index = 0; Index < AuthoredMesh->Constraints.Num(); ++Index)
	{
		FConstraintInstance* C = AuthoredMesh->Constraints[Index];
		if (!C) { AddError(TEXT("missing authored claim constraint")); return false; }
		if (Index % 2 == 0) { C->EnableProjection(); } else { C->DisableProjection(); }
		AuthoredProjection.Add(C->IsProjectionEnabled());
	}
	TWeakObjectPtr<UPhysicalMaterial> AuthoredMaterial = NewObject<UPhysicalMaterial>(GetTransientPackage());
	AuthoredMesh->SetPhysMaterialOverride(AuthoredMaterial.Get());
	FUERLSkeletalMeshRobotRuntimeConfig ClaimConfig = PhantomXRuntimeConfig(FVector(300.0, 0.0, -25.0));
	ClaimConfig.Observations = Config.Observations;
	ClaimConfig.bClaimAuthoredActor = true;
	ClaimConfig.ClaimedMesh = AuthoredMesh;
	if (!Runtime.Initialize(*World, ClaimConfig, Error))
	{
		AddError(FString::Printf(TEXT("claimed PhantomX runtime initialization failed: %s"), *Error));
		return false;
	}
	TestTrue(TEXT("claimed robot has physics bodies"), AuthoredMesh->Bodies.Num() > 0);
	for (const FConstraintInstance* C : AuthoredMesh->Constraints)
	{
		TestTrue(TEXT("claimed Robot disables position projection while controlled"), C && !C->IsProjectionEnabled());
	}
	for (int32 Index = 0; Index < AuthoredMesh->Bodies.Num(); ++Index)
	{
		const FBodyInstance* Body = AuthoredMesh->Bodies[Index];
		TestTrue(TEXT("claim enforces minimum solver accuracy"), Body
			&& Body->GetPositionSolverIterationCount() >= 32 && Body->GetVelocitySolverIterationCount() >= 8);
		if (Body && AuthoredIterationOverrides[Index])
		{
			TestEqual(TEXT("claim preserves larger authored position count"),
				Body->GetPositionSolverIterationCount(), FMath::Max(32, AuthoredIterations[Index].X));
			TestEqual(TEXT("claim preserves larger authored velocity count"),
				Body->GetVelocitySolverIterationCount(), FMath::Max(8, AuthoredIterations[Index].Y));
			TestEqual(TEXT("claim preserves authored projection count"),
				Body->GetProjectionSolverIterationCount(), AuthoredIterations[Index].Z);
		}
	}
	TestEqual(TEXT("claimed robot bodies never sleep while controlled"),
		CountBodiesWithSleepType(Chaos::ESleepType::NeverSleep), AuthoredMesh->Bodies.Num());
	// Material replacement uses the same component API as ground-friction events.
	// The original must survive GC while it is no longer referenced by the mesh.
	AuthoredMesh->SetPhysMaterialOverride(NewObject<UPhysicalMaterial>(GetTransientPackage()));
	CollectGarbage(RF_NoFlags);
	TestTrue(TEXT("claimed original material survives garbage collection"), AuthoredMaterial.IsValid());
	Runtime.Reset();
	TestTrue(TEXT("claim release restores the original material after garbage collection"),
		AuthoredMaterial.IsValid() && AuthoredMesh->GetPhysicsMaterialOverride() == AuthoredMaterial.Get());
	for (int32 Index = 0; Index < AuthoredProjection.Num(); ++Index)
	{
		const FConstraintInstance* C = AuthoredMesh->GetConstraintInstanceByIndex(Index);
		TestTrue(TEXT("claim release restores each authored projection setting"),
			C && C->IsProjectionEnabled() == AuthoredProjection[Index]);
	}
	for (int32 Index = 0; Index < AuthoredMesh->Bodies.Num(); ++Index)
	{
		const FBodyInstance* Body = AuthoredMesh->Bodies[Index];
		TestTrue(TEXT("claim release restores authored iteration counts and override state"), Body
			&& Body->PositionSolverIterationCount == AuthoredIterations[Index].X
			&& Body->VelocitySolverIterationCount == AuthoredIterations[Index].Y
			&& Body->ProjectionSolverIterationCount == AuthoredIterations[Index].Z
			&& (Body->GetPositionSolverIterationCount() >= 0) == AuthoredIterationOverrides[Index]);
	}
	TestEqual(TEXT("release restores the authored material sleep type"),
		CountBodiesWithSleepType(Chaos::ESleepType::MaterialSleep), AuthoredMesh->Bodies.Num());

	PreviewScene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-E2E-ROBOT-CONTENT-004: idle PhantomX stays awake, follows a changed drive target, and claim release restores sleep"));
	return true;
}

namespace UERLGenericRobotDriveTestsPrivate
{
	/** Pin synchronous 5 ms Chaos steps: deployment substeps, or one lockstep step per tick. */
	struct FFiveMillisecondPhysicsGuard
	{
		explicit FFiveMillisecondPhysicsGuard(bool bSubstepping = true)
		{
			UPhysicsSettings* Settings = GetMutableDefault<UPhysicsSettings>();
			OldTickAsync = Settings->bTickPhysicsAsync;
			OldSubstepping = Settings->bSubstepping;
			OldSubsteppingAsync = Settings->bSubsteppingAsync;
			OldSubstep = Settings->MaxSubstepDeltaTime;
			OldMaxSubsteps = Settings->MaxSubsteps;
			Settings->bTickPhysicsAsync = false;
			Settings->bSubstepping = bSubstepping;
			Settings->bSubsteppingAsync = false;
			Settings->MaxSubstepDeltaTime = 0.005f;
			Settings->MaxSubsteps = 7;
		}

		~FFiveMillisecondPhysicsGuard()
		{
			UPhysicsSettings* Settings = GetMutableDefault<UPhysicsSettings>();
			Settings->bTickPhysicsAsync = OldTickAsync;
			Settings->bSubstepping = OldSubstepping;
			Settings->bSubsteppingAsync = OldSubsteppingAsync;
			Settings->MaxSubstepDeltaTime = OldSubstep;
			Settings->MaxSubsteps = OldMaxSubsteps;
		}

		bool OldTickAsync = false;
		bool OldSubstepping = false;
		bool OldSubsteppingAsync = false;
		float OldSubstep = 0.0f;
		int32 OldMaxSubsteps = 0;
	};

	UStaticMeshComponent* AddTestCube(AActor& Owner, const FVector& Location, bool bSimulate)
	{
		UStaticMeshComponent* Cube = NewObject<UStaticMeshComponent>(&Owner);
		Cube->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Cube->SetMobility(EComponentMobility::Movable);
		Cube->SetWorldLocation(Location);
		Cube->SetWorldScale3D(FVector(0.1));
		Cube->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Owner.AddInstanceComponent(Cube);
		Cube->RegisterComponent();
		Cube->SetEnableGravity(false);
		Cube->SetSimulatePhysics(bSimulate);
		return Cube;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotRevoluteDriveCoordinateTest,
	"UERL.Integration.Robot.GenericDrive.AC_UE_INT_ROBOT_DRIVE_001.RevoluteDriveEnablesTheUnlockedAxis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotRevoluteDriveCoordinateTest::RunTest(const FString& Parameters)
{
	using namespace UERLGenericRobotDriveTestsPrivate;
	FFiveMillisecondPhysicsGuard Guard;
	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = PreviewScene->GetWorld();

	struct FCase
	{
		EUERLJointCoordinate Coordinate;
		UStaticMeshComponent* Parent = nullptr;
		UStaticMeshComponent* Child = nullptr;
		UPhysicsConstraintComponent* Joint = nullptr;
	};
	TArray<FCase> Cases = {
		{ EUERLJointCoordinate::Twist },
		{ EUERLJointCoordinate::Swing1 },
		{ EUERLJointCoordinate::Swing2 },
	};
	for (int32 Index = 0; Index < Cases.Num(); ++Index)
	{
		FCase& Case = Cases[Index];
		const FVector Location(300.0 * Index, 0.0, 500.0);
		AActor* Owner = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Location));
		Case.Parent = AddTestCube(*Owner, Location, false);
		Case.Child = AddTestCube(*Owner, Location, true);
		Case.Joint = NewObject<UPhysicsConstraintComponent>(Owner);
		Case.Joint->SetWorldLocation(Location);
		Case.Joint->SetLinearXLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
		Case.Joint->SetLinearYLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
		Case.Joint->SetLinearZLimit(ELinearConstraintMotion::LCM_Locked, 0.0f);
		Case.Joint->SetAngularTwistLimit(Case.Coordinate == EUERLJointCoordinate::Twist
			? EAngularConstraintMotion::ACM_Free : EAngularConstraintMotion::ACM_Locked, 0.0f);
		Case.Joint->SetAngularSwing1Limit(Case.Coordinate == EUERLJointCoordinate::Swing1
			? EAngularConstraintMotion::ACM_Free : EAngularConstraintMotion::ACM_Locked, 0.0f);
		Case.Joint->SetAngularSwing2Limit(Case.Coordinate == EUERLJointCoordinate::Swing2
			? EAngularConstraintMotion::ACM_Free : EAngularConstraintMotion::ACM_Locked, 0.0f);
		Owner->AddInstanceComponent(Case.Joint);
		Case.Joint->RegisterComponent();
		// Robot constraints bind the child body as Frame1 and the parent as Frame2.
		Case.Joint->SetConstrainedComponents(Case.Child, NAME_None, Case.Parent, NAME_None);
		Case.Child->SetEnableGravity(false);
		Case.Child->SetAngularDamping(0.0f);
		Case.Child->WakeAllRigidBodies();

		FUERLActuatorConfig Actuator;
		Actuator.CoordinateType = TEXT("revolute");
		Actuator.Coordinate = UERLJointCoordinateName(Case.Coordinate);
		Actuator.TargetMode = TEXT("position");
		Actuator.Stiffness = 25.0;
		Actuator.Damping = 0.5;
		Actuator.EffortLimit = 2.8;
		ConfigureGenericRobotRevoluteAngularDrive(Case.Joint->ConstraintInstance, Actuator);
		bool bTwistDrive = false;
		bool bSwingDrive = false;
		Case.Joint->ConstraintInstance.GetOrientationDriveTwistAndSwing(bTwistDrive, bSwingDrive);
		const bool bExpectTwist = Case.Coordinate == EUERLJointCoordinate::Twist;
		TestEqual(TEXT("twist position drive follows the joint coordinate"), bTwistDrive, bExpectTwist);
		TestEqual(TEXT("swing position drive follows the joint coordinate"), bSwingDrive, !bExpectTwist);
	}
	PreviewScene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-INT-ROBOT-DRIVE-001: a revolute position drive enables twist or swing to match the unlocked coordinate"));
	return true;
}

namespace UERLGenericRobotDriveTestsPrivate
{
	TUniquePtr<FPreviewScene> MakePhysicsPreviewScene()
	{
		return MakeUnique<FPreviewScene>(
			FPreviewScene::ConstructionValues()
				.SetCreateDefaultLighting(false)
				.SetCreatePhysicsScene(true)
				.ShouldSimulatePhysics(true)
				.SetTransactional(false)
				.SetEditor(false));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotDeployedEffortSolverStepTest,
	"UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_005.DeployedEffortActuatorMatchesTrainingPerSolverStep",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotDeployedEffortSolverStepTest::RunTest(const FString& Parameters)
{
	using namespace UERLGenericRobotTestsPrivate;
	using namespace UERLGenericRobotDriveTestsPrivate;
	// The real CartPole cart: zero stiffness makes it an effort actuator.
	constexpr double CartDamping = 1.0;
	constexpr double CartEffortLimit = 100.0;
	constexpr float CartEffort = 20.0f;
	constexpr int32 SolverSteps = 9;
	const FVector Origin(0.0, 0.0, 200.0);

	TSharedRef<IUERLRobotFactory> Factory = UERLGenericRobot::MakeFactory();
	FUERLProviderConfig Input;
	Input.AssetPath = CartPoleRobotAsset;
	FUERLProviderConfig Effective;
	FString Error;
	if (!Factory->ValidateConfig(Input, Effective, Error))
	{
		AddError(Error);
		return false;
	}
	const int32 CartJointIndex = Factory->Describe().Topology.Joints.IndexOfByPredicate(
		[](const FUERLJointTopology& Joint) { return Joint.CoordinateType == EUERLJointCoordinateType::Prismatic; });
	if (CartJointIndex == INDEX_NONE)
	{
		AddError(TEXT("CartPole has no prismatic cart joint"));
		return false;
	}
	const FName CartJointName = Factory->Describe().Topology.Joints[CartJointIndex].Name;
	Input.Actuators.Add(FUERLActuatorConfig{
		0, CartJointIndex, CartJointName, TEXT("prismatic"), TEXT("linear_x"), TEXT("N"), TEXT("effort"),
		0.0, CartDamping, CartEffortLimit, 0.0 });
	if (!Factory->ValidateConfig(Input, Effective, Error))
	{
		AddError(Error);
		return false;
	}
	const FUERLRobotDescriptor& Descriptor = Factory->Describe();
	TArray<FUERLFieldDescriptor> CartFields;
	for (const EUERLObservationType Type : { EUERLObservationType::JointPosition, EUERLObservationType::JointVelocity })
	{
		const FUERLFieldDescriptor* Field = Descriptor.StateFields.FindByPredicate(
			[CartJointIndex, Type](const FUERLFieldDescriptor& Candidate)
			{
				return Candidate.Observation.JointIndex == CartJointIndex && Candidate.Observation.Type == Type;
			});
		if (!Field)
		{
			AddError(TEXT("CartPole cart joint State is unavailable"));
			return false;
		}
		CartFields.Add(*Field);
	}
	FUERLBatchSchema Schema;
	Schema.ActionFields.Add(FUERLBatchFieldBinding{ Descriptor.ActionFields[0], 0 });
	Schema.ActionWidth = 1;
	Schema.StateFields.Add(FUERLBatchFieldBinding{ CartFields[0], 0 });
	Schema.StateFields.Add(FUERLBatchFieldBinding{ CartFields[1], 1 });
	Schema.StateWidth = 2;
	FUERLBatchBinding Binding;
	if (!Binding.Compile(Schema, Descriptor.ActionFields, Descriptor.StateFields, Error))
	{
		AddError(Error);
		return false;
	}

	// Training reference: the Worker re-applies the command before every lockstep frame.
	float TrainingPosition = 0.0f;
	float TrainingVelocity = 0.0f;
	{
		FFiveMillisecondPhysicsGuard Lockstep(false);
		TUniquePtr<FPreviewScene> PreviewScene = MakePhysicsPreviewScene();
		UWorld* World = PreviewScene->GetWorld();
		TUniquePtr<IUERLRobot> Robot = Factory->Create(Effective);
		FUERLSlotCollisionPlan CollisionPlan;
		TArray<FUERLSlotContext> Slots = { FUERLSlotContext{ 0, Origin, 0.0, FVector::UpVector, {} } };
		if (!Robot->PrepareState(CartFields, Error)
			|| !CollisionPlan.Compile(1, EUERLEnvironmentCollisionScope::SharedWorld, Error))
		{
			AddError(Error);
			return false;
		}
		Slots[0].CollisionProfile = CollisionPlan.Profile(0);
		FUERLResetBatch Reset;
		Reset.Rows.Add(FUERLResetRow{ 0, 0, Origin, 0.0, FVector::UpVector, {} });
		if (!Robot->SpawnIntoSlots(*World, Slots, Error) || !Robot->ResetSlots(Reset, Error))
		{
			AddError(Error);
			return false;
		}
		TestTrue(TEXT("the training robot asks its host to re-apply effort every frame"),
			Robot->RequiresCommandsEveryPhysicsFrame());
		const float ActionData[] = { CartEffort };
		const FUERLNamedActionReader Reader(Binding, { ActionData, 1, 1 });
		for (int32 Step = 0; Step < SolverSteps; ++Step)
		{
			TestTrue(TEXT("training command applies"), Robot->ApplyCommands(Reader, Error));
			World->Tick(ELevelTick::LEVELTICK_All, 0.005f);
			++GFrameCounter;
		}
		float StateData[2] = {};
		FUERLNamedStateWriter Writer(Binding, { StateData, 1, 2 });
		TestTrue(TEXT("training State collects"), Robot->TryCollectState({ 0 }, Writer, Error));
		TrainingPosition = StateData[0];
		TrainingVelocity = StateData[1];
		Robot->DestroySlots();
	}

	// Deployment: one command, then three game frames of three synchronous substeps each.
	FFiveMillisecondPhysicsGuard Substepped(true);
	TUniquePtr<FPreviewScene> PreviewScene = MakePhysicsPreviewScene();
	UWorld* World = PreviewScene->GetWorld();
	FUERLSkeletalMeshRobotRuntimeConfig Config;
	Config.AssetPath = CartPoleRobotAsset;
	Config.bClaimAuthoredActor = false;
	Config.GroundOrigin = Origin;
	FUERLSkeletalMeshRuntimeActuator& Cart = Config.Actuators.AddDefaulted_GetRef();
	Cart.JointName = CartJointName;
	Cart.Damping = CartDamping;
	Cart.EffortLimit = CartEffortLimit;
	Config.Observations.Add({ EUERLObservationType::JointPosition, CartJointName });
	Config.Observations.Add({ EUERLObservationType::JointVelocity, CartJointName });
	FUERLSkeletalMeshRobotRuntime Runtime;
	if (!Runtime.Initialize(*World, Config, Error))
	{
		AddError(FString::Printf(TEXT("zero-stiffness CartPole deployment runtime failed: %s"), *Error));
		return false;
	}
	const float Targets[] = { CartEffort };
	TestTrue(TEXT("deployment effort target applies once"), Runtime.ApplyActuatorTargets(Targets, Error));
	for (int32 Frame = 0; Frame < SolverSteps / 3; ++Frame)
	{
		World->Tick(ELevelTick::LEVELTICK_All, 0.015f);
		++GFrameCounter;
	}
	TArray<float> State;
	TestTrue(TEXT("deployment State collects"), Runtime.CollectState(State, Error) && State.Num() == 2);
	if (State.Num() == 2)
	{
		AddInfo(FString::Printf(
			TEXT("[VERIFY] cart after %d solver steps: training position=%.6f velocity=%.6f deployment position=%.6f velocity=%.6f"),
			SolverSteps, TrainingPosition, TrainingVelocity, State[0], State[1]));
		TestTrue(TEXT("the training reference accelerates the cart"), TrainingVelocity > 0.05f);
		TestTrue(TEXT("deployment holds the effort for every substep like training"),
			FMath::IsNearlyEqual(State[1], TrainingVelocity, 0.01f * TrainingVelocity));
		TestTrue(TEXT("deployment cart displacement matches training"),
			FMath::IsNearlyEqual(State[0], TrainingPosition, 0.01f * FMath::Abs(TrainingPosition) + 1.0e-5f));
	}
	Runtime.Reset();
	PreviewScene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-E2E-ROBOT-CONTENT-005: a zero-stiffness deployed actuator starts and re-applies its effort every Chaos substep"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLDeploymentOverheadGroundTest,
	"UERL.Integration.Policy.Ground.AC_UE_INT_POLICY_GROUND_006.OverheadWorldStaticIsNotGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLDeploymentOverheadGroundTest::RunTest(const FString& Parameters)
{
	using namespace UERLGenericRobotTestsPrivate;
	using namespace UERLGenericRobotDriveTestsPrivate;
	TUniquePtr<FPreviewScene> PreviewScene = MakePhysicsPreviewScene();
	UWorld* World = PreviewScene->GetWorld();
	AActor* Ground = SpawnGround(*World);
	TestNotNull(TEXT("overhead-test WorldStatic ground created"), Ground);
	if (!Ground)
	{
		return false;
	}
	FUERLSkeletalMeshRobotRuntimeConfig Config = PhantomXRuntimeConfig(FVector(0.0, 0.0, -25.0));
	Config.Observations.Add({ EUERLObservationType::TerrainHeight, FName(TEXT("base_link")) });
	Config.Observations.Add({ EUERLObservationType::GroundClearance, FName(TEXT("base_link")) });
	FUERLSkeletalMeshRobotRuntime Runtime;
	FString Error;
	if (!Runtime.Initialize(*World, Config, Error))
	{
		AddError(FString::Printf(TEXT("PhantomX runtime initialization failed: %s"), *Error));
		return false;
	}
	TArray<float> OpenSky;
	FTransform StartPose;
	if (!Runtime.CollectState(OpenSky, Error) || !Runtime.GetPrimaryActorTransform(StartPose, Error)
		|| OpenSky.Num() != UERLTerrainHeightScanWidth + 1)
	{
		AddError(Error);
		return false;
	}

	// A 10 cm slab 2.5 m above the root, like a ceiling over an indoor floor.
	AActor* Ceiling = SpawnWorldStaticBox(
		*World, StartPose.GetLocation() + FVector(0.0, 0.0, 250.0), FVector(5.0, 5.0, 0.1));
	TestNotNull(TEXT("overhead WorldStatic slab created"), Ceiling);
	TArray<float> UnderCeiling;
	if (!Ceiling || !Runtime.CollectState(UnderCeiling, Error))
	{
		AddError(Error);
		return false;
	}
	if (!TestEqual(TEXT("overhead observation preserves all 36 channels"), UnderCeiling.Num(), 36))
	{
		return false;
	}
	bool bSameObservation = UnderCeiling.Num() == OpenSky.Num();
	for (int32 Index = 0; bSameObservation && Index < OpenSky.Num(); ++Index)
	{
		bSameObservation = FMath::IsNearlyEqual(UnderCeiling[Index], OpenSky[Index], 1.0e-4f);
	}
	// State order follows Config.Observations: 35 scan values (index 12 is under the root), then clearance.
	AddInfo(FString::Printf(TEXT("[VERIFY] root clearance open=%.4f under ceiling=%.4f; scan under root open=%.4f under ceiling=%.4f"),
		OpenSky.Last(), UnderCeiling.Last(), OpenSky[12], UnderCeiling[12]));
	TestTrue(TEXT("terrain scan and clearance ignore WorldStatic geometry overhead"), bSameObservation);

	TestTrue(TEXT("reference-pose reset succeeds under the slab"), Runtime.ResetToReferencePose(Error));
	FTransform ResetPose;
	TestTrue(TEXT("reset pose is readable"), Runtime.GetPrimaryActorTransform(ResetPose, Error));
	AddInfo(FString::Printf(TEXT("[VERIFY] root Z start=%.2f after reset=%.2f"),
		StartPose.GetLocation().Z, ResetPose.GetLocation().Z));
	TestTrue(TEXT("reset snaps to the floor, not onto the slab"),
		FMath::Abs(ResetPose.GetLocation().Z - StartPose.GetLocation().Z) < 2.0);

	Runtime.Reset();
	PreviewScene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-INT-POLICY-GROUND-006: deployment terrain, clearance and pose reset stay on the floor under a ceiling"));
	return true;
}

#endif
