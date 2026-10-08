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

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	constexpr TCHAR CartPoleRobotAsset[] = TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole");
	constexpr TCHAR PhantomXRobotAsset[] = TEXT("/Game/Robots/PhantomX/SK_PhantomX.SK_PhantomX");

	FUERLBatchSchema SelectAllFields(const FUERLRobotDescriptor& Descriptor)
	{
		FUERLBatchSchema Schema;
		for (const FUERLFieldDescriptor& Field : Descriptor.ActionFields)
		{
			FUERLBatchFieldBinding& Binding = Schema.ActionFields.AddDefaulted_GetRef();
			Binding.Field = Field;
			Binding.Column = Schema.ActionWidth;
			Schema.ActionWidth += Field.Width;
		}
		for (const FUERLFieldDescriptor& Field : Descriptor.StateFields)
		{
			FUERLBatchFieldBinding& Binding = Schema.StateFields.AddDefaulted_GetRef();
			Binding.Field = Field;
			Binding.Column = Schema.StateWidth;
			Schema.StateWidth += Field.Width;
		}
		return Schema;
	}

	TArray<FUERLFieldDescriptor> FieldsFromSchema(const FUERLBatchSchema& Schema)
	{
		TArray<FUERLFieldDescriptor> Fields;
		Fields.Reserve(Schema.StateFields.Num());
		for (const FUERLBatchFieldBinding& Binding : Schema.StateFields)
		{
			Fields.Add(Binding.Field);
		}
		return Fields;
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
		Component->SetCollisionResponseToAllChannels(ECR_Block);
		Component->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Component->SetWorldScale3D(FVector(10.0, 10.0, 0.5));
		return Ground;
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

	/** PhantomX position actuators with the Python asset gains and reference pose. */
	FUERLSkeletalMeshRobotRuntimeConfig PhantomXRuntimeConfig(const FVector& GroundOrigin)
	{
		FUERLSkeletalMeshRobotRuntimeConfig Config;
		Config.AssetPath = PhantomXRobotAsset;
		Config.bClaimAuthoredActor = false;
		Config.GroundOrigin = GroundOrigin;
		Config.InitialRootHeightMeters = 0.18;
		for (const TCHAR* Leg : { TEXT("rf"), TEXT("rm"), TEXT("rr"), TEXT("lf"), TEXT("lm"), TEXT("lr") })
		{
			for (const TPair<const TCHAR*, double>& Segment : {
				TPair<const TCHAR*, double>(TEXT("c1"), 0.0),
				TPair<const TCHAR*, double>(TEXT("thigh"), 0.15),
				TPair<const TCHAR*, double>(TEXT("tibia"), -0.30) })
			{
				FUERLSkeletalMeshRuntimeActuator& Actuator = Config.Actuators.AddDefaulted_GetRef();
				Actuator.JointName = FName(*FString::Printf(TEXT("%s_%s"), Segment.Key, Leg));
				Actuator.Stiffness = 25.0;
				Actuator.Damping = 0.5;
				Actuator.EffortLimit = 2.8;
				Actuator.DefaultPosition = Segment.Value;
			}
		}
		return Config;
	}

	USkeletalMeshComponent* FindSpawnedRobotMesh(UWorld& World)
	{
		for (TActorIterator<AActor> It(&World); It; ++It)
		{
			USkeletalMeshComponent* Candidate = It->FindComponentByClass<USkeletalMeshComponent>();
			if (Candidate && Candidate->GetFName() == FName(TEXT("RobotSkeletalMesh")))
			{
				return Candidate;
			}
		}
		return nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotAuthoredConfigTest,
	"UERL.Unit.Robot.GenericSkeletalMesh.AuthoredActorConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotAuthoredConfigTest::RunTest(const FString& Parameters)
{
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLRobot"));
	FString Error;
	const TSharedPtr<IUERLRobotFactory> Factory = FUERLRobotRegistry::Get().Resolve(
		UERLGenericRobot::RobotId, Error);
	TestTrue(TEXT("generic factory is registered"), Factory.IsValid());
	if (!Factory)
	{
		return false;
	}

	FUERLProviderConfig Input;
	Input.AssetPath = CartPoleRobotAsset;
	Input.Scalars.Add(UERLGenericRobot::ClaimAuthoredActor, 1.0);
	FUERLProviderConfig Effective;
	TestTrue(TEXT("generic Robot accepts the explicit authored-Actor mode"),
		Factory->ValidateConfig(Input, Effective, Error));
	Input.Scalars.Add(TEXT("robot.unowned"), 1.0);
	TestFalse(TEXT("generic Robot rejects unknown provider scalars"),
		Factory->ValidateConfig(Input, Effective, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotCartPoleContentSmokeTest,
	"UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_001.CartPoleContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotCartPoleContentSmokeTest::RunTest(const FString& Parameters)
{
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLRobot"));
	FString Error;
	const TSharedPtr<IUERLRobotFactory> Factory = FUERLRobotRegistry::Get().Resolve(
		UERLGenericRobot::RobotId, Error);
	TestTrue(TEXT("generic factory is registered"), Factory.IsValid());
	if (!Factory.IsValid())
	{
		return false;
	}

	FUERLProviderConfig Input;
	Input.AssetPath = CartPoleRobotAsset;
	FUERLProviderConfig Effective;
	TestTrue(TEXT("CartPole is a loadable scalar-joint SkeletalMesh with PhysicsAsset"),
		Factory->ValidateConfig(Input, Effective, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("CartPole validation diagnostic: %s"), *Error));
	}
	if (!Error.IsEmpty() || Factory->Describe().Topology.BodyNames.Num() == 0)
	{
		return false;
	}
	const FUERLRobotTopology& InitialTopology = Factory->Describe().Topology;
	const int32 CartBodyIndex = InitialTopology.BodyNames.IndexOfByKey(FName(TEXT("cart")));
	const int32 CartJointIndex = InitialTopology.Joints.IndexOfByPredicate(
		[CartBodyIndex](const FUERLJointTopology& Joint)
		{
			return Joint.ChildBodyIndex == CartBodyIndex
				&& Joint.CoordinateType == EUERLJointCoordinateType::Prismatic;
		});
	TestTrue(TEXT("CartPole exposes the cart prismatic joint"), CartJointIndex != INDEX_NONE);
	if (CartJointIndex == INDEX_NONE)
	{
		return false;
	}
	const FUERLJointTopology CartJoint = InitialTopology.Joints[CartJointIndex];
	Input.Actuators.Add(FUERLActuatorConfig{
		0, CartJointIndex, CartJoint.Name, TEXT("prismatic"), TEXT("linear_x"), TEXT("N"), TEXT("effort"),
		0.0, 1.0, 100.0, 0.0 });
	Error.Reset();
	TestTrue(TEXT("CartPole cart actuator validates"), Factory->ValidateConfig(Input, Effective, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
		return false;
	}

	const FUERLRobotDescriptor& Descriptor = Factory->Describe();
	const FUERLRobotTopology& Topology = Descriptor.Topology;
	TestTrue(TEXT("real asset publishes reflected bodies"), Descriptor.Topology.BodyNames.Num() > 0);
	TestTrue(TEXT("real asset publishes State descriptors"), Descriptor.StateFields.Num() > 0);
	TestTrue(TEXT("real asset publishes contact descriptors"), Descriptor.StateFields.ContainsByPredicate(
		[](const FUERLFieldDescriptor& Field)
		{
			return Field.Observation.Type == EUERLObservationType::Contact;
		}));

	const FUERLBatchSchema Schema = SelectAllFields(Descriptor);
	FUERLBatchBinding Binding;
	TestTrue(TEXT("real descriptor set compiles into a selected batch"),
		Binding.Compile(Schema, Descriptor.ActionFields, Descriptor.StateFields, Error));
	if (!Binding.IsCompiled())
	{
		return false;
	}

	TUniquePtr<IUERLRobot> Robot = Factory->Create(Effective);
	TestTrue(TEXT("prismatic effort commands are applied every physics frame"),
		Robot->RequiresCommandsEveryPhysicsFrame());
	const TArray<FUERLFieldDescriptor> SelectedStateFields = FieldsFromSchema(Schema);
	TestTrue(TEXT("selected real State plan prepares before spawn"),
		Robot->PrepareState(SelectedStateFields, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("CartPole selected plan diagnostic: %s"), *Error));
	}
	if (!Error.IsEmpty())
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
	TestNotNull(TEXT("real content smoke world created"), World);
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

	// Keep the Robot within the 2 m ground-clearance probe.  SpawnGround places a
	// 50 cm tall cube centered at Z=-50, so its top face is near Z=-25.
	const FVector SlotOrigin(0.0, 0.0, 50.0);
	TArray<FUERLSlotContext> Slots = {
		FUERLSlotContext{ 0, SlotOrigin, 0.0, FVector::UpVector, { Ground } },
	};
	FUERLSlotCollisionPlan CollisionPlan;
	TestTrue(TEXT("real content Slot collision profile compiles"), CollisionPlan.Compile(
		1, EUERLEnvironmentCollisionScope::SlotIsolated, Error));
	Slots[0].CollisionProfile = CollisionPlan.Profile(0);
	UStaticMeshComponent* GroundMesh = Ground->FindComponentByClass<UStaticMeshComponent>();
	if (!GroundMesh)
	{
		Ground->Destroy();
		return false;
	}
	Slots[0].CollisionProfile.ApplySlotEnvironment(*GroundMesh);
	TestTrue(TEXT("real CartPole spawns into the physics world"),
		Robot->SpawnIntoSlots(*World, Slots, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(FString::Printf(TEXT("CartPole spawn diagnostic: %s"), *Error));
	}
	if (!Error.IsEmpty())
	{
		Robot->DestroySlots();
		Ground->Destroy();
		return false;
	}
	FUERLResetBatch Reset;
	Reset.Rows.Add(FUERLResetRow{ 0, 0, SlotOrigin, 0.0, FVector::UpVector, {} });
	TestTrue(TEXT("real CartPole resets into the slot"), Robot->ResetSlots(Reset, Error));

	TArray<float> StateData;
	StateData.SetNumZeroed(Schema.StateWidth);
	FUERLNamedStateWriter Writer(Binding, { StateData.GetData(), 1, Schema.StateWidth });
	const TArray<int32> SlotIds = { 0 };
	Robot->CollectState(SlotIds, Writer);
	TestTrue(TEXT("real CartPole validates after initial collect"),
		Robot->ValidateSlot(0, Error) == EUERLSlotFaultCode::None);
	TArray<FName> ContactFields;
	for (const FUERLFieldDescriptor& Field : Descriptor.StateFields)
	{
		if (Field.Observation.Type == EUERLObservationType::Contact)
		{
			ContactFields.Add(Field.Name);
		}
	}
	TestTrue(TEXT("real content has at least one body contact field"), ContactFields.Num() > 0);
	for (const FName ContactField : ContactFields)
	{
		const FUERLBatchFieldBinding* FieldBinding = Binding.FindState(ContactField);
		TestNotNull(TEXT("contact field has a selected column"), FieldBinding);
		if (FieldBinding)
		{
			TestEqual(TEXT("contact is clear after reset"), StateData[FieldBinding->Column], 0.0f);
		}
	}

	USkeletalMeshComponent* RobotComponent = nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		if (USkeletalMeshComponent* Candidate = It->FindComponentByClass<USkeletalMeshComponent>())
		{
			RobotComponent = Candidate;
			break;
		}
	}
	TestNotNull(TEXT("spawned Robot component is available for hit dispatch"), RobotComponent);
	if (!RobotComponent)
	{
		Robot->DestroySlots();
		Ground->Destroy();
		return false;
	}
	FHitResult BlockingGroundHit;
	BlockingGroundHit.bBlockingHit = true;
	BlockingGroundHit.MyBoneName = FName(TEXT("pole"));
	AActor* OtherSlotGround = SpawnGround(*World);
	TestNotNull(TEXT("another Slot's WorldStatic ground created"), OtherSlotGround);
	if (!OtherSlotGround)
	{
		Robot->DestroySlots();
		Ground->Destroy();
		return false;
	}
	RobotComponent->OnComponentHit.Broadcast(
		RobotComponent, OtherSlotGround, OtherSlotGround->FindComponentByClass<UStaticMeshComponent>(),
		FVector::ZeroVector, BlockingGroundHit);
	StateData.SetNumZeroed(Schema.StateWidth);
	Robot->CollectState(SlotIds, Writer);
	for (const FName ContactField : ContactFields)
	{
		const FUERLBatchFieldBinding* FieldBinding = Binding.FindState(ContactField);
		if (FieldBinding)
		{
			TestEqual(TEXT("another Slot's WorldStatic hit is excluded"), StateData[FieldBinding->Column], 0.0f);
		}
	}

	RobotComponent->OnComponentHit.Broadcast(
		RobotComponent, Ground, Ground->FindComponentByClass<UStaticMeshComponent>(),
		FVector::ZeroVector, BlockingGroundHit);
	StateData.SetNumZeroed(Schema.StateWidth);
	Robot->CollectState(SlotIds, Writer);
	bool bContactObserved = false;
	for (const FName ContactField : ContactFields)
	{
		const FUERLBatchFieldBinding* FieldBinding = Binding.FindState(ContactField);
		if (FieldBinding && StateData[FieldBinding->Column] > 0.5f)
		{
			bContactObserved = true;
		}
	}
	TestFalse(TEXT("a body-ground hit event alone does not publish terminal support"), bContactObserved);

	StateData.SetNumZeroed(Schema.StateWidth);
	Robot->CollectState(SlotIds, Writer);
	for (const FName ContactField : ContactFields)
	{
		const FUERLBatchFieldBinding* FieldBinding = Binding.FindState(ContactField);
		if (FieldBinding)
		{
			TestEqual(TEXT("contact event is cleared at the next State boundary"), StateData[FieldBinding->Column], 0.0f);
		}
	}

	const FUERLBatchFieldBinding* CartPositionBinding = Schema.StateFields.FindByPredicate(
		[CartJointIndex](const FUERLBatchFieldBinding& Field)
		{
			return Field.Field.Observation.Type == EUERLObservationType::JointPosition
				&& Field.Field.Observation.JointIndex == CartJointIndex;
		});
	const FUERLBatchFieldBinding* CartVelocityBinding = Schema.StateFields.FindByPredicate(
		[CartJointIndex](const FUERLBatchFieldBinding& Field)
		{
			return Field.Field.Observation.Type == EUERLObservationType::JointVelocity
				&& Field.Field.Observation.JointIndex == CartJointIndex;
		});
	FBodyInstance* CartBody = RobotComponent->GetBodyInstance(FName(TEXT("cart")));
	FBodyInstance* CartParentBody = RobotComponent->GetBodyInstance(
		Topology.BodyNames[CartJoint.ParentBodyIndex]);
	TestNotNull(TEXT("cart position State is selected"), CartPositionBinding);
	TestNotNull(TEXT("cart velocity State is selected"), CartVelocityBinding);
	TestNotNull(TEXT("spawned cart body is available"), CartBody);
	TestNotNull(TEXT("spawned cart parent body is available"), CartParentBody);
	if (!CartPositionBinding || !CartVelocityBinding || !CartBody || !CartParentBody)
	{
		Robot->DestroySlots();
		OtherSlotGround->Destroy();
		Ground->Destroy();
		return false;
	}
	const float ActionData[] = { 100.0f };
	FUERLNamedActionReader Reader(Binding, { ActionData, 1, Schema.ActionWidth });
	TestTrue(TEXT("100-newton cart command applies"), Robot->ApplyCommands(Reader, Error));
	constexpr float PhysicsDt = 1.0f / 120.0f;
	World->Tick(ELevelTick::LEVELTICK_All, PhysicsDt);
	StateData.SetNumZeroed(Schema.StateWidth);
	Robot->CollectState(SlotIds, Writer);
	const float CartPosition = StateData[CartPositionBinding->Column];
	const float CartVelocity = StateData[CartVelocityBinding->Column];
	AddInfo(FString::Printf(
		TEXT("cart mass=%.6f parent_mass=%.6f position=%.9f velocity=%.9f"),
		CartBody->GetBodyMass(), CartParentBody->GetBodyMass(), CartPosition, CartVelocity));
	TestTrue(TEXT("100 newtons produces observable constrained cart velocity"), CartVelocity > 1.0e-3f);

	Robot->DestroySlots();
	OtherSlotGround->Destroy();
	Ground->Destroy();
	PreviewScene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-E2E-ROBOT-CONTENT-001: CartPole asset, provider spawn, body State, and contact lifecycle"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotRevolutePhysicsTest,
	"UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_002.RevolutePhysics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotRevolutePhysicsTest::RunTest(const FString& Parameters)
{
	constexpr TCHAR CartPoleAsset[] = TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole");
	TSharedRef<IUERLRobotFactory> Factory = UERLGenericRobot::MakeFactory();
	FUERLProviderConfig Discovery;
	Discovery.AssetPath = CartPoleAsset;
	FUERLProviderConfig Effective;
	FString Error;
	TestTrue(TEXT("CartPole topology is reflected"), Factory->ValidateConfig(Discovery, Effective, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
		return false;
	}

	const FUERLRobotTopology& Topology = Factory->Describe().Topology;
	const int32 PoleBodyIndex = Topology.BodyNames.IndexOfByKey(FName(TEXT("pole")));
	const int32 CartBodyIndex = Topology.BodyNames.IndexOfByKey(FName(TEXT("cart")));
	const int32 PoleJointIndex = Topology.Joints.IndexOfByPredicate(
		[&Topology, PoleBodyIndex](const FUERLJointTopology& Joint)
		{
			return Joint.ChildBodyIndex == PoleBodyIndex
				&& Joint.CoordinateType == EUERLJointCoordinateType::Revolute;
		});
	const int32 CartJointIndex = Topology.Joints.IndexOfByPredicate(
		[CartBodyIndex](const FUERLJointTopology& Joint)
		{
			return Joint.ChildBodyIndex == CartBodyIndex
				&& Joint.CoordinateType == EUERLJointCoordinateType::Prismatic;
		});
	TestTrue(TEXT("CartPole exposes the pole revolute joint"), PoleJointIndex != INDEX_NONE);
	TestTrue(TEXT("CartPole exposes the cart prismatic joint"), CartJointIndex != INDEX_NONE);
	if (PoleJointIndex == INDEX_NONE || CartJointIndex == INDEX_NONE)
	{
		return false;
	}
	const FUERLJointTopology PoleJoint = Topology.Joints[PoleJointIndex];
	FUERLProviderConfig PositionInput;
	PositionInput.AssetPath = CartPoleAsset;
	PositionInput.Actuators.Add(FUERLActuatorConfig{
		0, PoleJointIndex, PoleJoint.Name, TEXT("revolute"), TEXT("twist"), TEXT("rad"), TEXT("position"),
		25.0, 0.5, 10.0, 0.0 });
	FUERLProviderConfig PositionEffective;
	Error.Reset();
	TestTrue(TEXT("revolute position actuator validates"),
		Factory->ValidateConfig(PositionInput, PositionEffective, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
		return false;
	}
	TUniquePtr<IUERLRobot> PositionRobot = Factory->Create(PositionEffective);
	TestFalse(TEXT("revolute position target is retained by the constraint drive"),
		PositionRobot->RequiresCommandsEveryPhysicsFrame());

	FUERLProviderConfig Input;
	Input.AssetPath = CartPoleAsset;
	Input.Actuators.Add(FUERLActuatorConfig{
		0, PoleJointIndex, PoleJoint.Name, TEXT("revolute"), TEXT("twist"), TEXT("N*m"), TEXT("effort"),
		0.0, 0.001, 10.0, 0.0 });
	Input.ResetBindings.Add(FUERLResetBinding{
		0, FName(TEXT("test.pole.position")), TEXT("joint_position"), PoleJointIndex,
		INDEX_NONE, 0, TEXT("twist"), TEXT("rad") });
	Input.ResetBindings.Add(FUERLResetBinding{
		1, FName(TEXT("test.pole.velocity")), TEXT("joint_velocity"), PoleJointIndex,
		INDEX_NONE, 0, TEXT("twist"), TEXT("rad/s") });
	Error.Reset();
	TestTrue(TEXT("pole effort and reset bindings validate"), Factory->ValidateConfig(Input, Effective, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
		return false;
	}

	const FUERLRobotDescriptor& Descriptor = Factory->Describe();
	const FUERLFieldDescriptor* PolePosition = Descriptor.StateFields.FindByPredicate(
		[PoleJointIndex](const FUERLFieldDescriptor& Field)
		{
			return Field.Observation.JointIndex == PoleJointIndex
				&& Field.Observation.Type == EUERLObservationType::JointPosition;
		});
	const FUERLFieldDescriptor* PoleVelocity = Descriptor.StateFields.FindByPredicate(
		[PoleJointIndex](const FUERLFieldDescriptor& Field)
		{
			return Field.Observation.JointIndex == PoleJointIndex
				&& Field.Observation.Type == EUERLObservationType::JointVelocity;
		});
	const FUERLFieldDescriptor* PoleContact = Descriptor.StateFields.FindByPredicate(
		[PoleBodyIndex](const FUERLFieldDescriptor& Field)
		{
			return Field.Observation.BodyIndex == PoleBodyIndex
				&& Field.Observation.Type == EUERLObservationType::Contact;
		});
	TestNotNull(TEXT("pole position State is published"), PolePosition);
	TestNotNull(TEXT("pole velocity State is published"), PoleVelocity);
	TestNotNull(TEXT("pole contact State is published"), PoleContact);
	if (!PolePosition || !PoleVelocity || !PoleContact || Descriptor.ActionFields.Num() != 1)
	{
		return false;
	}

	FUERLBatchSchema Schema;
	Schema.ActionFields.Add(FUERLBatchFieldBinding{ Descriptor.ActionFields[0], 0 });
	Schema.ActionWidth = Descriptor.ActionFields[0].Width;
	Schema.StateFields.Add(FUERLBatchFieldBinding{ *PolePosition, 0 });
	Schema.StateFields.Add(FUERLBatchFieldBinding{ *PoleVelocity, PolePosition->Width });
	Schema.StateFields.Add(FUERLBatchFieldBinding{
		*PoleContact, PolePosition->Width + PoleVelocity->Width });
	Schema.StateWidth = PolePosition->Width + PoleVelocity->Width + PoleContact->Width;
	FUERLBatchBinding Binding;
	TestTrue(TEXT("pole Action and State schema compiles"), Binding.Compile(
		Schema, Descriptor.ActionFields, Descriptor.StateFields, Error));

	TUniquePtr<IUERLRobot> Robot = Factory->Create(Effective);
	TestTrue(TEXT("revolute effort commands are applied every physics frame"),
		Robot->RequiresCommandsEveryPhysicsFrame());
	const TArray<FUERLFieldDescriptor> SelectedStateFields = { *PolePosition, *PoleVelocity, *PoleContact };
	TestTrue(TEXT("pole State plan prepares"), Robot->PrepareState(SelectedStateFields, Error));
	TUniquePtr<FPreviewScene> PreviewScene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = PreviewScene->GetWorld();
	TestNotNull(TEXT("revolute physics world created"), World);
	if (!World)
	{
		return false;
	}
	AActor* Environment0 = World->SpawnActor<AActor>();
	AActor* Environment1 = World->SpawnActor<AActor>();
	TestNotNull(TEXT("Slot 0 Environment owner created"), Environment0);
	TestNotNull(TEXT("Slot 1 Environment owner created"), Environment1);
	if (!Environment0 || !Environment1)
	{
		return false;
	}

	const FVector GroundNormal = FVector(0.0, 1.0, 1.0).GetSafeNormal();
	TArray<FUERLSlotContext> Slots = {
		FUERLSlotContext{ 0, FVector(-500.0, 0.0, 500.0), 0.0, GroundNormal, { Environment0 } },
		FUERLSlotContext{ 1, FVector(500.0, 0.0, 500.0), 0.0, GroundNormal, { Environment1 } },
	};
	FUERLSlotCollisionPlan CollisionPlan;
	TestTrue(TEXT("two revolute physics Slot collision profiles compile"), CollisionPlan.Compile(
		2, EUERLEnvironmentCollisionScope::SlotIsolated, Error));
	Slots[0].CollisionProfile = CollisionPlan.Profile(0);
	Slots[1].CollisionProfile = CollisionPlan.Profile(1);
	UBoxComponent* EnvironmentComponent0 = NewObject<UBoxComponent>(Environment0);
	UBoxComponent* EnvironmentComponent1 = NewObject<UBoxComponent>(Environment1);
	Environment0->SetRootComponent(EnvironmentComponent0);
	Environment1->SetRootComponent(EnvironmentComponent1);
	EnvironmentComponent0->SetBoxExtent(FVector(10.0));
	Slots[0].CollisionProfile.ApplySlotEnvironment(*EnvironmentComponent0);
	EnvironmentComponent0->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	EnvironmentComponent0->RegisterComponent();
	EnvironmentComponent1->SetBoxExtent(FVector(10.0));
	Slots[1].CollisionProfile.ApplySlotEnvironment(*EnvironmentComponent1);
	EnvironmentComponent1->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	EnvironmentComponent1->RegisterComponent();
	TestTrue(TEXT("CartPole spawns in a nonidentity Slot frame"), Robot->SpawnIntoSlots(*World, Slots, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
		return false;
	}

	FUERLResetBatch CanonicalReset;
	for (const FUERLSlotContext& Slot : Slots)
	{
		FUERLResetRow& Row = CanonicalReset.Rows.AddDefaulted_GetRef();
		Row.SlotId = Slot.SlotId;
		Row.Origin = Slot.Origin;
		Row.GroundNormal = GroundNormal;
	}
	TestTrue(TEXT("Initialize-style reset restores canonical state without overrides"),
		Robot->ResetSlots(CanonicalReset, Error));
	if (!Error.IsEmpty())
	{
		AddInfo(Error);
		return false;
	}

	FUERLResetBatch Reset;
	for (const FUERLSlotContext& Slot : Slots)
	{
		FUERLResetRow& Row = Reset.Rows.AddDefaulted_GetRef();
		Row.SlotId = Slot.SlotId;
		Row.Origin = Slot.Origin;
		Row.GroundNormal = GroundNormal;
		Row.Values = { 0.2, 0.0 };
	}
	TestTrue(TEXT("positive pole reset applies"), Robot->ResetSlots(Reset, Error));

	TArray<float> StateData;
	StateData.SetNumZeroed(Schema.StateWidth * Slots.Num());
	FUERLNamedStateWriter Writer(Binding, { StateData.GetData(), Slots.Num(), Schema.StateWidth });
	const TArray<int32> SlotIds = { 0, 1 };
	Robot->CollectState(SlotIds, Writer);
	const FUERLBatchFieldBinding* PositionBinding = Binding.FindState(PolePosition->Name);
	const FUERLBatchFieldBinding* VelocityBinding = Binding.FindState(PoleVelocity->Name);
	const FUERLBatchFieldBinding* ContactBinding = Binding.FindState(PoleContact->Name);
	TestNotNull(TEXT("pole position column is selected"), PositionBinding);
	TestNotNull(TEXT("pole velocity column is selected"), VelocityBinding);
	TestNotNull(TEXT("pole contact column is selected"), ContactBinding);
	if (!PositionBinding || !VelocityBinding || !ContactBinding)
	{
		Robot->DestroySlots();
		return false;
	}
	AddInfo(FString::Printf(TEXT("positive pole reset observed position=%.6f"),
		StateData[PositionBinding->Column]));
	TestTrue(TEXT("positive reset is observed in radians with the same sign"),
		FMath::IsNearlyEqual(StateData[PositionBinding->Column], 0.2f, 1.0e-3f)
			&& FMath::IsNearlyEqual(
				StateData[Schema.StateWidth + PositionBinding->Column], 0.2f, 1.0e-3f));

	for (FUERLResetRow& Row : Reset.Rows)
	{
		Row.Values[0] = 0.0;
	}
	TestTrue(TEXT("neutral pole reset applies"), Robot->ResetSlots(Reset, Error));
	TArray<USkeletalMeshComponent*> Components;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		USkeletalMeshComponent* Candidate = It->FindComponentByClass<USkeletalMeshComponent>();
		if (Candidate && Candidate->GetFName() == FName(TEXT("RobotSkeletalMesh")))
		{
			Components.Add(Candidate);
		}
	}
	Components.Sort([](const USkeletalMeshComponent& Left, const USkeletalMeshComponent& Right)
	{
		return Left.GetOwner()->GetActorLocation().X < Right.GetOwner()->GetActorLocation().X;
	});
	TestEqual(TEXT("both spawned Robot components are available"), Components.Num(), 2);
	if (Components.Num() != 2)
	{
		Robot->DestroySlots();
		return false;
	}
	FBodyInstance* PositivePoleBody = Components[0]->GetBodyInstance(FName(TEXT("pole")));
	FBodyInstance* PositiveParentBody = Components[0]->GetBodyInstance(Topology.BodyNames[PoleJoint.ParentBodyIndex]);
	FConstraintInstance* PositiveConstraint = Components[0]->FindConstraintInstance(PoleJoint.Name);
	FBodyInstance* NegativePoleBody = Components[1]->GetBodyInstance(FName(TEXT("pole")));
	FBodyInstance* NegativeParentBody = Components[1]->GetBodyInstance(Topology.BodyNames[PoleJoint.ParentBodyIndex]);
	FConstraintInstance* NegativeConstraint = Components[1]->FindConstraintInstance(PoleJoint.Name);
	if (!PositivePoleBody || !PositiveParentBody || !PositiveConstraint
		|| !NegativePoleBody || !NegativeParentBody || !NegativeConstraint)
	{
		AddError(TEXT("spawned CartPole bodies or constraints are missing"));
		Robot->DestroySlots();
		return false;
	}

	const FTransform InitialPoleBodyWorld = PositivePoleBody->GetUnrealWorldTransform();
	const FTransform InitialParentBodyWorld = PositiveParentBody->GetUnrealWorldTransform();
	const FTransform PositiveSlotFrame(FQuat::FindBetweenNormals(FVector::UpVector, GroundNormal), Slots[0].Origin);
	const FTransform NegativeSlotFrame(FQuat::FindBetweenNormals(FVector::UpVector, GroundNormal), Slots[1].Origin);
	const FTransform PositiveParentInSlot = InitialParentBodyWorld.GetRelativeTransform(PositiveSlotFrame);
	const FTransform NegativeParentInSlot = NegativeParentBody->GetUnrealWorldTransform().GetRelativeTransform(NegativeSlotFrame);
	TestTrue(TEXT("canonical reset leaves both Slot-local cart poses identical"),
		PositiveParentInSlot.Equals(NegativeParentInSlot, 1.0e-3f));
	const FTransform ChildFrame = ComposeGenericConstraintFrameWorld(
		PositiveConstraint->GetRefFrame(EConstraintFrame::Frame1), InitialPoleBodyWorld);
	const FVector AxisWorld = ChildFrame.TransformVectorNoScale(
		GenericJointCoordinateAxis(EUERLJointCoordinate::Twist)).GetSafeNormal();
	const float ActionData[] = { 1.0f, -1.0f };
	FUERLNamedActionReader Reader(Binding, { ActionData, Slots.Num(), Schema.ActionWidth });
	constexpr float PhysicsDt = 1.0f / 120.0f;
	constexpr int32 PhysicsTicks = 1;
	for (int32 Tick = 0; Tick < PhysicsTicks; ++Tick)
	{
		TestTrue(TEXT("opposite pole torques apply"), Robot->ApplyCommands(Reader, Error));
		World->Tick(ELevelTick::LEVELTICK_All, PhysicsDt);
	}

	StateData.SetNumZeroed(Schema.StateWidth * Slots.Num());
	Robot->CollectState(SlotIds, Writer);
	const float PositiveVelocity = StateData[VelocityBinding->Column];
	const float NegativeVelocity = StateData[Schema.StateWidth + VelocityBinding->Column];
	const FTransform PostChildFrame = ComposeGenericConstraintFrameWorld(
		PositiveConstraint->GetRefFrame(EConstraintFrame::Frame1), PositivePoleBody->GetUnrealWorldTransform());
	const FVector PostAxisWorld = PostChildFrame.TransformVectorNoScale(
		GenericJointCoordinateAxis(EUERLJointCoordinate::Twist)).GetSafeNormal();
	const float PositiveChaosVelocity = FVector::DotProduct(
		PositivePoleBody->GetUnrealWorldAngularVelocityInRadians()
			- PositiveParentBody->GetUnrealWorldAngularVelocityInRadians(), PostAxisWorld);
	AddInfo(FString::Printf(TEXT("pole reset position=%.6f velocity observed=%.6f chaos=%.6f"),
		StateData[PositionBinding->Column], PositiveVelocity, PositiveChaosVelocity));
	TestTrue(TEXT("provider State matches the positive-torque Chaos velocity"),
		FMath::IsNearlyEqual(PositiveVelocity, PositiveChaosVelocity, 1.0e-3f));

	const FTransform NegativeChildFrame = ComposeGenericConstraintFrameWorld(
		NegativeConstraint->GetRefFrame(EConstraintFrame::Frame1), NegativePoleBody->GetUnrealWorldTransform());
	const FVector NegativeAxisWorld = NegativeChildFrame.TransformVectorNoScale(
		GenericJointCoordinateAxis(EUERLJointCoordinate::Twist)).GetSafeNormal();
	const float NegativeChaosVelocity = FVector::DotProduct(
		NegativePoleBody->GetUnrealWorldAngularVelocityInRadians()
			- NegativeParentBody->GetUnrealWorldAngularVelocityInRadians(), NegativeAxisWorld);
	AddInfo(FString::Printf(TEXT("negative trial velocity observed=%.6f chaos=%.6f"),
		NegativeVelocity, NegativeChaosVelocity));
	TestTrue(TEXT("provider State matches the negative-torque Chaos velocity"),
		FMath::IsNearlyEqual(NegativeVelocity, NegativeChaosVelocity, 1.0e-3f));
	TestTrue(TEXT("positive torque increases joint velocity relative to negative torque"),
		PositiveVelocity > NegativeVelocity);

	const FVector AxisLocal = InitialPoleBodyWorld.GetRotation().UnrotateVector(AxisWorld);
	const FVector ParentAxisLocal = InitialParentBodyWorld.GetRotation().UnrotateVector(AxisWorld);
	const FVector Inertia = PositivePoleBody->GetBodyInertiaTensor();
	const FVector ParentInertia = PositiveParentBody->GetBodyInertiaTensor();
	const double ExpectedAcceleration = ConvertGenericJointEffortToChaos(
		1.0, EUERLJointCoordinate::Twist)
		* (FMath::Square(AxisLocal.X) / Inertia.X
			+ FMath::Square(AxisLocal.Y) / Inertia.Y
			+ FMath::Square(AxisLocal.Z) / Inertia.Z
			+ FMath::Square(ParentAxisLocal.X) / ParentInertia.X
			+ FMath::Square(ParentAxisLocal.Y) / ParentInertia.Y
			+ FMath::Square(ParentAxisLocal.Z) / ParentInertia.Z);
	const double ObservedAcceleration = (PositiveVelocity - NegativeVelocity)
		/ (2.0 * PhysicsDt * PhysicsTicks);
	AddInfo(FString::Printf(TEXT("pole angular acceleration expected=%.6f observed=%.6f"),
		ExpectedAcceleration, ObservedAcceleration));
	TestTrue(TEXT("one newton-metre produces the expected-order constrained angular acceleration"),
		ObservedAcceleration >= ExpectedAcceleration * 0.1
			&& ObservedAcceleration <= ExpectedAcceleration);

	FTransform DisturbedCart = InitialParentBodyWorld;
	DisturbedCart.AddToTranslation(FVector(75.0, 0.0, 0.0));
	PositiveParentBody->SetBodyTransform(DisturbedCart, ETeleportType::TeleportPhysics, true);
	PositiveParentBody->SetLinearVelocity(FVector(100.0, 0.0, 0.0), false);
	TestTrue(TEXT("partial pole reset applies after passive cart disturbance"), Robot->ResetSlots(Reset, Error));
	const FTransform RestoredCart = PositiveParentBody->GetUnrealWorldTransform();
	TestTrue(TEXT("partial reset restores the undeclared passive cart pose"),
		RestoredCart.Equals(InitialParentBodyWorld, 1.0e-3f));
	TestTrue(TEXT("partial reset restores the undeclared passive cart velocity"),
		PositiveParentBody->GetUnrealWorldVelocity().IsNearlyZero(1.0e-3f));

	FHitResult Slot0Contact;
	Slot0Contact.bBlockingHit = true;
	Slot0Contact.MyBoneName = FName(TEXT("pole"));
	FHitResult Slot1Contact = Slot0Contact;
	Components[0]->OnComponentHit.Broadcast(
		Components[0], Environment0, EnvironmentComponent0, FVector::ZeroVector, Slot0Contact);
	Components[1]->OnComponentHit.Broadcast(
		Components[1], Environment1, EnvironmentComponent1, FVector::ZeroVector, Slot1Contact);
	const FTransform UntouchedSlot1Transform = NegativeParentBody->GetUnrealWorldTransform();
	FUERLResetBatch SparseReset;
	SparseReset.Rows.Add(Reset.Rows[0]);
	TestTrue(TEXT("sparse reset applies to Slot 0"), Robot->ResetSlots(SparseReset, Error));
	TestTrue(TEXT("sparse reset leaves Slot 1 body transform unchanged"),
		NegativeParentBody->GetUnrealWorldTransform().Equals(UntouchedSlot1Transform, 1.0e-3f));
	StateData.SetNumZeroed(Schema.StateWidth * Slots.Num());
	Robot->CollectState(SlotIds, Writer);
	TestEqual(TEXT("sparse reset clears selected Slot contact"),
		StateData[ContactBinding->Column], 0.0f);
	TestEqual(TEXT("sparse reset clears unselected Slot terminal support without a physics sample"),
		StateData[Schema.StateWidth + ContactBinding->Column], 0.0f);

	Robot->DestroySlots();
	PreviewScene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-E2E-ROBOT-CONTENT-002: reset/action/State signs and SI torque survive a rotated Slot frame"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLGenericRobotSharedWorldBodiesTest,
	"UERL.Integration.Robot.GenericSkeletalMesh.AC_UE_E2E_ROBOT_CONTENT_003.SharedWorldBodies",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLGenericRobotSharedWorldBodiesTest::RunTest(const FString& Parameters)
{
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
	const FUERLRobotTopology& Topology = Factory->Describe().Topology;
	const int32 CartBodyIndex = Topology.BodyNames.IndexOfByKey(FName(TEXT("cart")));
	const int32 CartJointIndex = Topology.Joints.IndexOfByPredicate(
		[CartBodyIndex](const FUERLJointTopology& Joint)
		{
			return Joint.ChildBodyIndex == CartBodyIndex
				&& Joint.CoordinateType == EUERLJointCoordinateType::Prismatic;
		});
	TestTrue(TEXT("shared-world fixture exposes the cart actuator joint"), CartJointIndex != INDEX_NONE);
	if (CartJointIndex == INDEX_NONE)
	{
		return false;
	}
	const FUERLJointTopology& CartJoint = Topology.Joints[CartJointIndex];
	Input.Actuators.Add(FUERLActuatorConfig{
		0, CartJointIndex, CartJoint.Name, TEXT("prismatic"), TEXT("linear_x"), TEXT("N"), TEXT("effort"),
		0.0, 1.0, 100.0, 0.0 });
	if (!Factory->ValidateConfig(Input, Effective, Error))
	{
		AddError(Error);
		return false;
	}
	TUniquePtr<IUERLRobot> Robot = Factory->Create(Effective);
	const FUERLRobotDescriptor& Descriptor = Factory->Describe();
	const FUERLFieldDescriptor* ContactField = Descriptor.StateFields.FindByPredicate(
		[](const FUERLFieldDescriptor& Field)
		{
			return Field.Observation.Type == EUERLObservationType::Contact;
		});
	TestNotNull(TEXT("shared-world fixture has a contact observation"), ContactField);
	if (!ContactField)
	{
		return false;
	}
	if (!Robot->PrepareState({ *ContactField }, Error))
	{
		AddError(Error);
		return false;
	}
	FUERLBatchSchema Schema;
	Schema.ActionFields.Add(FUERLBatchFieldBinding{ Descriptor.ActionFields[0], 0 });
	Schema.ActionWidth = Descriptor.ActionFields[0].Width;
	Schema.StateFields.Add(FUERLBatchFieldBinding{ *ContactField, 0 });
	Schema.StateWidth = ContactField->Width;
	FUERLBatchBinding Binding;
	if (!Binding.Compile(Schema, Descriptor.ActionFields, Descriptor.StateFields, Error))
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
	FUERLSlotCollisionPlan CollisionPlan;
	if (!CollisionPlan.Compile(2, EUERLEnvironmentCollisionScope::SharedWorld, Error))
	{
		AddError(Error);
		return false;
	}
	TArray<FUERLSlotContext> Slots = {
		FUERLSlotContext{ 0, FVector(-300.0, 0.0, 500.0), 0.0, FVector::UpVector, {} },
		FUERLSlotContext{ 1, FVector(300.0, 0.0, 500.0), 0.0, FVector::UpVector, {} },
	};
	Slots[0].CollisionProfile = CollisionPlan.Profile(0);
	Slots[1].CollisionProfile = CollisionPlan.Profile(1);
	if (!Robot->SpawnIntoSlots(*World, Slots, Error))
	{
		AddError(Error);
		return false;
	}

	TArray<USkeletalMeshComponent*> Components;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		USkeletalMeshComponent* Candidate = It->FindComponentByClass<USkeletalMeshComponent>();
		if (Candidate && Candidate->GetFName() == FName(TEXT("RobotSkeletalMesh")))
		{
			Components.Add(Candidate);
		}
	}
	Components.Sort([](const USkeletalMeshComponent& Left, const USkeletalMeshComponent& Right)
	{
		return Left.GetOwner()->GetActorLocation().X < Right.GetOwner()->GetActorLocation().X;
	});
	TestEqual(TEXT("both shared-map Robots spawn without per-Slot Environment actors"), Components.Num(), 2);
	if (Components.Num() != 2)
	{
		Robot->DestroySlots();
		return false;
	}

	for (int32 SlotId = 0; SlotId < Components.Num(); ++SlotId)
	{
		TestEqual(TEXT("shared-map Robot remains a PhysicsBody"),
			Components[SlotId]->GetCollisionObjectType(), ECC_PhysicsBody);
		TestEqual(TEXT("shared-map Robot blocks WorldStatic"),
			Components[SlotId]->GetCollisionResponseToChannel(ECC_WorldStatic), ECR_Block);
		TestEqual(TEXT("shared-map Robot preserves PhysicsAsset self-collision response"),
			Components[SlotId]->GetCollisionResponseToChannel(ECC_PhysicsBody), ECR_Block);
		for (const FName BodyName : Topology.BodyNames)
		{
			FBodyInstance* Body = Components[SlotId]->GetBodyInstance(BodyName);
			FPhysicsActorHandle Handle = Body ? Body->GetPhysicsActorHandle() : nullptr;
			Chaos::FPBDRigidParticle* Particle = Handle
				? Handle->GetParticle_LowLevel()->CastToRigidParticle() : nullptr;
			TestNotNull(TEXT("reflected Robot body has a Chaos particle"), Particle);
			if (Particle)
			{
				TestEqual(TEXT("every Robot body receives its Slot collision group"),
					Particle->CollisionGroup(), CollisionPlan.Profile(SlotId).CollisionGroup());
			}
		}
	}

	FHitResult BlockingHit;
	BlockingHit.bBlockingHit = true;
	BlockingHit.MyBoneName = Topology.BodyNames[ContactField->Observation.BodyIndex];
	Components[0]->OnComponentHit.Broadcast(
		Components[0], Components[1]->GetOwner(), Components[1], FVector::ZeroVector, BlockingHit);
	TArray<float> StateData;
	StateData.SetNumZeroed(Schema.StateWidth * Components.Num());
	FUERLNamedStateWriter Writer(Binding, { StateData.GetData(), Components.Num(), Schema.StateWidth });
	Robot->CollectState({ 0, 1 }, Writer);
	TestEqual(TEXT("a hit event alone does not enter terminal support State"), StateData[0], 0.0f);

	AActor* Ground = SpawnGround(*World);
	TestNotNull(TEXT("shared WorldStatic contact fixture is available"), Ground);
	if (Ground)
	{
		UStaticMeshComponent* GroundMesh = Ground->FindComponentByClass<UStaticMeshComponent>();
		TestNotNull(TEXT("shared WorldStatic fixture mesh is available"), GroundMesh);
		if (GroundMesh)
		{
			GroundMesh->SetMobility(EComponentMobility::Movable);
		}
		Ground->SetActorScale3D(FVector(2.0, 2.0, 0.5));
		const FBodyInstance* ContactBody = Components[0]->GetBodyInstance(
			Topology.BodyNames[ContactField->Observation.BodyIndex]);
		const double ContactBottom = ContactBody ? ContactBody->GetBodyBounds().Min.Z : 0.0;
		Ground->SetActorLocation(FVector(
			Components[0]->GetComponentLocation().X,
			Components[0]->GetComponentLocation().Y,
			ContactBottom - 25.0), false, nullptr, ETeleportType::TeleportPhysics);
		constexpr double SolverStepSeconds = 1.0 / 120.0;
		Robot->SamplePhysicsContacts(SolverStepSeconds);
		for (float& Value : StateData)
		{
			Value = 0.0f;
		}
		Robot->CollectState({ 0, 1 }, Writer);
		TestEqual(TEXT("shared WorldStatic geometry enters the owning Robot contact State"), StateData[0], 1.0f);
		TestEqual(TEXT("support geometry does not enter another Robot contact State"),
			StateData[Schema.StateWidth], 0.0f);
		Ground->Destroy();
	}

	Robot->DestroySlots();
	PreviewScene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-E2E-ROBOT-CONTENT-003: Generic Robot bodies and contacts remain Slot-isolated on a shared World"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyGroundSamplingTest,
	"UERL.Integration.Policy.Ground.AC_UE_INT_POLICY_GROUND_001_005.RootRelativeCaching",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyGroundSamplingTest::RunTest(const FString& Parameters)
{
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
	AuthoredMesh->RegisterComponent();
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
	TestEqual(TEXT("release restores the authored material sleep type"),
		CountBodiesWithSleepType(Chaos::ESleepType::MaterialSleep), AuthoredMesh->Bodies.Num());

	PreviewScene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-E2E-ROBOT-CONTENT-004: idle PhantomX stays awake, follows a changed drive target, and claim release restores sleep"));
	return true;
}

namespace
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

namespace
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
