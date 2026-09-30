#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "PreviewScene.h"
#include "UERLIsolatedGridEnvironment.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLIsolatedGridEnvironmentDescriptorTest,
	"UERL.Unit.Worker.IsolatedGrid.AC_UE_UNIT_ISOLATED_GRID_001.DescriptorAndConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLIsolatedGridEnvironmentDescriptorTest::RunTest(const FString& Parameters)
{
	TSharedRef<IUERLEnvironmentFactory> Factory = UERLIsolatedGrid::MakeFactory();
	TestEqual(TEXT("isolated-grid descriptor uses Slot-local collision"),
		Factory->Describe().CollisionScope, EUERLEnvironmentCollisionScope::SlotIsolated);

	FUERLProviderConfig Input;
	FUERLProviderConfig Effective;
	FString Error;
	TestTrue(TEXT("isolated-grid defaults validate"), Factory->ValidateConfig(Input, Effective, Error));
	TestEqual(TEXT("default grid has eight columns"), Effective.Scalars[UERLIsolatedGrid::Columns], 8.0);
	Input.Scalars.Add(UERLIsolatedGrid::Columns, 64.5);
	TestFalse(TEXT("fractional columns fail at the config boundary"),
		Factory->ValidateConfig(Input, Effective, Error));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ISOLATED-GRID-001: descriptor and scalar boundary are stable"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLIsolatedGridEnvironmentPlacementTest,
	"UERL.Unit.Worker.IsolatedGrid.AC_UE_UNIT_ISOLATED_GRID_002.RowMajorAnchors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLIsolatedGridEnvironmentPlacementTest::RunTest(const FString& Parameters)
{
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld& World = *Scene->GetWorld();

	TSharedRef<IUERLEnvironmentFactory> Factory = UERLIsolatedGrid::MakeFactory();
	FUERLProviderConfig Input;
	Input.Scalars.Add(UERLIsolatedGrid::SpacingX, 1.0);
	Input.Scalars.Add(UERLIsolatedGrid::SpacingY, 2.0);
	Input.Scalars.Add(UERLIsolatedGrid::Columns, 2.0);
	Input.Scalars.Add(UERLIsolatedGrid::OriginX, -0.5);
	Input.Scalars.Add(UERLIsolatedGrid::OriginY, -1.0);
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
	if (!CollisionPlan.Compile(4, EUERLEnvironmentCollisionScope::SlotIsolated, Error))
	{
		AddError(Error);
		return false;
	}
	FUERLTerrainConfig PlaneTerrain;
	PlaneTerrain.NumLevels = 1;
	PlaneTerrain.CellSize[0] = 1.0;
	PlaneTerrain.CellSize[1] = 2.0;
	FUERLTerrainTierConfig& Tier = PlaneTerrain.Tiers.AddDefaulted_GetRef();
	Tier.Level = 0;
	Tier.Primitive = EUERLTerrainPrimitive::Plane;
	Tier.Params = MakeShared<FJsonObject>();
	TestTrue(TEXT("four isolated Slots create anchor actors"),
		Environment->CreateSlots(World, 4, CollisionPlan, PlaneTerrain, Slots, Error));
	if (Slots.Num() != 4)
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Slot 0 starts at configured origin"),
		Slots[0].Origin.Equals(FVector(-50.0, -100.0, 0.0), 0.1));
	TestTrue(TEXT("Slot 1 advances one X spacing"),
		Slots[1].Origin.Equals(FVector(50.0, -100.0, 0.0), 0.1));
	TestTrue(TEXT("Slot 2 advances one Y spacing"),
		Slots[2].Origin.Equals(FVector(-50.0, 100.0, 0.0), 0.1));
	for (const FUERLSlotContext& Slot : Slots)
	{
		TestEqual(TEXT("each isolated Slot owns one anchor and one terrain actor"), Slot.EnvironmentActors.Num(), 2);
		TestTrue(TEXT("isolated-grid ground normal points up"),
			Slot.GroundNormal.Equals(FVector::UpVector, 0.01));
	}
	Environment->DestroySlots();
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ISOLATED-GRID-002: row-major Slots own independent anchors"));
	return true;
}

#endif
