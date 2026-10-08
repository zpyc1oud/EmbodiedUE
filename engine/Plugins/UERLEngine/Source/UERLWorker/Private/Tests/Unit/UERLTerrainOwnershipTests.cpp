#include "Misc/AutomationTest.h"

#include "Components/BoxComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PreviewScene.h"
#include "ProceduralMeshComponent.h"
#include "UERLSlotCollisionPlan.h"
#include "UERLTerrainGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTerrainSlotOwnershipTest,
	"UERL.Integration.Worker.Terrain.AC_UE_INTEGRATION_TERRAIN_001.SlotOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTerrainSlotOwnershipTest::RunTest(const FString& Parameters)
{
	FUERLTerrainConfig Config;
	Config.NumLevels = 2;
	Config.CellSize[0] = 4.0;
	Config.CellSize[1] = 4.0;
	FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
	Tier.Level = 0;
	Tier.Primitive = EUERLTerrainPrimitive::Plane;
	Tier.Params = MakeShared<FJsonObject>();
	FUERLTerrainTierConfig& Tier1 = Config.Tiers.AddDefaulted_GetRef();
	Tier1.Level = 1;
	Tier1.Primitive = EUERLTerrainPrimitive::Plane;
	Tier1.Params = MakeShared<FJsonObject>();

	TArray<FUERLSlotContext> Slots = {
		FUERLSlotContext{ 0, FVector::ZeroVector },
		FUERLSlotContext{ 1, FVector(0.0, 400.0, 0.0) },
	};
	FUERLSlotCollisionPlan CollisionPlan;
	FString Error;
	TestTrue(TEXT("two Slot collision profiles compile"), CollisionPlan.Compile(
		2, EUERLEnvironmentCollisionScope::SlotIsolated, Error));
	Slots[0].CollisionProfile = CollisionPlan.Profile(0);
	Slots[1].CollisionProfile = CollisionPlan.Profile(1);
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(false)
			.SetTransactional(false)
			.SetEditor(false));
	FUERLTerrainGenerator Generator;
	TestTrue(TEXT("two Slot plane terrain generates"),
		Generator.Generate(*Scene->GetWorld(), Config, Slots, Error));
	TestEqual(TEXT("Slot 0 owns one static atlas actor"), Slots[0].EnvironmentActors.Num(), 1);
	TestEqual(TEXT("Slot 1 owns one static atlas actor"), Slots[1].EnvironmentActors.Num(), 1);
	if (Slots[0].EnvironmentActors.IsEmpty() || Slots[1].EnvironmentActors.IsEmpty())
	{
		AddError(Error);
		return false;
	}
	TestNotEqual(TEXT("Slots do not share one terrain actor"),
		Slots[0].EnvironmentActors[0].Get(), Slots[1].EnvironmentActors[0].Get());
	const UProceduralMeshComponent* Slot0Terrain =
		Slots[0].EnvironmentActors[0]->FindComponentByClass<UProceduralMeshComponent>();
	const UProceduralMeshComponent* Slot1Terrain =
		Slots[1].EnvironmentActors[0]->FindComponentByClass<UProceduralMeshComponent>();
	TestNotNull(TEXT("Slot 0 atlas mesh exists"), Slot0Terrain);
	TestNotNull(TEXT("Slot 1 atlas mesh exists"), Slot1Terrain);
	if (Slot0Terrain && Slot1Terrain)
	{
		TestNotEqual(TEXT("Slot terrain object channels differ"),
			Slot0Terrain->GetCollisionObjectType(), Slot1Terrain->GetCollisionObjectType());
		TestEqual(TEXT("Slot 0 terrain ignores Slot 1 terrain"),
			Slot0Terrain->GetCollisionResponseToChannel(Slot1Terrain->GetCollisionObjectType()), ECR_Ignore);
	}
	Generator.Destroy();
	Scene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-INTEGRATION-TERRAIN-001: generated terrain actors and instances are Slot-owned"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSharedTerrainPhysicsTest,
	"UERL.Integration.Worker.Terrain.AC_UE_INTEGRATION_TERRAIN_002.SharedWorldPhysics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSharedTerrainPhysicsTest::RunTest(const FString& Parameters)
{
	FUERLTerrainConfig Config;
	Config.NumLevels = 1;
	Config.CellSize[0] = 4.0;
	Config.CellSize[1] = 4.0;
	FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
	Tier.Level = 0;
	Tier.Primitive = EUERLTerrainPrimitive::Plane;
	Tier.Params = MakeShared<FJsonObject>();

	FUERLSlotCollisionPlan CollisionPlan;
	FString Error;
	if (!CollisionPlan.Compile(1, EUERLEnvironmentCollisionScope::SharedWorld, Error))
	{
		AddError(Error);
		return false;
	}
	TArray<FUERLSlotContext> Slots = { FUERLSlotContext{ 0, FVector::ZeroVector } };
	Slots[0].CollisionProfile = CollisionPlan.Profile(0);
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld& World = *Scene->GetWorld();
	FUERLTerrainGenerator Generator;
	if (!Generator.GenerateShared(World, Config, Slots, Error))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("shared Slot queries one static atlas owner"), Slots[0].TerrainQueryActors.Num(), 1);

	AActor* Owner = World.SpawnActor<AActor>();
	UBoxComponent* Body = NewObject<UBoxComponent>(Owner);
	Owner->SetRootComponent(Body);
	Body->SetBoxExtent(FVector(25.0));
	Body->SetWorldLocation(FVector(0.0, 0.0, 200.0));
	CollisionPlan.Profile(0).ApplyRobot(*Body);
	Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Body->RegisterComponent();
	Body->SetSimulatePhysics(true);
	CollisionPlan.Profile(0).ApplyRobotBody(*Body->GetBodyInstance());

	constexpr float PhysicsDt = 1.0f / 120.0f;
	for (int32 Tick = 0; Tick < 240; ++Tick)
	{
		World.Tick(ELevelTick::LEVELTICK_All, PhysicsDt);
		++GFrameCounter;
	}
	const double BodyZ = Body->GetBodyInstance()->GetUnrealWorldTransform().GetLocation().Z;
	TestTrue(TEXT("a shared-world Robot remains supported by generated plane collision"),
		BodyZ >= 24.0 && BodyZ <= 30.0);
	AddInfo(FString::Printf(TEXT("shared generated plane falling body z=%.3f"), BodyZ));

	Generator.Destroy();
	Scene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-INTEGRATION-TERRAIN-002: shared procedural terrain supports shared-world Robot physics"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSharedHeightfield64SlotGenerationTest,
	"UERL.Integration.Worker.Terrain.AC_UE_INTEGRATION_TERRAIN_004.SharedHeightfield64Slots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSharedHeightfield64SlotGenerationTest::RunTest(const FString& Parameters)
{
	constexpr int32 SlotSide = 8;
	constexpr int32 NumSlots = SlotSide * SlotSide;
	constexpr double SlotSpan = 24.0;

	FUERLTerrainConfig WideConfig;
	WideConfig.NumLevels = 1;
	WideConfig.CellSize[0] = 30.0;
	WideConfig.CellSize[1] = 30.0;
	WideConfig.BorderWidth = 1.0;
	FUERLTerrainTierConfig& WideTier = WideConfig.Tiers.AddDefaulted_GetRef();
	WideTier.Level = 0;
	WideTier.Primitive = EUERLTerrainPrimitive::Heightfield;
	WideTier.Seed = 20260831;
	WideTier.PlatformWidth = 0.8;
	WideTier.Params = MakeShared<FJsonObject>();
	WideTier.Params->SetArrayField(TEXT("noise_range"), {
		MakeShared<FJsonValueNumber>(-0.12), MakeShared<FJsonValueNumber>(0.12) });
	WideTier.Params->SetNumberField(TEXT("noise_step"), 0.005);
	WideTier.Params->SetNumberField(TEXT("horizontal_scale"), 1.0);
	WideTier.Params->SetNumberField(TEXT("vertical_scale"), 0.005);
	WideTier.Params->SetNumberField(TEXT("downsampled_scale"), 4.0);

	FUERLTerrainConfig NarrowConfig = WideConfig;
	NarrowConfig.Tiers[0].PlatformWidth = 0.0;
	TArray<FVector> SlotOrigins;
	SlotOrigins.Reserve(NumSlots);
	for (int32 Row = 0; Row < SlotSide; ++Row)
	{
		for (int32 Column = 0; Column < SlotSide; ++Column)
		{
			const double X = -SlotSpan * 0.5 + static_cast<double>(Column) * SlotSpan / (SlotSide - 1);
			const double Y = -SlotSpan * 0.5 + static_cast<double>(Row) * SlotSpan / (SlotSide - 1);
			SlotOrigins.Add(FVector(X * 100.0, Y * 100.0, 0.0));
		}
	}

	TArray<FUERLTerrainTierPlan> NarrowPlans;
	TArray<FUERLTerrainTierPlan> WidePlans;
	FString Error;
	const bool bNarrowBuilt = FUERLTerrainGenerator::BuildSharedPlan(
		NarrowConfig, SlotOrigins, NarrowPlans, Error);
	const bool bWideBuilt = FUERLTerrainGenerator::BuildSharedPlan(
		WideConfig, SlotOrigins, WidePlans, Error);
	TestTrue(FString::Printf(TEXT("64-slot reset-window plans build: %s"), *Error), bNarrowBuilt && bWideBuilt);
	if (!bNarrowBuilt || !bWideBuilt || NarrowPlans.Num() != 1 || WidePlans.Num() != 1)
	{
		return false;
	}

	if (!TestEqual(TEXT("narrow plan has one mesh"), NarrowPlans[0].Meshes.Num(), 1)
		|| !TestEqual(TEXT("wide plan has one mesh"), WidePlans[0].Meshes.Num(), 1)
		|| !TestEqual(TEXT("narrow plan samples all Slots"), NarrowPlans[0].Samples.Num(), NumSlots)
		|| !TestEqual(TEXT("wide plan samples all Slots"), WidePlans[0].Samples.Num(), NumSlots))
	{
		return false;
	}

	const FUERLTerrainMeshSpec& NarrowMesh = NarrowPlans[0].Meshes[0];
	const FUERLTerrainMeshSpec& WideMesh = WidePlans[0].Meshes[0];
	const bool bGeometryUnchanged = NarrowMesh.VerticesMeters == WideMesh.VerticesMeters
		&& NarrowMesh.Normals == WideMesh.Normals
		&& NarrowMesh.Triangles == WideMesh.Triangles;
	TestTrue(TEXT("reset window leaves the 32 m heightfield geometry unchanged"), bGeometryUnchanged);

	bool bResetWindowMonotonic = true;
	for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
	{
		bResetWindowMonotonic = bResetWindowMonotonic
			&& WidePlans[0].Samples[SlotId].GroundHeight
				>= NarrowPlans[0].Samples[SlotId].GroundHeight - 1.0e-6;
	}
	TestTrue(TEXT("each wider reset window keeps or raises the sampled terrain top"), bResetWindowMonotonic);

	FString CollisionError;
	FUERLSlotCollisionPlan CollisionPlan;
	if (!CollisionPlan.Compile(NumSlots, EUERLEnvironmentCollisionScope::SharedWorld, CollisionError))
	{
		AddError(CollisionError);
		return false;
	}
	TArray<FUERLSlotContext> Slots;
	Slots.Reserve(NumSlots);
	for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
	{
		FUERLSlotContext& Slot = Slots.AddDefaulted_GetRef();
		Slot.SlotId = SlotId;
		Slot.Origin = SlotOrigins[SlotId];
		Slot.CollisionProfile = CollisionPlan.Profile(SlotId);
	}

	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(false)
			.SetTransactional(false)
			.SetEditor(false));
	FUERLTerrainGenerator Generator;
	if (!Generator.GenerateShared(*Scene->GetWorld(), WideConfig, Slots, Error))
	{
		AddError(Error);
		return false;
	}

	bool bSharedOwner = true;
	AActor* SharedOwner = nullptr;
	for (FUERLSlotContext& Slot : Slots)
	{
		bSharedOwner = bSharedOwner && Slot.TerrainQueryActors.Num() == 1;
		if (SharedOwner == nullptr && Slot.TerrainQueryActors.Num() == 1)
		{
			SharedOwner = Slot.TerrainQueryActors[0].Get();
		}
		bSharedOwner = bSharedOwner && Slot.TerrainQueryActors.Num() == 1
			&& Slot.TerrainQueryActors[0].Get() == SharedOwner;
	}
	TestTrue(TEXT("all 64 Slots reference one generated SharedWorld owner"), bSharedOwner && SharedOwner != nullptr);
	if (SharedOwner == nullptr)
	{
		Generator.Destroy();
		return false;
	}

	UProceduralMeshComponent* Heightfield = SharedOwner->FindComponentByClass<UProceduralMeshComponent>();
	TestNotNull(TEXT("SharedWorld heightfield collision component exists"), Heightfield);
	FProcMeshSection* Section = Heightfield ? Heightfield->GetProcMeshSection(0) : nullptr;
	TestNotNull(TEXT("SharedWorld heightfield collision section exists"), Section);
	const bool bRuntimeMesh = Section != nullptr
		&& Section->ProcVertexBuffer.Num() == WideMesh.VerticesMeters.Num()
		&& Section->ProcIndexBuffer.Num() == WideMesh.Triangles.Num();
	TestTrue(TEXT("runtime SharedWorld collision keeps the generated 32 m heightfield mesh"), bRuntimeMesh);

	bool bRuntimeSamples = Generator.TierPlans().Num() == 1
		&& Generator.TierPlans()[0].Samples.Num() == NumSlots;
	if (bRuntimeSamples)
	{
		for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
		{
			const FUERLTerrainSpawnSample& Expected = WidePlans[0].Samples[SlotId];
			const FUERLTerrainSpawnSample& Actual = Generator.TierPlans()[0].Samples[SlotId];
			bRuntimeSamples = bRuntimeSamples
				&& FMath::IsNearlyEqual(Actual.GroundHeight, Expected.GroundHeight)
				&& Actual.Origin.Equals(Expected.Origin, 1.0e-6);
		}
	}
	TestTrue(TEXT("runtime SharedWorld preserves all 64 reset samples"), bRuntimeSamples);
	AddInfo(TEXT("[VERIFY] AC-UE-INTEGRATION-TERRAIN-004: generated 32 m SharedWorld heightfield for 64 Slots"));
	Generator.Destroy();
	Scene.Reset();
	return bGeometryUnchanged && bResetWindowMonotonic && bSharedOwner
		&& bRuntimeMesh && bRuntimeSamples;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSharedRandomGrid64SlotGenerationTest,
	"UERL.Integration.Worker.Terrain.AC_UE_INTEGRATION_TERRAIN_005.SharedRandomGrid64Slots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSharedRandomGrid64SlotGenerationTest::RunTest(const FString& Parameters)
{
	constexpr int32 SlotSide = 8;
	constexpr int32 NumSlots = SlotSide * SlotSide;
	constexpr int32 GridSide = 80;
	constexpr double SlotSpan = 24.0;

	FUERLTerrainConfig Config;
	Config.NumLevels = 1;
	Config.CellSize[0] = 30.0;
	Config.CellSize[1] = 30.0;
	Config.BorderWidth = 1.0;
	FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
	Tier.Level = 0;
	Tier.Primitive = EUERLTerrainPrimitive::Boxes;
	Tier.Seed = 20260902;
	Tier.PlatformWidth = 0.8;
	Tier.Params = MakeShared<FJsonObject>();
	Tier.Params->SetNumberField(TEXT("grid_width"), 0.4);
	Tier.Params->SetArrayField(TEXT("grid_height_range"), {
		MakeShared<FJsonValueNumber>(0.01), MakeShared<FJsonValueNumber>(0.10) });
	Tier.Params->SetBoolField(TEXT("holes"), false);
	Tier.Params->SetStringField(TEXT("generator"), TEXT("random_grid"));
	Tier.Params->SetNumberField(TEXT("difficulty"), 1.0);

	TArray<FVector> SlotOrigins;
	SlotOrigins.Reserve(NumSlots);
	for (int32 Row = 0; Row < SlotSide; ++Row)
	{
		for (int32 Column = 0; Column < SlotSide; ++Column)
		{
			const double X = -SlotSpan * 0.5 + static_cast<double>(Column) * SlotSpan / (SlotSide - 1);
			const double Y = -SlotSpan * 0.5 + static_cast<double>(Row) * SlotSpan / (SlotSide - 1);
			SlotOrigins.Add(FVector(X * 100.0, Y * 100.0, 0.0));
		}
	}

	TArray<FUERLTerrainTierPlan> ExpectedPlans;
	FString Error;
	const bool bExpectedBuilt = FUERLTerrainGenerator::BuildSharedPlan(
		Config, SlotOrigins, ExpectedPlans, Error);
	TestTrue(FString::Printf(TEXT("canonical random-grid SharedWorld plan builds: %s"), *Error),
		bExpectedBuilt);
	if (!bExpectedBuilt || ExpectedPlans.Num() != 1 || ExpectedPlans[0].Boxes.Num() != GridSide * GridSide
		|| ExpectedPlans[0].CollisionMeshes.Num() != 1 || ExpectedPlans[0].Samples.Num() != NumSlots)
	{
		return false;
	}

	const FUERLTerrainTierPlan& ExpectedPlan = ExpectedPlans[0];
	const FUERLTerrainMeshSpec& ExpectedCollision = ExpectedPlan.CollisionMeshes[0];
	double MinX = TNumericLimits<double>::Max();
	double MaxX = TNumericLimits<double>::Lowest();
	double MinY = TNumericLimits<double>::Max();
	double MaxY = TNumericLimits<double>::Lowest();
	double MinHeight = TNumericLimits<double>::Max();
	double MaxHeight = TNumericLimits<double>::Lowest();
	double MaxAdjacentDelta = 0.0;
	for (int32 Index = 0; Index < ExpectedPlan.Boxes.Num(); ++Index)
	{
		const FUERLTerrainBoxSpec& Box = ExpectedPlan.Boxes[Index];
		const double Height = Box.ExtentMeters.Z * 2.0;
		MinX = FMath::Min(MinX, Box.CenterMeters.X - Box.ExtentMeters.X);
		MaxX = FMath::Max(MaxX, Box.CenterMeters.X + Box.ExtentMeters.X);
		MinY = FMath::Min(MinY, Box.CenterMeters.Y - Box.ExtentMeters.Y);
		MaxY = FMath::Max(MaxY, Box.CenterMeters.Y + Box.ExtentMeters.Y);
		MinHeight = FMath::Min(MinHeight, Height);
		MaxHeight = FMath::Max(MaxHeight, Height);
		const int32 X = Index % GridSide;
		const int32 Y = Index / GridSide;
		if (X + 1 < GridSide)
		{
			MaxAdjacentDelta = FMath::Max(
				MaxAdjacentDelta,
				FMath::Abs(Height - ExpectedPlan.Boxes[Index + 1].ExtentMeters.Z * 2.0));
		}
		if (Y + 1 < GridSide)
		{
			MaxAdjacentDelta = FMath::Max(
				MaxAdjacentDelta,
				FMath::Abs(Height - ExpectedPlan.Boxes[Index + GridSide].ExtentMeters.Z * 2.0));
		}
	}
	const bool bRasterEnvelope = FMath::IsNearlyEqual(MinX, -16.0)
		&& FMath::IsNearlyEqual(MaxX, 16.0)
		&& FMath::IsNearlyEqual(MinY, -16.0)
		&& FMath::IsNearlyEqual(MaxY, 16.0)
		&& MinHeight >= 0.01 - 1.0e-9
		&& MaxHeight > 0.08
		&& MaxHeight <= 0.10 + 1.0e-9;
	TestTrue(TEXT("canonical random-grid plan has a gap-free 32 m raster and 0.10 m cap"), bRasterEnvelope);
	TestTrue(TEXT("canonical random-grid plan stays within the 0.225 step-slope cap"),
		MaxAdjacentDelta <= 0.09 + 1.0e-9);

	FString CollisionError;
	FUERLSlotCollisionPlan CollisionPlan;
	if (!CollisionPlan.Compile(NumSlots, EUERLEnvironmentCollisionScope::SharedWorld, CollisionError))
	{
		AddError(CollisionError);
		return false;
	}
	TArray<FUERLSlotContext> Slots;
	Slots.Reserve(NumSlots);
	for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
	{
		FUERLSlotContext& Slot = Slots.AddDefaulted_GetRef();
		Slot.SlotId = SlotId;
		Slot.Origin = SlotOrigins[SlotId];
		Slot.CollisionProfile = CollisionPlan.Profile(SlotId);
	}

	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(false)
			.SetTransactional(false)
			.SetEditor(false));
	FUERLTerrainGenerator Generator;
	if (!Generator.GenerateShared(*Scene->GetWorld(), Config, Slots, Error))
	{
		AddError(Error);
		return false;
	}

	AActor* SharedOwner = nullptr;
	bool bSharedOwner = true;
	for (FUERLSlotContext& Slot : Slots)
	{
		bSharedOwner = bSharedOwner && Slot.TerrainQueryActors.Num() == 1;
		if (SharedOwner == nullptr && Slot.TerrainQueryActors.Num() == 1)
		{
			SharedOwner = Slot.TerrainQueryActors[0].Get();
		}
		bSharedOwner = bSharedOwner && Slot.TerrainQueryActors.Num() == 1
			&& Slot.TerrainQueryActors[0].Get() == SharedOwner;
	}
	TestTrue(TEXT("all 64 Slots reference one generated random-grid SharedWorld owner"),
		bSharedOwner && SharedOwner != nullptr);
	if (SharedOwner == nullptr)
	{
		Generator.Destroy();
		return false;
	}

	const UHierarchicalInstancedStaticMeshComponent* Visual =
		SharedOwner->FindComponentByClass<UHierarchicalInstancedStaticMeshComponent>();
	TestNotNull(TEXT("random-grid SharedWorld visual instances exist"), Visual);
	bool bRuntimeVisual = Visual != nullptr;
	if (Visual)
	{
		bRuntimeVisual = Visual->GetInstanceCount() == ExpectedPlan.Boxes.Num();
		for (int32 BoxIndex = 0; BoxIndex < ExpectedPlan.Boxes.Num() && bRuntimeVisual; ++BoxIndex)
		{
			FTransform InstanceTransform;
			bRuntimeVisual = Visual->GetInstanceTransform(BoxIndex, InstanceTransform, true);
			if (bRuntimeVisual)
			{
				const FUERLTerrainBoxSpec& ExpectedBox = ExpectedPlan.Boxes[BoxIndex];
				bRuntimeVisual = InstanceTransform.GetLocation().Equals(
					ExpectedBox.CenterMeters * 100.0, 1.0e-3)
					&& InstanceTransform.GetScale3D().Equals(ExpectedBox.ExtentMeters * 2.0, 1.0e-3);
			}
		}
	}
	TestTrue(TEXT("runtime random-grid visual raster preserves every generated cell"), bRuntimeVisual);

	UProceduralMeshComponent* Collision =
		SharedOwner->FindComponentByClass<UProceduralMeshComponent>();
	TestNotNull(TEXT("random-grid SharedWorld collision component exists"), Collision);
	FProcMeshSection* Section = Collision ? Collision->GetProcMeshSection(0) : nullptr;
	TestNotNull(TEXT("random-grid SharedWorld collision section exists"), Section);
	const bool bRuntimeCollision = Section != nullptr
		&& Section->ProcVertexBuffer.Num() == ExpectedCollision.VerticesMeters.Num()
		&& Section->ProcIndexBuffer.Num() == ExpectedCollision.Triangles.Num();
	TestTrue(TEXT("runtime random-grid collision preserves the merged no-gap mesh"), bRuntimeCollision);

	double ResetMin = TNumericLimits<double>::Max();
	double ResetMax = TNumericLimits<double>::Lowest();
	bool bRuntimeSamples = Generator.TierPlans().Num() == 1
		&& Generator.TierPlans()[0].Samples.Num() == NumSlots;
	if (bRuntimeSamples)
	{
		for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
		{
			const FUERLTerrainSpawnSample& ExpectedSample = ExpectedPlan.Samples[SlotId];
			const FUERLTerrainSpawnSample& ActualSample = Generator.TierPlans()[0].Samples[SlotId];
			ResetMin = FMath::Min(ResetMin, ActualSample.GroundHeight / 100.0);
			ResetMax = FMath::Max(ResetMax, ActualSample.GroundHeight / 100.0);
			bRuntimeSamples = bRuntimeSamples
				&& FMath::IsNearlyEqual(ActualSample.GroundHeight, ExpectedSample.GroundHeight)
				&& ActualSample.Origin.Equals(ExpectedSample.Origin, 1.0e-3);
		}
	}
	TestTrue(TEXT("runtime random-grid preserves all 64 reset top-surface samples"), bRuntimeSamples);
	AddInfo(FString::Printf(
		TEXT("[VERIFY] SharedWorld random-grid: boxes=%d height=%.4f..%.4f max_adjacent_delta=%.4f step_slope=%.4f reset_height=%.4f..%.4f"),
		ExpectedPlan.Boxes.Num(), MinHeight, MaxHeight, MaxAdjacentDelta, MaxAdjacentDelta / 0.4,
		ResetMin, ResetMax));
	Generator.Destroy();
	Scene.Reset();
	return bRasterEnvelope && MaxAdjacentDelta <= 0.09 + 1.0e-9
		&& bSharedOwner && bRuntimeVisual && bRuntimeCollision && bRuntimeSamples;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLFinalTerrainCollisionSubmissionTest,
	"UERL.Integration.Worker.Terrain.AC_UE_INTEGRATION_TERRAIN_003.FinalCollisionSubmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLFinalTerrainCollisionSubmissionTest::RunTest(const FString& Parameters)
{
	FString Error;
	FString Unused;

	FUERLTerrainConfig Config;
	Config.NumLevels = 4;
	Config.CellSize[0] = 2.0;
	Config.CellSize[1] = 2.0;
	Config.BorderWidth = 0.25;

	FUERLTerrainTierConfig& Plane = Config.Tiers.AddDefaulted_GetRef();
	Plane.Level = 0;
	Plane.Primitive = EUERLTerrainPrimitive::Plane;
	Plane.Params = MakeShared<FJsonObject>();

	const auto AddHeightfieldTier = [&Config](int32 Level, int32 Seed)
	{
		FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
		Tier.Level = Level;
		Tier.Primitive = EUERLTerrainPrimitive::Heightfield;
		Tier.Seed = Seed;
		Tier.PlatformWidth = 0.5;
		Tier.Params = MakeShared<FJsonObject>();
		Tier.Params->SetArrayField(TEXT("noise_range"), {
			MakeShared<FJsonValueNumber>(0.05), MakeShared<FJsonValueNumber>(0.15) });
		Tier.Params->SetNumberField(TEXT("noise_step"), 0.01);
		Tier.Params->SetNumberField(TEXT("horizontal_scale"), 0.25);
		Tier.Params->SetNumberField(TEXT("vertical_scale"), 0.01);
		Tier.Params->SetNumberField(TEXT("downsampled_scale"), 0.5);
	};

	AddHeightfieldTier(1, 11);

	FUERLTerrainTierConfig& Boxes = Config.Tiers.AddDefaulted_GetRef();
	Boxes.Level = 2;
	Boxes.Primitive = EUERLTerrainPrimitive::Boxes;
	Boxes.Seed = 22;
	Boxes.Params = MakeShared<FJsonObject>();
	Boxes.Params->SetNumberField(TEXT("grid_width"), 0.5);
	Boxes.Params->SetArrayField(TEXT("grid_height_range"), {
		MakeShared<FJsonValueNumber>(0.1), MakeShared<FJsonValueNumber>(0.2) });
	Boxes.Params->SetBoolField(TEXT("holes"), false);

	AddHeightfieldTier(3, 33);

	const TArray<FVector> SlotOrigins = { FVector::ZeroVector };
	TArray<FUERLTerrainTierPlan> SharedPlans;
	if (!FUERLTerrainGenerator::BuildSharedPlan(Config, SlotOrigins, SharedPlans, Unused))
	{
		AddError(Unused);
		return false;
	}

	int32 ExpectedVisualInstances = 0;
	int32 ExpectedCollisionVertices = 0;
	int32 ExpectedCollisionIndices = 0;
	int32 ExpectedHeightfieldVertices = 0;
	int32 ExpectedHeightfieldIndices = 0;
	for (const FUERLTerrainTierPlan& Plan : SharedPlans)
	{
		ExpectedVisualInstances += Plan.Boxes.Num();
		for (const FUERLTerrainMeshSpec& Mesh : Plan.CollisionMeshes)
		{
			ExpectedCollisionVertices += Mesh.VerticesMeters.Num();
			ExpectedCollisionIndices += Mesh.Triangles.Num();
		}
		for (const FUERLTerrainMeshSpec& Mesh : Plan.Meshes)
		{
			ExpectedHeightfieldVertices += Mesh.VerticesMeters.Num();
			ExpectedHeightfieldIndices += Mesh.Triangles.Num();
		}
	}

	FUERLSlotCollisionPlan CollisionPlan;
	if (!CollisionPlan.Compile(1, EUERLEnvironmentCollisionScope::SharedWorld, Error))
	{
		AddError(Error);
		return false;
	}
	TArray<FUERLSlotContext> Slots = { FUERLSlotContext{ 0, FVector::ZeroVector } };
	Slots[0].CollisionProfile = CollisionPlan.Profile(0);
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.ShouldSimulatePhysics(false)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld& World = *Scene->GetWorld();
	FUERLTerrainGenerator Generator;
	if (!Generator.GenerateShared(World, Config, Slots, Error))
	{
		AddError(Error);
		return false;
	}

	TestEqual(TEXT("shared terrain keeps one atlas owner"), Slots[0].TerrainQueryActors.Num(), 1);
	if (Slots[0].TerrainQueryActors.IsEmpty())
	{
		Generator.Destroy();
		return false;
	}
	AActor* Owner = Slots[0].TerrainQueryActors[0].Get();
	TestNotNull(TEXT("shared terrain atlas owner is valid"), Owner);
	if (!Owner)
	{
		Generator.Destroy();
		return false;
	}

	TArray<UProceduralMeshComponent*> MeshComponents;
	Owner->GetComponents(MeshComponents);
	TestEqual(TEXT("shared mixed terrain has one component per collision display layer"),
		MeshComponents.Num(), 2);

	int32 HiddenComponentCount = 0;
	int32 VisibleComponentCount = 0;
	int32 HiddenVertices = 0;
	int32 HiddenIndices = 0;
	int32 VisibleVertices = 0;
	int32 VisibleIndices = 0;
	bool bIndicesInRange = true;
	bool bIndicesFormTriangles = true;
	for (UProceduralMeshComponent* Component : MeshComponents)
	{
		TestNotNull(TEXT("final terrain mesh component is valid"), Component);
		if (!Component)
		{
			continue;
		}
		TestEqual(TEXT("final terrain mesh uses one section"), Component->GetNumSections(), 1);
		FProcMeshSection* Section = Component->GetProcMeshSection(0);
		TestNotNull(TEXT("final terrain section is available"), Section);
		if (!Section)
		{
			continue;
		}
		for (const uint32 Index : Section->ProcIndexBuffer)
		{
			bIndicesInRange = bIndicesInRange && Index < static_cast<uint32>(Section->ProcVertexBuffer.Num());
		}
		if (Component->IsVisible())
		{
			++VisibleComponentCount;
			VisibleVertices += Section->ProcVertexBuffer.Num();
			VisibleIndices += Section->ProcIndexBuffer.Num();
		}
		else
		{
			++HiddenComponentCount;
			HiddenVertices += Section->ProcVertexBuffer.Num();
			HiddenIndices += Section->ProcIndexBuffer.Num();
		}
		bIndicesFormTriangles = bIndicesFormTriangles && Section->ProcIndexBuffer.Num() % 3 == 0;
	}
	TestEqual(TEXT("one hidden boxes collision component exists"), HiddenComponentCount, 1);
	TestEqual(TEXT("one visible heightfield component exists"), VisibleComponentCount, 1);
	TestEqual(TEXT("hidden collision vertices are preserved after aggregation"),
		HiddenVertices, ExpectedCollisionVertices);
	TestEqual(TEXT("hidden collision indices are preserved after aggregation"),
		HiddenIndices, ExpectedCollisionIndices);
	TestEqual(TEXT("visible heightfield vertices are preserved after aggregation"),
		VisibleVertices, ExpectedHeightfieldVertices);
	TestEqual(TEXT("visible heightfield indices are preserved after aggregation"),
		VisibleIndices, ExpectedHeightfieldIndices);
	TestTrue(TEXT("aggregated terrain indices stay within the final vertex buffer"), bIndicesInRange);
	TestTrue(TEXT("aggregated terrain indices form complete triangles"), bIndicesFormTriangles);

	const UHierarchicalInstancedStaticMeshComponent* Visual =
		Owner->FindComponentByClass<UHierarchicalInstancedStaticMeshComponent>();
	TestNotNull(TEXT("shared terrain keeps its visual HISM"), Visual);
	if (Visual)
	{
		TestEqual(TEXT("visual HISM instance count is unchanged by collision aggregation"),
			Visual->GetInstanceCount(), ExpectedVisualInstances);
	}

	Generator.Destroy();
	Scene.Reset();
	AddInfo(TEXT("[VERIFY] AC-UE-INTEGRATION-TERRAIN-003: shared collision layers aggregate into one section each"));
	return bIndicesInRange && bIndicesFormTriangles
		&& HiddenComponentCount == 1 && VisibleComponentCount == 1;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLIndexedDiscreteCollisionMeshTest,
	"UERL.Unit.Worker.Terrain.IndexedDiscreteCollisionMesh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLIndexedDiscreteCollisionMeshTest::RunTest(const FString& Parameters)
{
	FString Error;

	FUERLTerrainConfig GridConfig;
	GridConfig.NumLevels = 1;
	GridConfig.CellSize[0] = 1.0;
	GridConfig.CellSize[1] = 1.0;
	GridConfig.BorderWidth = 0.0;
	FUERLTerrainTierConfig& GridTier = GridConfig.Tiers.AddDefaulted_GetRef();
	GridTier.Level = 0;
	GridTier.Primitive = EUERLTerrainPrimitive::Boxes;
	GridTier.Seed = 17;
	GridTier.Params = MakeShared<FJsonObject>();
	GridTier.Params->SetNumberField(TEXT("grid_width"), 0.5);
	GridTier.Params->SetArrayField(TEXT("grid_height_range"), {
		MakeShared<FJsonValueNumber>(0.1), MakeShared<FJsonValueNumber>(0.1) });
	GridTier.Params->SetBoolField(TEXT("holes"), false);

	TArray<FUERLTerrainTierPlan> GridPlans;
	TestTrue(TEXT("regular discrete grid builds"),
		FUERLTerrainGenerator::BuildSharedPlan(
			GridConfig, { FVector::ZeroVector }, GridPlans, Error));
	if (GridPlans.Num() != 1 || GridPlans[0].CollisionMeshes.Num() != 1)
	{
		AddError(Error);
		return false;
	}

	const FUERLTerrainMeshSpec& GridMesh = GridPlans[0].CollisionMeshes[0];
	TestEqual(TEXT("2x2 grid indexes top corners and reuses boundary vertices"),
		GridMesh.VerticesMeters.Num(), 24);
	TestEqual(TEXT("indexed grid keeps the original 24 triangles"),
		GridMesh.Triangles.Num(), 72);
	bool bGridIndicesInRange = true;
	for (const int32 Index : GridMesh.Triangles)
	{
		bGridIndicesInRange = bGridIndicesInRange
			&& Index >= 0 && Index < GridMesh.VerticesMeters.Num();
	}
	TestTrue(TEXT("indexed grid triangles stay within the vertex buffer"), bGridIndicesInRange);

	FUERLTerrainConfig HoleConfig;
	HoleConfig.NumLevels = 1;
	HoleConfig.CellSize[0] = 2.0;
	HoleConfig.CellSize[1] = 2.0;
	HoleConfig.BorderWidth = 0.0;
	FUERLTerrainTierConfig& HoleTier = HoleConfig.Tiers.AddDefaulted_GetRef();
	HoleTier.Level = 0;
	HoleTier.Primitive = EUERLTerrainPrimitive::Boxes;
	HoleTier.Seed = 29;
	HoleTier.PlatformWidth = 0.5;
	HoleTier.Params = MakeShared<FJsonObject>();
	HoleTier.Params->SetNumberField(TEXT("grid_width"), 0.5);
	HoleTier.Params->SetArrayField(TEXT("grid_height_range"), {
		MakeShared<FJsonValueNumber>(0.1), MakeShared<FJsonValueNumber>(0.9) });
	HoleTier.Params->SetBoolField(TEXT("holes"), true);

	TArray<FUERLTerrainTierPlan> HolePlans;
	TestTrue(TEXT("grid with holes and height steps builds"),
		FUERLTerrainGenerator::BuildSharedPlan(
			HoleConfig, { FVector::ZeroVector }, HolePlans, Error));
	if (HolePlans.Num() != 1 || HolePlans[0].CollisionMeshes.Num() != 1)
	{
		AddError(Error);
		return false;
	}

	const FUERLTerrainTierPlan& HolePlan = HolePlans[0];
	TestEqual(TEXT("hole grid keeps only the cross cells"), HolePlan.Boxes.Num(), 12);
	const FUERLTerrainMeshSpec& HoleMesh = HolePlan.CollisionMeshes[0];
	bool bHoleIndicesInRange = true;
	for (const int32 Index : HoleMesh.Triangles)
	{
		bHoleIndicesInRange = bHoleIndicesInRange
			&& Index >= 0 && Index < HoleMesh.VerticesMeters.Num();
	}
	TestTrue(TEXT("hole and step triangles stay within the vertex buffer"), bHoleIndicesInRange);

	FUERLTerrainConfig ExplicitConfig = GridConfig;
	ExplicitConfig.Tiers.Reset();
	FUERLTerrainTierConfig& ExplicitTier = ExplicitConfig.Tiers.AddDefaulted_GetRef();
	ExplicitTier.Level = 0;
	ExplicitTier.Primitive = EUERLTerrainPrimitive::Boxes;
	ExplicitTier.Params = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ExplicitBox = MakeShared<FJsonObject>();
	ExplicitBox->SetArrayField(TEXT("pose"), {
		MakeShared<FJsonValueNumber>(0.0), MakeShared<FJsonValueNumber>(0.0),
		MakeShared<FJsonValueNumber>(0.5), MakeShared<FJsonValueNumber>(0.0) });
	ExplicitBox->SetArrayField(TEXT("extent"), {
		MakeShared<FJsonValueNumber>(0.25), MakeShared<FJsonValueNumber>(0.25),
		MakeShared<FJsonValueNumber>(0.5) });
	ExplicitTier.Params->SetArrayField(TEXT("explicit"), {
		MakeShared<FJsonValueObject>(ExplicitBox) });

	TArray<FUERLTerrainTierPlan> ExplicitPlans;
	TestTrue(TEXT("explicit discrete box builds"),
		FUERLTerrainGenerator::BuildSharedPlan(
			ExplicitConfig, { FVector::ZeroVector }, ExplicitPlans, Error));
	if (ExplicitPlans.Num() != 1 || ExplicitPlans[0].CollisionMeshes.Num() != 1)
	{
		AddError(Error);
		return false;
	}

	const FUERLTerrainMeshSpec& ExplicitMesh = ExplicitPlans[0].CollisionMeshes[0];
	TestEqual(TEXT("explicit box keeps 12 triangles"), ExplicitMesh.Triangles.Num(), 36);
	TestEqual(TEXT("explicit box shares its 8 corner vertices"),
		ExplicitMesh.VerticesMeters.Num(), 8);
	bool bExplicitIndicesInRange = true;
	for (const int32 Index : ExplicitMesh.Triangles)
	{
		bExplicitIndicesInRange = bExplicitIndicesInRange
			&& Index >= 0 && Index < ExplicitMesh.VerticesMeters.Num();
	}
	TestTrue(TEXT("explicit box triangles stay within the vertex buffer"), bExplicitIndicesInRange);
	AddInfo(TEXT("[VERIFY] indexed discrete collision preserves triangles while reusing corner vertices"));
	return bGridIndicesInRange && bHoleIndicesInRange && bExplicitIndicesInRange;
}

#endif
