#include "Misc/AutomationTest.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PreviewScene.h"
#include "UERLRegistry.h"
#include "UERLSharedMapEnvironment.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace
{
	UBoxComponent* CreateWorldStaticGround(UWorld& World, const FVector& Extent)
	{
		AActor* Owner = World.SpawnActor<AActor>();
		UBoxComponent* Ground = NewObject<UBoxComponent>(Owner);
		Owner->SetRootComponent(Ground);
		Ground->SetBoxExtent(Extent);
		Ground->SetWorldLocation(FVector(0.0, 0.0, -Extent.Z));
		Ground->SetCollisionObjectType(ECC_WorldStatic);
		Ground->SetCollisionResponseToAllChannels(ECR_Block);
		Ground->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Ground->RegisterComponent();
		return Ground;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSharedMapEnvironmentDescriptorTest,
	"UERL.Unit.Worker.SharedMap.AC_UE_UNIT_SHARED_MAP_001.DescriptorAndConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSharedMapEnvironmentDescriptorTest::RunTest(const FString& Parameters)
{
	FString Error;
	TSharedPtr<IUERLEnvironmentFactory> Registered =
		FUERLEnvironmentRegistry::Get().Resolve(UERLSharedMap::EnvironmentId, Error);
	TestTrue(TEXT("shared-map Environment is registered by UERLWorker"), Registered.IsValid());
	if (!Registered)
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("shared-map descriptor declares SharedWorld collision"),
		Registered->Describe().CollisionScope, EUERLEnvironmentCollisionScope::SharedWorld);

	FUERLProviderConfig Input;
	FUERLProviderConfig Effective;
	TestTrue(TEXT("shared-map defaults validate"), Registered->ValidateConfig(Input, Effective, Error));
	TestEqual(TEXT("default grid has eight columns"), Effective.Scalars[UERLSharedMap::Columns], 8.0);
	Input.Scalars.Add(UERLSharedMap::Columns, 2.5);
	TestFalse(TEXT("fractional columns fail at the config boundary"),
		Registered->ValidateConfig(Input, Effective, Error));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-SHARED-MAP-001: built-in descriptor and scalar boundary are stable"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSharedMapEnvironmentPlacementTest,
	"UERL.Unit.Worker.SharedMap.AC_UE_UNIT_SHARED_MAP_002.WorldStaticPlacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSharedMapEnvironmentPlacementTest::RunTest(const FString& Parameters)
{
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld& World = *Scene->GetWorld();
	CreateWorldStaticGround(World, FVector(500.0, 500.0, 50.0));

	TSharedRef<IUERLEnvironmentFactory> Factory = UERLSharedMap::MakeFactory();
	FUERLProviderConfig Input;
	Input.Scalars.Add(UERLSharedMap::SpacingX, 1.0);
	Input.Scalars.Add(UERLSharedMap::SpacingY, 2.0);
	Input.Scalars.Add(UERLSharedMap::Columns, 2.0);
	Input.Scalars.Add(UERLSharedMap::TraceStartZ, 5.0);
	Input.Scalars.Add(UERLSharedMap::TraceDepth, 10.0);
	FUERLProviderConfig Effective;
	FString Error;
	if (!Factory->ValidateConfig(Input, Effective, Error))
	{
		AddError(Error);
		return false;
	}
	TUniquePtr<IUERLEnvironment> Environment = Factory->Create(Effective);
	TArray<FUERLSlotContext> Slots;
	FUERLSlotCollisionPlan CollisionPlan;
	if (!CollisionPlan.Compile(4, EUERLEnvironmentCollisionScope::SharedWorld, Error))
	{
		AddError(Error);
		return false;
	}
	FUERLTerrainConfig EmptyTerrain;
	TestTrue(TEXT("four Slots trace the shared WorldStatic map"),
		Environment->CreateSlots(World, 4, CollisionPlan, EmptyTerrain, Slots, Error));
	if (Slots.Num() != 4)
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Slot 0 starts at grid origin"), Slots[0].Origin.Equals(FVector(0.0, 0.0, 0.0), 0.1));
	TestTrue(TEXT("Slot 1 advances one X spacing"), Slots[1].Origin.Equals(FVector(100.0, 0.0, 0.0), 0.1));
	TestTrue(TEXT("Slot 2 advances one Y spacing"), Slots[2].Origin.Equals(FVector(0.0, 200.0, 0.0), 0.1));
	for (const FUERLSlotContext& Slot : Slots)
	{
		TestTrue(TEXT("shared-map Slot owns no copied actors"), Slot.EnvironmentActors.IsEmpty());
		TestTrue(TEXT("shared-map ground normal points up"), Slot.GroundNormal.Equals(FVector::UpVector, 0.01));
	}
	FVector ResolvedOrigin;
	double ResolvedGroundHeight = 0.0;
	FVector ResolvedGroundNormal;
	TestTrue(TEXT("authored map resolves its cached ground frame without the generator"),
		Environment->ResolveGroundFrame(
			2, 0, ResolvedOrigin, ResolvedGroundHeight, ResolvedGroundNormal, Error));
	TestTrue(TEXT("cached authored ground frame matches Slot 2"),
		ResolvedOrigin.Equals(Slots[2].Origin, 0.1)
			&& FMath::IsNearlyEqual(ResolvedGroundHeight, Slots[2].GroundHeight)
			&& ResolvedGroundNormal.Equals(Slots[2].GroundNormal, 0.01));
	TestFalse(TEXT("authored map rejects a procedural terrain level"),
		Environment->ResolveGroundFrame(
			2, 1, ResolvedOrigin, ResolvedGroundHeight, ResolvedGroundNormal, Error));

	Input.Scalars.Add(UERLSharedMap::OriginX, 20.0);
	Factory->ValidateConfig(Input, Effective, Error);
	Environment = Factory->Create(Effective);
	FUERLSlotCollisionPlan SingleSlotPlan;
	SingleSlotPlan.Compile(1, EUERLEnvironmentCollisionScope::SharedWorld, Error);
	TestFalse(TEXT("grid position without WorldStatic ground fails initialization"),
		Environment->CreateSlots(World, 1, SingleSlotPlan, EmptyTerrain, Slots, Error));
	TestTrue(TEXT("failed placement publishes no partial Slots"), Slots.IsEmpty());
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-SHARED-MAP-002: row-major Slots use shared WorldStatic ground frames"));
	return true;
}

#endif
