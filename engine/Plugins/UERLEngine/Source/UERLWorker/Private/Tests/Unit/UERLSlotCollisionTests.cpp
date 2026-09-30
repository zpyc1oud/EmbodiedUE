#include "Misc/AutomationTest.h"

#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PreviewScene.h"
#include "UERLSlotCollisionPlan.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSlotCollisionPlanTest,
	"UERL.Unit.Worker.SlotCollision.AC_UE_UNIT_SLOT_COLLISION_001.Plan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSlotCollisionPlanTest::RunTest(const FString& Parameters)
{
	constexpr int32 NumSlots = 64;
	FUERLSlotCollisionPlan Plan;
	FString Error;
	TestTrue(TEXT("64 Slot collision profiles compile"), Plan.Compile(
		NumSlots, EUERLEnvironmentCollisionScope::SlotIsolated, Error));
	if (!Plan.IsCompiled())
	{
		AddError(Error);
		return false;
	}
	TSet<ECollisionChannel> Channels;
	for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
	{
		Channels.Add(Plan.Profile(SlotId).Channel());
	}
	TestEqual(TEXT("every Slot channel is unique"), Channels.Num(), 64);

	UStaticMeshComponent* First = NewObject<UStaticMeshComponent>();
	UStaticMeshComponent* Last = NewObject<UStaticMeshComponent>();
	Plan.Profile(0).ApplyRobot(*First);
	Plan.Profile(63).ApplyRobot(*Last);
	TestNotEqual(TEXT("different Slots use different object channels"),
		First->GetCollisionObjectType(), Last->GetCollisionObjectType());
	TestEqual(TEXT("Slot blocks its own channel"),
		First->GetCollisionResponseToChannel(Plan.Profile(0).Channel()), ECR_Block);
	TestEqual(TEXT("Slot ignores another Slot channel"),
		First->GetCollisionResponseToChannel(Plan.Profile(63).Channel()), ECR_Ignore);
	TestFalse(TEXT("a 65th Slot cannot be represented by the Chaos channel plan"),
		Plan.Compile(65, EUERLEnvironmentCollisionScope::SlotIsolated, Error));
	TestFalse(TEXT("failed recompile clears the plan"), Plan.IsCompiled());
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-SLOT-COLLISION-001: 64 unique Slot channels compile with self-only blocking"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSharedWorldCollisionPlanTest,
	"UERL.Unit.Worker.SlotCollision.AC_UE_UNIT_SHARED_WORLD_COLLISION_001.Plan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSharedWorldCollisionPlanTest::RunTest(const FString& Parameters)
{
	constexpr int32 NumSlots = 512;
	FUERLSlotCollisionPlan Plan;
	FString Error;
	TestTrue(TEXT("512 shared-world profiles compile"), Plan.Compile(
		NumSlots, EUERLEnvironmentCollisionScope::SharedWorld, Error));
	if (!Plan.IsCompiled())
	{
		AddError(Error);
		return false;
	}
	TSet<int32> Groups;
	for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
	{
		const FUERLSlotCollisionProfile& Profile = Plan.Profile(SlotId);
		Groups.Add(Profile.CollisionGroup());
		TestTrue(TEXT("shared-world group is non-zero"), Profile.CollisionGroup() > 0);
	}
	TestEqual(TEXT("every shared-world Robot group is unique"), Groups.Num(), NumSlots);

	UStaticMeshComponent* Robot = NewObject<UStaticMeshComponent>();
	Plan.Profile(0).ApplyRobot(*Robot);
	TestEqual(TEXT("shared-world Robot uses PhysicsBody object type"),
		Robot->GetCollisionObjectType(), ECC_PhysicsBody);
	TestEqual(TEXT("shared-world Robot blocks WorldStatic map"),
		Robot->GetCollisionResponseToChannel(ECC_WorldStatic), ECR_Block);
	TestEqual(TEXT("shared-world Robot preserves same-Robot body response"),
		Robot->GetCollisionResponseToChannel(ECC_PhysicsBody), ECR_Block);

	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.SetTransactional(false)
			.SetEditor(false));
	AActor* Owner = Scene->GetWorld()->SpawnActor<AActor>();
	UBoxComponent* BodyComponent = NewObject<UBoxComponent>(Owner);
	Owner->SetRootComponent(BodyComponent);
	BodyComponent->SetBoxExtent(FVector(25.0));
	Plan.Profile(7).ApplyRobot(*BodyComponent);
	BodyComponent->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	BodyComponent->RegisterComponent();
	BodyComponent->SetSimulatePhysics(true);
	FBodyInstance* Body = BodyComponent->GetBodyInstance();
	TestNotNull(TEXT("shared-world fixture creates a physics body"), Body);
	if (Body)
	{
		TestTrue(TEXT("shared-world group applies to the initialized Chaos body"),
			Plan.Profile(7).ApplyRobotBody(*Body));
		FPhysicsActorHandle Handle = Body->GetPhysicsActorHandle();
		Chaos::FPBDRigidParticle* Particle = Handle
			? Handle->GetParticle_LowLevel()->CastToRigidParticle()
			: nullptr;
		TestNotNull(TEXT("shared-world fixture has a rigid Chaos particle"), Particle);
		if (Particle)
		{
			TestEqual(TEXT("Chaos body stores the Slot collision group"),
				Particle->CollisionGroup(), Plan.Profile(7).CollisionGroup());
		}
	}
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-SHARED-WORLD-COLLISION-001: 512 unique Robot groups preserve map and self response"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSlotCollisionWorldFilteringTest,
	"UERL.Integration.Worker.SlotCollision.AC_UE_INTEGRATION_SLOT_COLLISION_001.WorldFiltering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSlotCollisionWorldFilteringTest::RunTest(const FString& Parameters)
{
	constexpr int32 NumSlots = FUERLSlotCollisionPlan::MaxSlots;
	FUERLSlotCollisionPlan Plan;
	FString Error;
	if (!Plan.Compile(NumSlots, EUERLEnvironmentCollisionScope::SlotIsolated, Error))
	{
		AddError(Error);
		return false;
	}
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = Scene->GetWorld();
	TArray<UBoxComponent*> TerrainComponents;
	TArray<UBoxComponent*> RobotFilters;
	TerrainComponents.Reserve(NumSlots);
	RobotFilters.Reserve(NumSlots);

	for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
	{
		AActor* Owner = World->SpawnActor<AActor>();
		UBoxComponent* Terrain = NewObject<UBoxComponent>(Owner);
		Owner->SetRootComponent(Terrain);
		Terrain->SetBoxExtent(FVector(50.0));
		Terrain->SetWorldLocation(FVector::ZeroVector);
		Plan.Profile(SlotId).ApplySlotEnvironment(*Terrain);
		Terrain->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Terrain->RegisterComponent();
		TerrainComponents.Add(Terrain);

		UBoxComponent* RobotFilter = NewObject<UBoxComponent>();
		Plan.Profile(SlotId).ApplyRobot(*RobotFilter);
		RobotFilters.Add(RobotFilter);
	}

	for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
	{
		for (int32 OtherSlotId = 0; OtherSlotId < NumSlots; ++OtherSlotId)
		{
			const ECollisionResponse Expected = SlotId == OtherSlotId ? ECR_Block : ECR_Ignore;
			const ECollisionResponse Actual =
				RobotFilters[SlotId]->GetCollisionResponseToComponent(TerrainComponents[OtherSlotId]);
			if (Actual != Expected)
			{
				AddError(FString::Printf(
					TEXT("Slot %d pair response to Slot %d was %d, expected %d"),
					SlotId, OtherSlotId, static_cast<int32>(Actual), static_cast<int32>(Expected)));
				return false;
			}
		}

		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(
			Hit, FVector(0.0, 0.0, 100.0), FVector(0.0, 0.0, -100.0), Plan.Profile(SlotId).Channel()))
		{
			AddError(FString::Printf(TEXT("Slot %d query did not hit its terrain"), SlotId));
			return false;
		}
		if (Hit.GetComponent() != TerrainComponents[SlotId])
		{
			AddError(FString::Printf(TEXT("Slot %d query hit another Slot"), SlotId));
			return false;
		}
	}

	Scene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-INTEGRATION-SLOT-COLLISION-001: 64 overlapping Slots block only same-Slot pairs and queries"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSharedWorldCollisionPhysicsTest,
	"UERL.Integration.Worker.SharedWorldCollision.AC_UE_INTEGRATION_SHARED_WORLD_COLLISION_001.Physics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSharedWorldCollisionPhysicsTest::RunTest(const FString& Parameters)
{
	FUERLSlotCollisionPlan Plan;
	FString Error;
	if (!Plan.Compile(4, EUERLEnvironmentCollisionScope::SharedWorld, Error))
	{
		AddError(Error);
		return false;
	}
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld* World = Scene->GetWorld();

	AActor* GroundOwner = World->SpawnActor<AActor>();
	UBoxComponent* Ground = NewObject<UBoxComponent>(GroundOwner);
	GroundOwner->SetRootComponent(Ground);
	Ground->SetBoxExtent(FVector(1000.0, 1000.0, 25.0));
	Ground->SetWorldLocation(FVector(0.0, 0.0, -25.0));
	Ground->SetCollisionObjectType(ECC_WorldStatic);
	Ground->SetCollisionResponseToAllChannels(ECR_Block);
	Ground->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Ground->RegisterComponent();

	auto SpawnRobotBody = [World, &Plan](
		int32 SlotId, const FVector& Location, bool bGravity, const FVector& Velocity)
	{
		AActor* Owner = World->SpawnActor<AActor>();
		UBoxComponent* Body = NewObject<UBoxComponent>(Owner);
		Owner->SetRootComponent(Body);
		Body->SetBoxExtent(FVector(25.0));
		Body->SetWorldLocation(Location);
		Plan.Profile(SlotId).ApplyRobot(*Body);
		Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Body->SetLinearDamping(0.0);
		Body->SetAngularDamping(0.0);
		Body->RegisterComponent();
		Body->SetSimulatePhysics(true);
		Body->SetEnableGravity(bGravity);
		Plan.Profile(SlotId).ApplyRobotBody(*Body->GetBodyInstance());
		Body->SetPhysicsLinearVelocity(Velocity);
		return Body;
	};

	UBoxComponent* Left = SpawnRobotBody(0, FVector(-100.0, 0.0, 100.0), false, FVector(200.0, 0.0, 0.0));
	UBoxComponent* Right = SpawnRobotBody(1, FVector(100.0, 0.0, 100.0), false, FVector(-200.0, 0.0, 0.0));
	UBoxComponent* SameLeft = SpawnRobotBody(2, FVector(-100.0, 300.0, 100.0), false, FVector(200.0, 0.0, 0.0));
	UBoxComponent* SameRight = SpawnRobotBody(2, FVector(100.0, 300.0, 100.0), false, FVector(-200.0, 0.0, 0.0));
	UBoxComponent* Falling = SpawnRobotBody(3, FVector(400.0, 0.0, 200.0), true, FVector::ZeroVector);

	constexpr float PhysicsDt = 1.0f / 120.0f;
	for (int32 Tick = 0; Tick < 240; ++Tick)
	{
		World->Tick(ELevelTick::LEVELTICK_All, PhysicsDt);
		++GFrameCounter;
	}

	const double LeftX = Left->GetBodyInstance()->GetUnrealWorldTransform().GetLocation().X;
	const double RightX = Right->GetBodyInstance()->GetUnrealWorldTransform().GetLocation().X;
	const double SameLeftX = SameLeft->GetBodyInstance()->GetUnrealWorldTransform().GetLocation().X;
	const double SameRightX = SameRight->GetBodyInstance()->GetUnrealWorldTransform().GetLocation().X;
	const double FallingZ = Falling->GetBodyInstance()->GetUnrealWorldTransform().GetLocation().Z;
	AddInfo(FString::Printf(
		TEXT("shared-world body positions: cross=(%.3f, %.3f) same=(%.3f, %.3f) falling_z=%.3f"),
		LeftX, RightX, SameLeftX, SameRightX, FallingZ));
	TestTrue(TEXT("different Robot groups pass through one another"), LeftX > 200.0 && RightX < -200.0);
	TestTrue(TEXT("bodies in one Robot group still collide"), SameLeftX < 0.0 && SameRightX > 0.0);
	TestTrue(TEXT("a Robot group remains supported by group-zero WorldStatic ground"),
		FallingZ >= 24.0 && FallingZ <= 30.0);

	Scene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-INTEGRATION-SHARED-WORLD-COLLISION-001: Robots ignore other groups and still collide with the map"));
	return true;
}

#endif
