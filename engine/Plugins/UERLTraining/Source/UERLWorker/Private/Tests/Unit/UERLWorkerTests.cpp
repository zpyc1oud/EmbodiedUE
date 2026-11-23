#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include "PhysicsEngine/PhysicsSettings.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UERLBatchBinding.h"
#include "UERLFrameGate.h"
#include "UERLSafetyMonitor.h"
#include "UERLTerrainGenerator.h"
#include "UERLSessionSubsystem.h"
#include "UERLWorkerRuntime.h"

#include "Engine/Engine.h"
#include "Misc/App.h"

#include <limits>

#if WITH_EDITOR
#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Tests/AutomationEditorCommon.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FUERLTerrainConfig MakeRandomGridConfig(
		double CellSize,
		double BorderWidth,
		double GridWidth,
		double MinHeight,
		double MaxHeight,
		double Difficulty,
		uint64 Seed)
	{
		FUERLTerrainConfig Config;
		Config.NumLevels = 1;
		Config.CellSize[0] = CellSize;
		Config.CellSize[1] = CellSize;
		Config.BorderWidth = BorderWidth;
		FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
		Tier.Level = 0;
		Tier.Primitive = EUERLTerrainPrimitive::Boxes;
		Tier.Seed = Seed;
		Tier.PlatformWidth = 0.8;
		Tier.Params = MakeShared<FJsonObject>();
		Tier.Params->SetNumberField(TEXT("grid_width"), GridWidth);
		Tier.Params->SetArrayField(TEXT("grid_height_range"), {
			MakeShared<FJsonValueNumber>(MinHeight), MakeShared<FJsonValueNumber>(MaxHeight) });
		Tier.Params->SetBoolField(TEXT("holes"), false);
		Tier.Params->SetStringField(TEXT("generator"), TEXT("random_grid"));
		Tier.Params->SetNumberField(TEXT("difficulty"), Difficulty);
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLVariableDtWorkerContractTest,
	"UERL.Unit.Worker.VariableDt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLVariableDtWorkerContractTest::RunTest(const FString& Parameters)
{
	FUERLWorkerProjection Projection;
	Projection.NumSlots = 2;
	Projection.ActionWidth = 1;
	Projection.StateWidth = 1;
	Projection.CommandWidth = 1;
	Projection.WorldMap = TEXT("/Game/Maps/Test");
	Projection.EnvironmentId = FName(TEXT("test.environment"));
	Projection.RobotId = FName(TEXT("test.robot"));
	Projection.PhysicsDt = 0.005;
	Projection.DecimationMin = 1;
	Projection.DecimationMax = 7;
	TestTrue(TEXT("variable decimation projection is valid"), Projection.IsValid());

	FUERLWorkerProjection Reversed = Projection;
	Reversed.DecimationMin = 7;
	Reversed.DecimationMax = 1;
	TestFalse(TEXT("reversed decimation range is rejected"), Reversed.IsWorkerConfigValid());
	FUERLWorkerProjection Zero = Projection;
	Zero.DecimationMin = 0;
	TestFalse(TEXT("zero decimation range is rejected"), Zero.IsWorkerConfigValid());
	AddInfo(TEXT("[VERIFY] AC_UE_INT_WORKER_DT_001/003: Worker projection validates the inclusive decimation range"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTerrainGeneratorTest,
	"UERL.Unit.Worker.TerrainGenerator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTerrainGeneratorTest::RunTest(const FString& Parameters)
{
	auto MakeHeightfieldParams = [](double DownsampledScale)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetArrayField(TEXT("noise_range"), {
			MakeShared<FJsonValueNumber>(0.05), MakeShared<FJsonValueNumber>(0.4) });
		Params->SetNumberField(TEXT("noise_step"), 0.02);
		Params->SetNumberField(TEXT("horizontal_scale"), 0.25);
		Params->SetNumberField(TEXT("vertical_scale"), 0.02);
		Params->SetNumberField(TEXT("downsampled_scale"), DownsampledScale);
		return Params;
	};

	FUERLTerrainConfig Config;
	Config.NumLevels = 3;
	Config.CellSize[0] = 4.0;
	Config.CellSize[1] = 4.0;
	Config.Tiers.Reserve(Config.NumLevels);

	FUERLTerrainTierConfig& Plane = Config.Tiers.AddDefaulted_GetRef();
	Plane.Level = 0;
	Plane.Primitive = EUERLTerrainPrimitive::Plane;
	Plane.Seed = 11;
	Plane.Params = MakeShared<FJsonObject>();

	FUERLTerrainTierConfig& Heightfield = Config.Tiers.AddDefaulted_GetRef();
	Heightfield.Level = 1;
	Heightfield.Primitive = EUERLTerrainPrimitive::Heightfield;
	Heightfield.Seed = 22;
	Heightfield.PlatformWidth = 1.0;
	Heightfield.Params = MakeHeightfieldParams(0.25);

	FUERLTerrainTierConfig& Boxes = Config.Tiers.AddDefaulted_GetRef();
	Boxes.Level = 2;
	Boxes.Primitive = EUERLTerrainPrimitive::Boxes;
	Boxes.Seed = 33;
	Boxes.PlatformWidth = 1.0;
	Boxes.Params = MakeShared<FJsonObject>();
	Boxes.Params->SetNumberField(TEXT("grid_width"), 0.5);
	Boxes.Params->SetArrayField(TEXT("grid_height_range"), {
		MakeShared<FJsonValueNumber>(0.025), MakeShared<FJsonValueNumber>(0.1) });
	Boxes.Params->SetBoolField(TEXT("holes"), false);

	const TArray<FVector> SlotOrigins = { FVector::ZeroVector, FVector(0.0, 400.0, 0.0) };
	TArray<FUERLTerrainTierPlan> First;
	TArray<FUERLTerrainTierPlan> Second;
	FString Error;
	const bool bFirstBuild = FUERLTerrainGenerator::BuildPlan(Config, SlotOrigins, First, Error);
	const bool bSecondBuild = FUERLTerrainGenerator::BuildPlan(Config, SlotOrigins, Second, Error);
	TestTrue(TEXT("all three terrain primitives build"), bFirstBuild);
	TestTrue(TEXT("same terrain input builds twice"), bSecondBuild);
	if (!bFirstBuild || !bSecondBuild || First.Num() != Second.Num() || First.Num() < 3)
	{
		AddError(TEXT("deterministic terrain plans have different tier counts"));
		return false;
	}
	bool bEqual = true;
	for (int32 TierIndex = 0; TierIndex < First.Num(); ++TierIndex)
	{
		const FUERLTerrainTierPlan& A = First[TierIndex];
		const FUERLTerrainTierPlan& B = Second[TierIndex];
		bEqual = bEqual && A.Level == B.Level && A.Samples.Num() == B.Samples.Num()
			&& A.Boxes.Num() == B.Boxes.Num() && A.Meshes.Num() == B.Meshes.Num();
		for (int32 SampleIndex = 0; SampleIndex < A.Samples.Num() && bEqual; ++SampleIndex)
		{
			bEqual = A.Samples[SampleIndex].Origin == B.Samples[SampleIndex].Origin
				&& A.Samples[SampleIndex].GroundHeight == B.Samples[SampleIndex].GroundHeight
				&& A.Samples[SampleIndex].GroundNormal == B.Samples[SampleIndex].GroundNormal;
		}
		for (int32 BoxIndex = 0; BoxIndex < A.Boxes.Num() && bEqual; ++BoxIndex)
		{
			bEqual = A.Boxes[BoxIndex].CenterMeters == B.Boxes[BoxIndex].CenterMeters
				&& A.Boxes[BoxIndex].ExtentMeters == B.Boxes[BoxIndex].ExtentMeters
				&& A.Boxes[BoxIndex].YawRadians == B.Boxes[BoxIndex].YawRadians;
		}
		for (int32 MeshIndex = 0; MeshIndex < A.Meshes.Num() && bEqual; ++MeshIndex)
		{
			bEqual = A.Meshes[MeshIndex].VerticesMeters == B.Meshes[MeshIndex].VerticesMeters
				&& A.Meshes[MeshIndex].Normals == B.Meshes[MeshIndex].Normals
				&& A.Meshes[MeshIndex].Triangles == B.Meshes[MeshIndex].Triangles;
		}
	}
	TestTrue(TEXT("same seed and params produce identical plans"), bEqual);
	if (!TestEqual(TEXT("plane uses solid geometry"), First[0].Boxes.Num(), 2)
		|| !TestEqual(TEXT("heightfield produces one mesh per Slot"), First[1].Meshes.Num(), 2))
	{
		return false;
	}
	TestEqual(TEXT("first plane box belongs to Slot 0"), First[0].Boxes[0].SlotId, 0);
	TestEqual(TEXT("second plane box belongs to Slot 1"), First[0].Boxes[1].SlotId, 1);
	TestEqual(TEXT("first heightfield belongs to Slot 0"), First[1].Meshes[0].SlotId, 0);
	TestEqual(TEXT("second heightfield belongs to Slot 1"), First[1].Meshes[1].SlotId, 1);
	TestTrue(TEXT("boxes produce deterministic solid geometry"), First[2].Boxes.Num() > 0);

	const TArray<FVector> SharedSpawnOrigins = {
		FVector(-100.0, 0.0, 0.0), FVector(100.0, 0.0, 0.0),
	};
	TArray<FUERLTerrainTierPlan> SharedPlans;
	FString SharedError;
	const bool bSharedBuilt = FUERLTerrainGenerator::BuildSharedPlan(
		Config, SharedSpawnOrigins, SharedPlans, SharedError);
	TestTrue(FString::Printf(TEXT("shared terrain atlas builds: %s"), *SharedError), bSharedBuilt);
	if (bSharedBuilt && SharedPlans.Num() == 3)
	{
		TestEqual(TEXT("shared plane generates one geometry owner"), SharedPlans[0].Boxes.Num(), 1);
		if (!TestEqual(TEXT("shared heightfield generates one mesh"), SharedPlans[1].Meshes.Num(), 1))
		{
			return false;
		}
		TestEqual(TEXT("shared tier keeps one spawn sample per Slot"), SharedPlans[1].Samples.Num(), 2);
		TestEqual(TEXT("shared geometry has no Slot owner"),
			SharedPlans[1].Meshes[0].SlotId, INDEX_NONE);
	}

	// Regression: changing downsampled_scale must change the spatial frequency
	// while preserving the fine output grid resolution.
	FUERLTerrainConfig CoarseSampleConfig = Config;
	CoarseSampleConfig.Tiers[1].Params = MakeHeightfieldParams(0.5);
	TArray<FUERLTerrainTierPlan> CoarseSamplePlans;
	FString CoarseSampleError;
	const bool bCoarseSampleBuilt = FUERLTerrainGenerator::BuildPlan(
		CoarseSampleConfig, SlotOrigins, CoarseSamplePlans, CoarseSampleError);
	TestTrue(FString::Printf(TEXT("coarse-sampled heightfield builds: %s"), *CoarseSampleError), bCoarseSampleBuilt);
	if (bCoarseSampleBuilt && First.Num() > 1 && CoarseSamplePlans.Num() > 1)
	{
		const FUERLTerrainMeshSpec& BaseMesh = First[1].Meshes[0];
		if (!TestEqual(TEXT("coarse heightfield has two Slot meshes"), CoarseSamplePlans[1].Meshes.Num(), 2))
		{
			return false;
		}
		const FUERLTerrainMeshSpec& CoarseSampleMesh = CoarseSamplePlans[1].Meshes[0];
		if (!TestEqual(TEXT("downsampling preserves fine vertex count"),
			BaseMesh.VerticesMeters.Num(), CoarseSampleMesh.VerticesMeters.Num()))
		{
			return false;
		}
		bool bHeightfieldChanged = false;
		for (int32 Index = 0; Index < BaseMesh.VerticesMeters.Num(); ++Index)
		{
			if (!FMath::IsNearlyEqual(BaseMesh.VerticesMeters[Index].Z, CoarseSampleMesh.VerticesMeters[Index].Z))
			{
				bHeightfieldChanged = true;
				break;
			}
		}
		TestTrue(TEXT("downsampled_scale changes the generated spatial field"), bHeightfieldChanged);
	}

	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-TERRAIN-001: isolated and shared terrain plans are deterministic"));
	return bEqual && bSharedBuilt;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLHeightfieldResetWindowTest,
	"UERL.Unit.Worker.Terrain.HeightfieldResetWindow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLHeightfieldResetWindowTest::RunTest(const FString& Parameters)
{
	FUERLTerrainConfig NarrowConfig;
	NarrowConfig.NumLevels = 1;
	NarrowConfig.CellSize[0] = 4.0;
	NarrowConfig.CellSize[1] = 4.0;
	NarrowConfig.BorderWidth = 1.0;
	FUERLTerrainTierConfig& NarrowTier = NarrowConfig.Tiers.AddDefaulted_GetRef();
	NarrowTier.Level = 0;
	NarrowTier.Primitive = EUERLTerrainPrimitive::Heightfield;
	NarrowTier.Seed = 22;
	NarrowTier.PlatformWidth = 0.0;
	NarrowTier.Params = MakeShared<FJsonObject>();
	NarrowTier.Params->SetArrayField(TEXT("noise_range"), {
		MakeShared<FJsonValueNumber>(-0.1), MakeShared<FJsonValueNumber>(0.1) });
	NarrowTier.Params->SetNumberField(TEXT("noise_step"), 0.02);
	NarrowTier.Params->SetNumberField(TEXT("horizontal_scale"), 0.25);
	NarrowTier.Params->SetNumberField(TEXT("vertical_scale"), 0.02);
	NarrowTier.Params->SetNumberField(TEXT("downsampled_scale"), 0.5);

	FUERLTerrainConfig WideConfig = NarrowConfig;
	WideConfig.Tiers[0].PlatformWidth = 1.5;
	const TArray<FVector> SlotOrigins = { FVector::ZeroVector };
	TArray<FUERLTerrainTierPlan> NarrowPlans;
	TArray<FUERLTerrainTierPlan> WidePlans;
	FString NarrowError;
	FString WideError;
	const bool bNarrowBuilt = FUERLTerrainGenerator::BuildPlan(
		NarrowConfig, SlotOrigins, NarrowPlans, NarrowError);
	const bool bWideBuilt = FUERLTerrainGenerator::BuildPlan(
		WideConfig, SlotOrigins, WidePlans, WideError);
	TestTrue(FString::Printf(TEXT("heightfield without reset window builds: %s"), *NarrowError),
		bNarrowBuilt);
	TestTrue(FString::Printf(TEXT("heightfield with wider reset window builds: %s"), *WideError),
		bWideBuilt);
	if (!bNarrowBuilt || !bWideBuilt || NarrowPlans.Num() != 1 || WidePlans.Num() != 1)
	{
		return false;
	}
	if (NarrowPlans[0].Meshes.Num() != 1 || WidePlans[0].Meshes.Num() != 1)
	{
		AddError(TEXT("heightfield reset-window plans did not produce one mesh"));
		return false;
	}

	const FUERLTerrainMeshSpec& NarrowMesh = NarrowPlans[0].Meshes[0];
	const FUERLTerrainMeshSpec& WideMesh = WidePlans[0].Meshes[0];
	const bool bGeometryUnchanged = NarrowMesh.VerticesMeters == WideMesh.VerticesMeters
		&& NarrowMesh.Normals == WideMesh.Normals
		&& NarrowMesh.Triangles == WideMesh.Triangles;
	TestTrue(TEXT("reset window does not flatten or modify the heightfield"), bGeometryUnchanged);
	return bGeometryUnchanged;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLDiscreteRandomGridDeterminismTest,
	"UERL.Unit.Worker.DiscreteRandomGridDeterminism",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLDiscreteRandomGridDeterminismTest::RunTest(const FString& Parameters)
{
	const FUERLTerrainConfig Config = MakeRandomGridConfig(
		12.0, 1.0, 0.1, 0.01, 0.10, 1.0, 20260831);
	const TArray<FVector> SlotOrigins = { FVector::ZeroVector };
	TArray<FUERLTerrainTierPlan> First;
	TArray<FUERLTerrainTierPlan> Second;
	FString FirstError;
	FString SecondError;
	const bool bFirstBuilt = FUERLTerrainGenerator::BuildPlan(
		Config, SlotOrigins, First, FirstError);
	const bool bSecondBuilt = FUERLTerrainGenerator::BuildPlan(
		Config, SlotOrigins, Second, SecondError);
	TestTrue(FString::Printf(TEXT("first random-grid build: %s"), *FirstError), bFirstBuilt);
	TestTrue(FString::Printf(TEXT("second random-grid build: %s"), *SecondError), bSecondBuilt);
	if (!bFirstBuilt || !bSecondBuilt || First.Num() != 1 || Second.Num() != 1)
	{
		return false;
	}

	const FUERLTerrainTierPlan& FirstPlan = First[0];
	const FUERLTerrainTierPlan& SecondPlan = Second[0];
	bool bEqual = FirstPlan.Level == SecondPlan.Level
		&& FirstPlan.Samples.Num() == SecondPlan.Samples.Num()
		&& FirstPlan.Boxes.Num() == SecondPlan.Boxes.Num();
	for (int32 Index = 0; Index < FirstPlan.Samples.Num() && bEqual; ++Index)
	{
		bEqual = FirstPlan.Samples[Index].Origin == SecondPlan.Samples[Index].Origin
			&& FirstPlan.Samples[Index].GroundHeight == SecondPlan.Samples[Index].GroundHeight
			&& FirstPlan.Samples[Index].GroundNormal == SecondPlan.Samples[Index].GroundNormal;
	}
	for (int32 Index = 0; Index < FirstPlan.Boxes.Num() && bEqual; ++Index)
	{
		bEqual = FirstPlan.Boxes[Index].CenterMeters == SecondPlan.Boxes[Index].CenterMeters
			&& FirstPlan.Boxes[Index].ExtentMeters == SecondPlan.Boxes[Index].ExtentMeters
			&& FirstPlan.Boxes[Index].YawRadians == SecondPlan.Boxes[Index].YawRadians;
	}
	bEqual = bEqual && FirstPlan.CollisionMeshes.Num() == SecondPlan.CollisionMeshes.Num();
	for (int32 Index = 0; Index < FirstPlan.CollisionMeshes.Num() && bEqual; ++Index)
	{
		bEqual = FirstPlan.CollisionMeshes[Index].SlotId == SecondPlan.CollisionMeshes[Index].SlotId
			&& FirstPlan.CollisionMeshes[Index].VerticesMeters
				== SecondPlan.CollisionMeshes[Index].VerticesMeters
			&& FirstPlan.CollisionMeshes[Index].Normals == SecondPlan.CollisionMeshes[Index].Normals
			&& FirstPlan.CollisionMeshes[Index].Triangles == SecondPlan.CollisionMeshes[Index].Triangles;
	}
	TestTrue(TEXT("same random-grid seed and params produce identical plans"), bEqual);
	return bEqual;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLDiscreteRandomGridTerrainTest,
	"UERL.Unit.Worker.DiscreteRandomGridTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLDiscreteRandomGridTerrainTest::RunTest(const FString& Parameters)
{
	FUERLTerrainConfig Config = MakeRandomGridConfig(
		12.0, 1.0, 0.1, 0.01, 0.10, 1.0, 20260831);
	TArray<FUERLTerrainTierPlan> Plans;
	FString Error;
	const bool bBuilt = FUERLTerrainGenerator::BuildPlan(
		Config, { FVector::ZeroVector }, Plans, Error);
	TestTrue(FString::Printf(TEXT("random-grid boxes build: %s"), *Error), bBuilt);
	if (!bBuilt || Plans.Num() != 1)
	{
		return false;
	}

	const TArray<FUERLTerrainBoxSpec>& Boxes = Plans[0].Boxes;
	const int32 GridX = 140;
	const int32 GridY = 140;
	TestEqual(TEXT("0.1 m raster covers every cell"), Boxes.Num(), GridX * GridY);
	if (Boxes.Num() != GridX * GridY)
	{
		return false;
	}

	double MinX = TNumericLimits<double>::Max();
	double MaxX = TNumericLimits<double>::Lowest();
	double MinY = TNumericLimits<double>::Max();
	double MaxY = TNumericLimits<double>::Lowest();
	double MinHeight = TNumericLimits<double>::Max();
	double MaxHeight = TNumericLimits<double>::Lowest();
	double SumA = 0.0;
	double SumB = 0.0;
	double SumAA = 0.0;
	double SumBB = 0.0;
	double SumAB = 0.0;
	int32 PairCount = 0;
	for (int32 Y = 0; Y < GridY; ++Y)
	{
		for (int32 X = 0; X < GridX; ++X)
		{
			const FUERLTerrainBoxSpec& Box = Boxes[Y * GridX + X];
			MinX = FMath::Min(MinX, Box.CenterMeters.X - Box.ExtentMeters.X);
			MaxX = FMath::Max(MaxX, Box.CenterMeters.X + Box.ExtentMeters.X);
			MinY = FMath::Min(MinY, Box.CenterMeters.Y - Box.ExtentMeters.Y);
			MaxY = FMath::Max(MaxY, Box.CenterMeters.Y + Box.ExtentMeters.Y);
			const double Height = Box.ExtentMeters.Z * 2.0;
			MinHeight = FMath::Min(MinHeight, Height);
			MaxHeight = FMath::Max(MaxHeight, Height);
			TestEqual(TEXT("box has the requested horizontal footprint"), Box.ExtentMeters.X, 0.05);
			TestEqual(TEXT("box has the requested vertical base"), Box.CenterMeters.Z - Box.ExtentMeters.Z, 0.0);
			if (X + 1 < GridX)
			{
				const double A = Box.ExtentMeters.Z * 2.0;
				const double B = Boxes[Y * GridX + X + 1].ExtentMeters.Z * 2.0;
				SumA += A;
				SumB += B;
				SumAA += A * A;
				SumBB += B * B;
				SumAB += A * B;
				++PairCount;
			}
		}
	}
	TestTrue(TEXT("box raster has no XY gap at the patch edge"),
		FMath::IsNearlyEqual(MinX, -7.0) && FMath::IsNearlyEqual(MaxX, 7.0)
		&& FMath::IsNearlyEqual(MinY, -7.0) && FMath::IsNearlyEqual(MaxY, 7.0));
	const double MeanA = SumA / PairCount;
	const double MeanB = SumB / PairCount;
	const double Covariance = SumAB / PairCount - MeanA * MeanB;
	const double VarianceA = SumAA / PairCount - MeanA * MeanA;
	const double VarianceB = SumBB / PairCount - MeanB * MeanB;
	const double Correlation = Covariance / FMath::Sqrt(VarianceA * VarianceB);
	TestTrue(TEXT("random-grid neighbors are independently sampled"), FMath::Abs(Correlation) < 0.15);
	TestTrue(TEXT("random-grid terrain produces visible relief"), MaxHeight - MinHeight > 0.04);
	TestTrue(TEXT("configured height range reaches a useful upper part"), MaxHeight > 0.08);
	TestTrue(TEXT("configured height range stays below its cap"), MaxHeight <= 0.10 + 1.0e-9);
	AddInfo(FString::Printf(TEXT("[VERIFY] discrete random-grid boxes: count=%d footprint=%.1fx%.1f height=%.4f..%.4f neighbor_corr=%.6f"),
		Boxes.Num(), MaxX - MinX, MaxY - MinY, MinHeight, MaxHeight,
		Correlation));

	const bool bTerrainValid = FMath::Abs(Correlation) < 0.15 && MaxHeight - MinHeight > 0.04
		&& FMath::IsNearlyEqual(MinX, -7.0) && FMath::IsNearlyEqual(MaxX, 7.0)
		&& FMath::IsNearlyEqual(MinY, -7.0) && FMath::IsNearlyEqual(MaxY, 7.0)
		&& MaxHeight > 0.08 && MaxHeight <= 0.10 + 1.0e-9;
	return bTerrainValid;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLDiscreteRandomGridSharedWorldTest,
	"UERL.Unit.Worker.DiscreteRandomGridSharedWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLDiscreteRandomGridSharedWorldTest::RunTest(const FString& Parameters)
{
	const FUERLTerrainConfig Config = MakeRandomGridConfig(
		12.0, 1.0, 0.1, 0.01, 0.10, 1.0, 20260831);
	const int32 GridX = 140;
	const int32 GridY = 140;
	TArray<FVector> SharedSpawnOrigins;
	for (int32 Row = 0; Row < 8; ++Row)
	{
		for (int32 Column = 0; Column < 8; ++Column)
		{
			const double X = -4.5 + static_cast<double>(Column) * (9.0 / 7.0);
			const double Y = -4.5 + static_cast<double>(Row) * (9.0 / 7.0);
			SharedSpawnOrigins.Add(FVector(X * 100.0, Y * 100.0, 0.0));
		}
	}

	TArray<FUERLTerrainTierPlan> SharedPlans;
	FString SharedError;
	const bool bSharedBuilt = FUERLTerrainGenerator::BuildSharedPlan(
		Config, SharedSpawnOrigins, SharedPlans, SharedError);
	TestTrue(FString::Printf(TEXT("64-slot SharedWorld terrain builds: %s"), *SharedError), bSharedBuilt);
	if (!bSharedBuilt || SharedPlans.Num() != 1)
	{
		return false;
	}

	const TArray<FUERLTerrainBoxSpec>& SharedBoxes = SharedPlans[0].Boxes;
	const bool bExpectedRaster = SharedBoxes.Num() == GridX * GridY;
	TestEqual(TEXT("SharedWorld keeps one 0.1 m raster for all Slots"),
		SharedBoxes.Num(), GridX * GridY);
	if (!bExpectedRaster || SharedPlans[0].Samples.Num() != SharedSpawnOrigins.Num())
	{
		return false;
	}

	double SharedMaxAdjacentDelta = 0.0;
	for (int32 Y = 0; Y < GridY; ++Y)
	{
		for (int32 X = 0; X < GridX; ++X)
		{
			const double Height = SharedBoxes[Y * GridX + X].ExtentMeters.Z * 2.0;
			if (X + 1 < GridX)
			{
				SharedMaxAdjacentDelta = FMath::Max(
					SharedMaxAdjacentDelta,
					FMath::Abs(Height - SharedBoxes[Y * GridX + X + 1].ExtentMeters.Z * 2.0));
			}
			if (Y + 1 < GridY)
			{
				SharedMaxAdjacentDelta = FMath::Max(
					SharedMaxAdjacentDelta,
					FMath::Abs(Height - SharedBoxes[(Y + 1) * GridX + X].ExtentMeters.Z * 2.0));
			}
		}
	}

	double SharedMinSampleHeight = TNumericLimits<double>::Max();
	double SharedMaxSampleHeight = TNumericLimits<double>::Lowest();
	for (const FUERLTerrainSpawnSample& Sample : SharedPlans[0].Samples)
	{
		const double GroundHeight = Sample.GroundHeight / 100.0;
		if (!FMath::IsFinite(GroundHeight))
		{
			return false;
		}
		SharedMinSampleHeight = FMath::Min(SharedMinSampleHeight, GroundHeight);
		SharedMaxSampleHeight = FMath::Max(SharedMaxSampleHeight, GroundHeight);
	}

	const bool bSharedStep = SharedMaxAdjacentDelta > 0.04
		&& SharedMaxAdjacentDelta <= 0.09 + 1.0e-9;
	const bool bSpatiallyVaryingSamples = SharedMaxSampleHeight - SharedMinSampleHeight > 0.001;
	TestTrue(TEXT("SharedWorld keeps random-grid steps within the height envelope"), bSharedStep);
	TestTrue(TEXT("SharedWorld reset samples are finite and spatially varying"), bSpatiallyVaryingSamples);

	const FUERLTerrainConfig LowerDifficultyConfig = MakeRandomGridConfig(
		12.0, 1.0, 0.1, 0.01, 0.10, 0.15, 20260831);
	TArray<FUERLTerrainTierPlan> LowerDifficultyPlans;
	FString LowerDifficultyError;
	const bool bLowerDifficultyBuilt = FUERLTerrainGenerator::BuildSharedPlan(
		LowerDifficultyConfig, SharedSpawnOrigins, LowerDifficultyPlans, LowerDifficultyError);
	TestTrue(FString::Printf(TEXT("lower-difficulty random-grid plan builds: %s"), *LowerDifficultyError),
		bLowerDifficultyBuilt);
	double LowerDifficultyMaxHeight = TNumericLimits<double>::Lowest();
	if (bLowerDifficultyBuilt && LowerDifficultyPlans.Num() == 1)
	{
		for (const FUERLTerrainBoxSpec& Box : LowerDifficultyPlans[0].Boxes)
		{
			LowerDifficultyMaxHeight = FMath::Max(
				LowerDifficultyMaxHeight, Box.ExtentMeters.Z * 2.0);
		}
	}
	const double LowerDifficultyCap = 0.01 + 0.15 * (0.10 - 0.01);
	const bool bDifficultyReducesEnvelope = bLowerDifficultyBuilt
		&& LowerDifficultyMaxHeight > 0.01
		&& LowerDifficultyMaxHeight <= LowerDifficultyCap + 1.0e-9
		&& LowerDifficultyMaxHeight < SharedMaxSampleHeight;
	TestTrue(TEXT("random-grid difficulty reduces the generated height envelope"), bDifficultyReducesEnvelope);
	AddInfo(FString::Printf(TEXT("[VERIFY] SharedWorld boxes: max_adjacent_delta=%.4f step_slope=%.4f reset_height=%.4f..%.4f"),
		SharedMaxAdjacentDelta, SharedMaxAdjacentDelta / 0.1,
		SharedMinSampleHeight, SharedMaxSampleHeight));
	return bSharedStep && bSpatiallyVaryingSamples && bDifficultyReducesEnvelope;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLDiscreteRandomGridResetHeightTest,
	"UERL.Unit.Worker.DiscreteRandomGridResetHeight",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLDiscreteRandomGridResetHeightTest::RunTest(const FString& Parameters)
{
	const FUERLTerrainConfig Config = MakeRandomGridConfig(
		1.0, 0.0, 1.0, 0.12, 0.24, 1.0, 20260831);
	TArray<FUERLTerrainTierPlan> Plans;
	FString Error;
	const bool bBuilt = FUERLTerrainGenerator::BuildSharedPlan(
		Config, { FVector::ZeroVector }, Plans, Error);
	TestTrue(FString::Printf(TEXT("single-cell SharedWorld terrain builds: %s"), *Error), bBuilt);
	if (!bBuilt || Plans.Num() != 1 || Plans[0].Samples.Num() != 1 || Plans[0].Boxes.Num() != 1)
	{
		return false;
	}

	const double BoxTop = Plans[0].Boxes[0].ExtentMeters.Z * 2.0;
	const bool bResetMatchesTop = FMath::IsNearlyEqual(
		Plans[0].Samples[0].GroundHeight, BoxTop * 100.0);
	TestTrue(TEXT("SharedWorld reset z equals the terrain top for a one-cell footprint"), bResetMatchesTop);
	return bResetMatchesTop;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTerrainCatalogTest,
	"UERL.Unit.Worker.TerrainCatalog",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTerrainCatalogTest::RunTest(const FString& Parameters)
{
	// Prove all four catalog terrains actually produce geometry in UE:
	// level 0 plane, 1 smooth heightfield, 2 blocky boxes, 3 hand-placed explicit boxes.
	FUERLTerrainConfig Config;
	Config.NumLevels = 4;
	Config.CellSize[0] = 4.0;
	Config.CellSize[1] = 4.0;
	Config.Tiers.Reserve(Config.NumLevels);

	FUERLTerrainTierConfig& Plane = Config.Tiers.AddDefaulted_GetRef();
	Plane.Level = 0;
	Plane.Primitive = EUERLTerrainPrimitive::Plane;
	Plane.Seed = 101;
	Plane.Params = MakeShared<FJsonObject>();

	FUERLTerrainTierConfig& Heightfield = Config.Tiers.AddDefaulted_GetRef();
	Heightfield.Level = 1;
	Heightfield.Primitive = EUERLTerrainPrimitive::Heightfield;
	Heightfield.Seed = 202;
	Heightfield.PlatformWidth = 1.0;
	Heightfield.Params = MakeShared<FJsonObject>();
	Heightfield.Params->SetArrayField(TEXT("noise_range"), {
		MakeShared<FJsonValueNumber>(0.02), MakeShared<FJsonValueNumber>(0.12) });
	Heightfield.Params->SetNumberField(TEXT("noise_step"), 0.01);
	Heightfield.Params->SetNumberField(TEXT("horizontal_scale"), 0.5);
	Heightfield.Params->SetNumberField(TEXT("vertical_scale"), 0.005);
	Heightfield.Params->SetNumberField(TEXT("downsampled_scale"), 1.5);

	FUERLTerrainTierConfig& Boxes = Config.Tiers.AddDefaulted_GetRef();
	Boxes.Level = 2;
	Boxes.Primitive = EUERLTerrainPrimitive::Boxes;
	Boxes.Seed = 303;
	Boxes.PlatformWidth = 1.0;
	Boxes.Params = MakeShared<FJsonObject>();
	Boxes.Params->SetNumberField(TEXT("grid_width"), 0.5);
	Boxes.Params->SetArrayField(TEXT("grid_height_range"), {
		MakeShared<FJsonValueNumber>(0.05), MakeShared<FJsonValueNumber>(0.2) });
	Boxes.Params->SetBoolField(TEXT("holes"), false);

	// Hand-placed obstacles enter through the boxes primitive's explicit escape hatch.
	FUERLTerrainTierConfig& Explicit = Config.Tiers.AddDefaulted_GetRef();
	Explicit.Level = 3;
	Explicit.Primitive = EUERLTerrainPrimitive::Boxes;
	Explicit.Seed = 404;
	Explicit.PlatformWidth = 1.0;
	Explicit.Params = MakeShared<FJsonObject>();
	{
		TSharedPtr<FJsonObject> Obstacle = MakeShared<FJsonObject>();
		Obstacle->SetArrayField(TEXT("pose"), {
			MakeShared<FJsonValueNumber>(0.5), MakeShared<FJsonValueNumber>(0.0),
			MakeShared<FJsonValueNumber>(0.15), MakeShared<FJsonValueNumber>(0.0) });
		Obstacle->SetArrayField(TEXT("extent"), {
			MakeShared<FJsonValueNumber>(0.2), MakeShared<FJsonValueNumber>(0.2),
			MakeShared<FJsonValueNumber>(0.15) });
		Explicit.Params->SetArrayField(TEXT("explicit"),
			{ MakeShared<FJsonValueObject>(Obstacle) });
	}

	const TArray<FVector> SlotOrigins = { FVector::ZeroVector, FVector(0.0, 400.0, 0.0) };
	TArray<FUERLTerrainTierPlan> Plans;
	TArray<FUERLTerrainTierPlan> Repeat;
	FString Error;
	const bool bBuilt = FUERLTerrainGenerator::BuildPlan(Config, SlotOrigins, Plans, Error);
	const bool bRebuilt = FUERLTerrainGenerator::BuildPlan(Config, SlotOrigins, Repeat, Error);
	TestTrue(FString::Printf(TEXT("all four catalog terrains build: %s"), *Error), bBuilt);
	TestTrue(TEXT("the same catalog config builds twice"), bRebuilt);
	if (!bBuilt || !bRebuilt || Plans.Num() != 4 || Repeat.Num() != 4)
	{
		AddError(TEXT("terrain catalog did not produce four tier plans"));
		return false;
	}

	// Each terrain must land non-empty geometry.
	TestEqual(TEXT("plane spawns solid geometry"), Plans[0].Boxes.Num(), 2);
	TestEqual(TEXT("heightfield spawns one mesh per Slot"), Plans[1].Meshes.Num(), 2);
	TestTrue(TEXT("smooth heightfield mesh carries vertices"),
		Plans[1].Meshes.Num() > 0 && Plans[1].Meshes[0].VerticesMeters.Num() > 0
		&& Plans[1].Meshes[0].Triangles.Num() > 0);
	if (Plans[1].Meshes.Num() > 0)
	{
		const FUERLTerrainMeshSpec& HeightfieldMesh = Plans[1].Meshes[0];
		double MinimumHeight = TNumericLimits<double>::Max();
		double MaximumHeight = TNumericLimits<double>::Lowest();
		for (const FVector& Vertex : HeightfieldMesh.VerticesMeters)
		{
			MinimumHeight = FMath::Min(MinimumHeight, Vertex.Z);
			MaximumHeight = FMath::Max(MaximumHeight, Vertex.Z);
		}
		TestTrue(TEXT("heightfield contains relief above the base datum"),
			MinimumHeight >= -KINDA_SMALL_NUMBER && MaximumHeight > KINDA_SMALL_NUMBER);

		if (!TestTrue(TEXT("heightfield indices form complete triangles"),
			HeightfieldMesh.Triangles.Num() > 0 && HeightfieldMesh.Triangles.Num() % 3 == 0))
		{
			return false;
		}
		bool bFacesUpward = true;
		for (int32 TriangleIndex = 0;
			TriangleIndex + 2 < HeightfieldMesh.Triangles.Num() && bFacesUpward;
			TriangleIndex += 3)
		{
			if (!TestTrue(TEXT("triangle references generated vertices"),
				HeightfieldMesh.VerticesMeters.IsValidIndex(HeightfieldMesh.Triangles[TriangleIndex])
				&& HeightfieldMesh.VerticesMeters.IsValidIndex(HeightfieldMesh.Triangles[TriangleIndex + 1])
				&& HeightfieldMesh.VerticesMeters.IsValidIndex(HeightfieldMesh.Triangles[TriangleIndex + 2])))
			{
				return false;
			}
			const FVector& A = HeightfieldMesh.VerticesMeters[HeightfieldMesh.Triangles[TriangleIndex]];
			const FVector& B = HeightfieldMesh.VerticesMeters[HeightfieldMesh.Triangles[TriangleIndex + 1]];
			const FVector& C = HeightfieldMesh.VerticesMeters[HeightfieldMesh.Triangles[TriangleIndex + 2]];
			// This is the same winding convention used by UE's procedural-mesh
			// tangent calculation: (B-C) x (A-C) must point upward.
			bFacesUpward = ((B - C) ^ (A - C)).Z > 0.0;
		}
		TestTrue(TEXT("heightfield triangles face upward"), bFacesUpward);
		AddInfo(FString::Printf(TEXT("heightfield Z range: %.4f..%.4f m"), MinimumHeight, MaximumHeight));
	}
	TestTrue(TEXT("blocky boxes spawn solid geometry"), Plans[2].Boxes.Num() > 0);
	// Explicit obstacle: one hand-placed box per Slot, at the requested extent.
	TestEqual(TEXT("explicit obstacles spawn one box per Slot"), Plans[3].Boxes.Num(), 2);
	if (Plans[3].Boxes.Num() > 0)
	{
		TestEqual(TEXT("explicit obstacle keeps the requested extent"),
			Plans[3].Boxes[0].ExtentMeters, FVector(0.2, 0.2, 0.15));
	}

	// Determinism across the whole catalog.
	bool bDeterministic = true;
	for (int32 TierIndex = 0; TierIndex < Plans.Num() && bDeterministic; ++TierIndex)
	{
		const FUERLTerrainTierPlan& A = Plans[TierIndex];
		const FUERLTerrainTierPlan& B = Repeat[TierIndex];
		bDeterministic = A.Boxes.Num() == B.Boxes.Num() && A.Meshes.Num() == B.Meshes.Num()
			&& A.Samples.Num() == B.Samples.Num();
		for (int32 BoxIndex = 0; BoxIndex < A.Boxes.Num() && bDeterministic; ++BoxIndex)
		{
			bDeterministic = A.Boxes[BoxIndex].CenterMeters == B.Boxes[BoxIndex].CenterMeters
				&& A.Boxes[BoxIndex].ExtentMeters == B.Boxes[BoxIndex].ExtentMeters;
		}
		for (int32 MeshIndex = 0; MeshIndex < A.Meshes.Num() && bDeterministic; ++MeshIndex)
		{
			bDeterministic = A.Meshes[MeshIndex].VerticesMeters == B.Meshes[MeshIndex].VerticesMeters;
		}
	}
	TestTrue(TEXT("the whole catalog is deterministic"), bDeterministic);

	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-TERRAIN-002: plane, heightfield, boxes, and hand-placed obstacles all spawn"));
	return bDeterministic;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSafetyFallbackTest,
	"UERL.Unit.Worker.SafetyFallback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSafetyFallbackTest::RunTest(const FString& Parameters)
{
	float StateData[] = { 1.0f, 2.0f, 3.0f, 4.0f };
	uint8 FaultData[] = { 0, 0 };
	FUERLMutableBatchView States{ StateData, 2, 2 };
	FUERLFaultBatchView Faults{ FaultData, 2 };
	TArray<EUERLSlotFaultCode> ProviderFaults = {
		EUERLSlotFaultCode::None,
		EUERLSlotFaultCode::None,
	};
	FUERLSafetyMonitor Safety;
	Safety.Initialize(2, 2);
	FString Error;
	TestTrue(TEXT("finite Initial State is accepted"), Safety.AcceptInitial(States, ProviderFaults, Faults, Error));

	StateData[2] = std::numeric_limits<float>::quiet_NaN();
	StateData[3] = 99.0f;
	TestTrue(TEXT("fault row is sanitized"), Safety.SanitizeTransition(States, ProviderFaults, Faults, Error));
	TestEqual(TEXT("last-valid first value restored"), StateData[2], 3.0f);
	TestEqual(TEXT("last-valid second value restored"), StateData[3], 4.0f);
	TestEqual(TEXT("fault code is explicit"), FaultData[1], static_cast<uint8>(EUERLSlotFaultCode::NonFiniteStagingState));
	TestEqual(TEXT("another Slot remains unchanged"), StateData[0], 1.0f);
	StateData[2] = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("a non-finite sparse reset cannot recover the selected Slot"),
		Safety.AcceptReset({ StateData, 2, 2 }, { 1 }, ProviderFaults, { FaultData, 2 }, Error));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-WORKER-002: non-finite staging state is sanitized"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLBatchBindingTest,
	"UERL.Unit.Worker.BatchBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLBatchBindingTest::RunTest(const FString& Parameters)
{
	FUERLFieldDescriptor Action;
	Action.Name = TEXT("robot.force");
	Action.Unit = TEXT("N");
	Action.CoordinateFrame = TEXT("world-x");
	Action.Semantic = TEXT("force");
	Action.Source = TEXT("test.robot");
	FUERLFieldDescriptor State;
	State.Name = TEXT("robot.position");
	State.Unit = TEXT("m");
	State.CoordinateFrame = TEXT("world-x");
	State.Semantic = TEXT("position");
	State.Source = TEXT("test.robot");
	FUERLBatchSchema Schema;
	Schema.ActionWidth = 1;
	Schema.StateWidth = 1;
	Schema.ActionFields.Add({ Action, 0 });
	Schema.StateFields.Add({ State, 0 });

	FUERLBatchBinding Binding;
	FString Error;
	TestTrue(TEXT("Bridge-selected fields bind by exact metadata"),
		Binding.Compile(Schema, { Action }, { State }, Error));
	float ActionData[] = { 2.5f };
	FUERLNamedActionReader Reader(Binding, { ActionData, 1, 1 });
	float Value = 0.0f;
	TestTrue(TEXT("named Action can be read"), Reader.ReadScalar(0, TEXT("robot.force"), Value));
	TestEqual(TEXT("named Action value"), Value, 2.5f);
	float ScalarStateData[] = { 0.0f };
	FUERLNamedStateWriter ScalarWriter(Binding, { ScalarStateData, 1, 1 });
	TestTrue(TEXT("named scalar State can be written"),
		ScalarWriter.WriteScalar(0, TEXT("robot.position"), 4.5f));
	TestEqual(TEXT("named scalar State value"), ScalarStateData[0], 4.5f);

	FUERLFieldDescriptor VectorAction = Action;
	VectorAction.Name = TEXT("robot.actuator.target");
	VectorAction.Shape = { 3 };
	VectorAction.Width = 3;
	FUERLFieldDescriptor VectorState = State;
	VectorState.Name = TEXT("robot.body.2.body_linear_velocity");
	VectorState.Shape = { 3 };
	VectorState.Width = 3;
	VectorState.Unit = TEXT("m/s");
	VectorState.Semantic = TEXT("body_linear_velocity");
	VectorState.Observation.Type = EUERLObservationType::BodyLinearVelocity;
	VectorState.Observation.BodyName = TEXT("pole");
	VectorState.Observation.BodyIndex = 2;
	FUERLBatchSchema VectorSchema;
	VectorSchema.ActionWidth = 3;
	VectorSchema.StateWidth = 3;
	VectorSchema.ActionFields.Add({ VectorAction, 0 });
	VectorSchema.StateFields.Add({ VectorState, 0 });
	FUERLBatchBinding VectorBinding;
	TestTrue(TEXT("vector fields bind by exact metadata"),
		VectorBinding.Compile(VectorSchema, { VectorAction }, { VectorState }, Error));
	float VectorActions[] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f };
	FUERLNamedActionReader VectorReader(VectorBinding, { VectorActions, 2, 3 });
	float ReadValues[3] = {};
	TestTrue(TEXT("named vector Action can be read"),
		VectorReader.ReadVector(1, TEXT("robot.actuator.target"), TArrayView<float>(ReadValues, 3)));
	TestEqual(TEXT("vector Action value 0"), ReadValues[0], 4.0f);
	TestEqual(TEXT("vector Action value 2"), ReadValues[2], 6.0f);
	float VectorStates[6] = {};
	FUERLNamedStateWriter VectorWriter(VectorBinding, { VectorStates, 2, 3 });
	const float StateValues[] = { -1.0f, -2.0f, -3.0f };
	TestTrue(TEXT("named vector State can be written"),
		VectorWriter.WriteVector(0, TEXT("robot.body.2.body_linear_velocity"), TConstArrayView<float>(StateValues, 3)));
	TestEqual(TEXT("vector State value 1"), VectorStates[1], -2.0f);
	TestFalse(TEXT("vector write rejects a wrong span width"),
		VectorWriter.WriteVector(0, TEXT("robot.body.2.body_linear_velocity"), TConstArrayView<float>(StateValues, 2)));
	TestFalse(TEXT("null State span rejects vector write"),
		VectorWriter.WriteVector(0, TEXT("robot.body.2.body_linear_velocity"), TConstArrayView<float>(static_cast<const float*>(nullptr), 3)));
	TestFalse(TEXT("vector Action rejects a wrong span width"),
		VectorReader.ReadVector(0, TEXT("robot.actuator.target"), TArrayView<float>(ReadValues, 2)));
	TestFalse(TEXT("null Action span rejects vector read"),
		VectorReader.ReadVector(0, TEXT("robot.actuator.target"), TArrayView<float>(static_cast<float*>(nullptr), 3)));

	FUERLFieldDescriptor MismatchedVectorState = VectorState;
	MismatchedVectorState.Observation.BodyIndex = 1;
	TestFalse(TEXT("observation binding drift rejects selected schema"),
		VectorBinding.Compile(VectorSchema, { VectorAction }, { MismatchedVectorState }, Error));
	TestEqual(TEXT("failed compile clears Action width"), VectorBinding.ActionWidth(), 0);
	TestEqual(TEXT("failed compile clears State width"), VectorBinding.StateWidth(), 0);
	TestFalse(TEXT("failed compile is not marked compiled"), VectorBinding.IsCompiled());
	FUERLNamedStateWriter InvalidViewWriter(VectorBinding, { nullptr, 1, 3 });
	InvalidViewWriter.FillRows({ 0 }, 99.0f);

	FUERLBatchSchema OverflowSchema = VectorSchema;
	OverflowSchema.ActionFields[0].Column = MAX_int32;
	TestFalse(TEXT("field column overflow is rejected"), OverflowSchema.IsValid());
	OverflowSchema.ActionFields[0].Column = 0;
	OverflowSchema.ActionWidth = 2;
	TestFalse(TEXT("field width overflow is rejected"), OverflowSchema.IsValid());
	FUERLFieldDescriptor ShapeOverflowField = VectorAction;
	ShapeOverflowField.Shape = { 65537, 65537 };
	ShapeOverflowField.Width = 131073;
	FUERLBatchSchema ShapeOverflowSchema = VectorSchema;
	ShapeOverflowSchema.ActionFields[0].Field = ShapeOverflowField;
	ShapeOverflowSchema.ActionWidth = 131073;
	TestFalse(TEXT("shape width overflow is rejected"), ShapeOverflowSchema.IsValid());
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-WORKER-003: named scalar/vector fields bind by descriptor"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLClosedFrameGateTest,
	"UERL.Unit.Worker.ClosedFrameGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLClosedFrameGateTest::RunTest(const FString& Parameters)
{
	const double OriginalDeltaTime = FApp::GetDeltaTime();
	UUERLFrameGate* Gate = NewObject<UUERLFrameGate>();
	TestTrue(TEXT("Frame Gate initializes"), Gate->Initialize(GEngine));
	Gate->Close();
	FApp::SetDeltaTime(0.25);
	TestTrue(TEXT("a closed Frame Gate returns control to the host Engine"), Gate->UpdateTimeStep(GEngine));
	Gate->Shutdown(GEngine);
	FApp::SetDeltaTime(OriginalDeltaTime);
	AddInfo(TEXT("[VERIFY] AC-U5-UNIT-002: closed Frame Gate returns timing ownership to the host"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLSessionCommandLineTest,
	"UERL.Unit.Worker.SessionCommandLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLSessionCommandLineTest::RunTest(const FString& Parameters)
{
	FUERLWorkerLaunchConfig Config;
	FString Error;
	TestEqual(TEXT("ordinary UE process stays inactive"),
		UUERLSessionSubsystem::ParseCommandLine(TEXT("-game"), Config, Error),
		EUERLCommandLineParseResult::Inactive);

	TestEqual(TEXT("NONE launch contract is accepted"),
		UUERLSessionSubsystem::ParseCommandLine(
			TEXT("-game -uerlport=7777 -uerlpresentation=none -nullrhi -unattended -nosound"), Config, Error),
		EUERLCommandLineParseResult::Valid);
	TestEqual(TEXT("NONE presentation selected"), Config.PresentationMode, EUERLPresentationMode::None);
	TestEqual(TEXT("command-line Worker owns its process"), Config.Ownership, EUERLSessionOwnership::Process);

	TestEqual(TEXT("performance output path is accepted"),
		UUERLSessionSubsystem::ParseCommandLine(
			TEXT("-game -uerlport=7777 -uerlpresentation=none -nullrhi -unattended -nosound "
				"-uerlperformancepath=C:/run/worker_stage_latency.jsonl"), Config, Error),
		EUERLCommandLineParseResult::Valid);
	TestEqual(TEXT("performance output path is retained"), Config.Bridge.PerformancePath,
		FString(TEXT("C:/run/worker_stage_latency.jsonl")));

	TestEqual(TEXT("VIEWPORT launch contract is accepted"),
		UUERLSessionSubsystem::ParseCommandLine(
			TEXT("-game -uerlport=7778 -uerlpresentation=viewport -windowed"), Config, Error),
		EUERLCommandLineParseResult::Valid);
	TestEqual(TEXT("VIEWPORT presentation selected"), Config.PresentationMode, EUERLPresentationMode::Viewport);

	TestEqual(TEXT("GAMEPLAY launch contract is accepted"),
		UUERLSessionSubsystem::ParseCommandLine(
			TEXT("-game -uerlport=7782 -uerlpresentation=gameplay -windowed"), Config, Error),
		EUERLCommandLineParseResult::Valid);
	TestEqual(TEXT("GAMEPLAY preserves the map PlayerController"),
		Config.PresentationMode, EUERLPresentationMode::Gameplay);
	TestEqual(TEXT("GAMEPLAY rejects NullRHI"),
		UUERLSessionSubsystem::ParseCommandLine(
			TEXT("-game -uerlport=7782 -uerlpresentation=gameplay -nullrhi"), Config, Error),
		EUERLCommandLineParseResult::Invalid);

	TestEqual(TEXT("explicit attach ownership is accepted"),
		UUERLSessionSubsystem::ParseCommandLine(
			TEXT("-game -uerlport=7779 -uerlpresentation=viewport -uerlattach"), Config, Error),
		EUERLCommandLineParseResult::Valid);
	TestEqual(TEXT("attach does not own the host process"), Config.Ownership, EUERLSessionOwnership::Attached);

	TestEqual(TEXT("VIEWPORT rejects NullRHI"),
		UUERLSessionSubsystem::ParseCommandLine(
			TEXT("-uerlport=7780 -uerlpresentation=viewport -nullrhi -uerlattach"), Config, Error),
		EUERLCommandLineParseResult::Invalid);
	TestEqual(TEXT("invalid attach still reports Attached ownership"),
		Config.Ownership, EUERLSessionOwnership::Attached);
	TestEqual(TEXT("NONE requires the complete process contract"),
		UUERLSessionSubsystem::ParseCommandLine(
			TEXT("-uerlport=7781 -uerlpresentation=none -nullrhi"), Config, Error),
		EUERLCommandLineParseResult::Invalid);
	AddInfo(TEXT("[VERIFY] AC-U5-UNIT-001: launch, viewport, and attach contracts are explicit"));
	return true;
}

#if WITH_EDITOR

DEFINE_LATENT_AUTOMATION_COMMAND_THREE_PARAMETER(
	FStartUERLPIEAttachCommand,
	FAutomationTestBase*, Test,
	int32, Port,
	double, DeadlineSeconds);

bool FStartUERLPIEAttachCommand::Update()
{
	UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
	UGameInstance* GameInstance = PlayWorld ? PlayWorld->GetGameInstance() : nullptr;
	UUERLSessionSubsystem* Session = GameInstance
		? GameInstance->GetSubsystem<UUERLSessionSubsystem>()
		: nullptr;
	if (!Session)
	{
		if (FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			Test->AddError(TEXT("PIE GameInstance SessionSubsystem did not become available"));
			return true;
		}
		return false;
	}

	FUERLWorkerLaunchConfig Config;
	Config.Bridge.Port = Port;
	Config.PresentationMode = EUERLPresentationMode::None;
	FString Error;
	if (!Session->StartAttached(Config, Error))
	{
		Test->AddError(FString::Printf(TEXT("PIE StartAttached failed: %s"), *Error));
		return true;
	}
	Test->AddInfo(FString::Printf(TEXT("[VERIFY] AC-U5-PIE-001: PIE StartAttached listening port=%d"), Port));
	return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_THREE_PARAMETER(
	FVerifyUERLPIEAttachClosedCommand,
	FAutomationTestBase*, Test,
	int32, Port,
	double, DeadlineSeconds);

bool FVerifyUERLPIEAttachClosedCommand::Update()
{
	UWorld* PlayWorld = GEditor ? GEditor->PlayWorld : nullptr;
	UGameInstance* GameInstance = PlayWorld ? PlayWorld->GetGameInstance() : nullptr;
	UUERLSessionSubsystem* Session = GameInstance
		? GameInstance->GetSubsystem<UUERLSessionSubsystem>()
		: nullptr;
	if (!Session || Session->IsSessionActive())
	{
		if (FPlatformTime::Seconds() >= DeadlineSeconds)
		{
			Test->AddError(TEXT("attached PIE Session did not close before the deadline"));
			return true;
		}
		return false;
	}

	Test->TestNotNull(TEXT("PIE World survives attached Shutdown"), PlayWorld);
	Test->TestTrue(TEXT("attached Shutdown removes the Frame Gate"), !GEngine || GEngine->GetCustomTimeStep() == nullptr);

	FUERLWorkerLaunchConfig Config;
	Config.Bridge.Port = Port;
	Config.PresentationMode = EUERLPresentationMode::None;
	FString Error;
	if (!Session->StartAttached(Config, Error))
	{
		Test->AddError(FString::Printf(TEXT("second PIE StartAttached failed: %s"), *Error));
		return true;
	}
	Session->Shutdown();
	Test->TestFalse(TEXT("second attached Session cleans up without ending PIE"), Session->IsSessionActive());

	UUERLFrameGate* HostTimeStep = NewObject<UUERLFrameGate>(Session);
	GEngine->SetCustomTimeStep(HostTimeStep);
	Error.Reset();
	Test->TestFalse(TEXT("attach rejects a host-owned CustomTimeStep"), Session->StartAttached(Config, Error));
	Test->TestTrue(TEXT("rejected attach preserves the host-owned CustomTimeStep"),
		GEngine->GetCustomTimeStep() == HostTimeStep);
	GEngine->SetCustomTimeStep(nullptr);
	Test->AddInfo(TEXT("[VERIFY] AC-U5-PIE-002: PIE host supports cleanup and reattach"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPIEAttachIntegrationTest,
	"UERL.Integration.Worker.PIEAttach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPIEAttachIntegrationTest::RunTest(const FString& Parameters)
{
	int32 Port = 0;
	if (!FParse::Value(FCommandLine::Get(), TEXT("uerltestpieattachport="), Port) || Port <= 0)
	{
		AddError(TEXT("-uerltestpieattachport is required"));
		return false;
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FStartUERLPIEAttachCommand(this, Port, FPlatformTime::Seconds() + 60.0));
	ADD_LATENT_AUTOMATION_COMMAND(FVerifyUERLPIEAttachClosedCommand(this, Port, FPlatformTime::Seconds() + 180.0));
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	return true;
}

#endif

#endif
