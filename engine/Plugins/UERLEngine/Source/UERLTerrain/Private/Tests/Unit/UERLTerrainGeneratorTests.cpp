#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UERLSubTerrainGenerator.h"
#include "UERLTerrainAtlas.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	TSharedPtr<FJsonObject> EmptyParams()
	{
		return MakeShared<FJsonObject>();
	}

	TSharedPtr<FJsonObject> HeightfieldParams()
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> NoiseRange;
		NoiseRange.Add(MakeShared<FJsonValueNumber>(-0.05));
		NoiseRange.Add(MakeShared<FJsonValueNumber>(0.05));
		Params->SetArrayField(TEXT("noise_range"), NoiseRange);
		Params->SetNumberField(TEXT("noise_step"), 0.01);
		Params->SetNumberField(TEXT("horizontal_scale"), 0.25);
		Params->SetNumberField(TEXT("vertical_scale"), 0.005);
		Params->SetNumberField(TEXT("downsampled_scale"), 0.5);
		return Params;
	}

	TSharedPtr<FJsonObject> BoxesParams(double Difficulty)
	{
		TSharedPtr<FJsonObject> Params = MakeShared<FJsonObject>();
		Params->SetNumberField(TEXT("grid_width"), 0.5);
		TArray<TSharedPtr<FJsonValue>> HeightRange;
		HeightRange.Add(MakeShared<FJsonValueNumber>(0.05));
		HeightRange.Add(MakeShared<FJsonValueNumber>(0.15));
		Params->SetArrayField(TEXT("grid_height_range"), HeightRange);
		Params->SetBoolField(TEXT("holes"), false);
		Params->SetStringField(TEXT("generator"), TEXT("random_grid"));
		Params->SetNumberField(TEXT("difficulty"), Difficulty);
		return Params;
	}

	FUERLSubTerrainRequest MakeRequest(
		TSharedPtr<FJsonObject> Params,
		double Difficulty,
		uint64 Seed = 17)
	{
		FUERLSubTerrainRequest Request;
		Request.Difficulty = Difficulty;
		Request.CellSize[0] = 2.0;
		Request.CellSize[1] = 2.0;
		Request.BorderWidth = 0.0;
		Request.PlatformWidth = 0.5;
		Request.Seed = Seed;
		Request.SlotId = 0;
		Request.PatchOriginMeters = FVector::ZeroVector;
		Request.ResetCenters = { FVector2D::ZeroVector };
		Request.Params = MoveTemp(Params);
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTerrainGeneratorsGenerateAndValidateTest,
	"UERL.Unit.Terrain.Generator.AC_UE_UNIT_TERRAIN_001.GenerateAndValidateParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTerrainGeneratorsGenerateAndValidateTest::RunTest(const FString& Parameters)
{
	const IUERLSubTerrainGenerator* Plane = FindSubTerrainGenerator(TEXT("plane"));
	const IUERLSubTerrainGenerator* Heightfield = FindSubTerrainGenerator(TEXT("heightfield"));
	const IUERLSubTerrainGenerator* Boxes = FindSubTerrainGenerator(TEXT("boxes"));
	TestNotNull(TEXT("plane generator is registered"), Plane);
	TestNotNull(TEXT("heightfield generator is registered"), Heightfield);
	TestNotNull(TEXT("boxes generator is registered"), Boxes);
	if (!Plane || !Heightfield || !Boxes)
	{
		return false;
	}

	FString Error;
	FUERLTerrainPatch Patch;
	TestTrue(TEXT("plane generates"),
		Plane->Generate(MakeRequest(EmptyParams(), 0.0), Patch, Error));
	TestTrue(TEXT("heightfield generates"),
		Heightfield->Generate(MakeRequest(HeightfieldParams(), 0.5, 23), Patch, Error));
	TestTrue(TEXT("boxes generates"),
		Boxes->Generate(MakeRequest(BoxesParams(0.75), 0.75, 29), Patch, Error));

	TSharedPtr<FJsonObject> BadPlane = MakeShared<FJsonObject>();
	BadPlane->SetNumberField(TEXT("unexpected"), 1.0);
	TestFalse(TEXT("plane rejects unexpected params"), Plane->ValidateParams(*BadPlane, Error));

	TSharedPtr<FJsonObject> BadHeightfield = HeightfieldParams();
	BadHeightfield->SetNumberField(TEXT("noise_step"), -1.0);
	TestFalse(TEXT("heightfield rejects invalid noise_step"),
		Heightfield->ValidateParams(*BadHeightfield, Error));

	TSharedPtr<FJsonObject> BadBoxes = BoxesParams(1.5);
	TestFalse(TEXT("boxes rejects difficulty outside [0,1]"),
		Boxes->ValidateParams(*BadBoxes, Error));

	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_TERRAIN_001: generators generate and reject invalid params"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTerrainRegistryTest,
	"UERL.Unit.Terrain.Registry.AC_UE_UNIT_TERRAIN_004.UnknownAndDuplicateIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTerrainRegistryTest::RunTest(const FString& Parameters)
{
	TestNull(TEXT("unknown generator id is not found"),
		FindSubTerrainGenerator(TEXT("not_a_real_primitive")));

	class FProbeGenerator final : public IUERLSubTerrainGenerator
	{
	public:
		virtual FName Id() const override { return FName(TEXT("plane")); }
		virtual bool ValidateParams(const FJsonObject&, FString& OutError) const override
		{
			OutError.Reset();
			return true;
		}
		virtual bool Generate(
			const FUERLSubTerrainRequest&,
			FUERLTerrainPatch&,
			FString& OutError) const override
		{
			OutError = TEXT("probe generator must not generate");
			return false;
		}
	};

	FString Error;
	TestFalse(TEXT("duplicate generator id registration fails"),
		RegisterSubTerrainGeneratorUnique(MakeShared<FProbeGenerator>(), Error));
	TestTrue(TEXT("duplicate registration reports an error"), !Error.IsEmpty());

	FUERLTerrainConfig Config;
	Config.NumLevels = 1;
	Config.CellSize[0] = 2.0;
	Config.CellSize[1] = 2.0;
	Config.BorderWidth = 0.0;
	FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
	Tier.Level = 0;
	Tier.Primitive = static_cast<EUERLTerrainPrimitive>(255);
	Tier.Params = MakeShared<FJsonObject>();
	TArray<FUERLTerrainTierPlan> Plans;
	Error.Reset();
	TestFalse(
		TEXT("BuildPlan rejects unknown primitive id"),
		FUERLTerrainAtlas::BuildPlan(Config, { FVector::ZeroVector }, Plans, Error));
	TestTrue(TEXT("unknown primitive error mentions unsupported"), Error.Contains(TEXT("unsupported")));

	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_TERRAIN_004: unknown and duplicate generator ids fail"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTerrainGeometryParityTest,
	"UERL.Unit.Terrain.Generator.GeometryBitParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTerrainGeometryParityTest::RunTest(const FString& Parameters)
{
	FUERLTerrainConfig Config;
	Config.NumLevels = 1;
	Config.CellSize[0] = 2.0;
	Config.CellSize[1] = 2.0;
	Config.BorderWidth = 0.0;
	FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
	Tier.Level = 0;
	Tier.Primitive = EUERLTerrainPrimitive::Heightfield;
	Tier.Seed = 20260903;
	Tier.PlatformWidth = 0.5;
	Tier.Params = HeightfieldParams();

	TArray<FUERLTerrainTierPlan> AtlasPlans;
	FString Error;
	TestTrue(TEXT("atlas heightfield plan builds"),
		FUERLTerrainAtlas::BuildSharedPlan(Config, { FVector::ZeroVector }, AtlasPlans, Error));

	const IUERLSubTerrainGenerator* Heightfield = FindSubTerrainGenerator(TEXT("heightfield"));
	TestNotNull(TEXT("heightfield generator exists for parity"), Heightfield);
	FUERLSubTerrainRequest Request = MakeRequest(HeightfieldParams(), 0.0, Tier.Seed);
	Request.SlotId = INDEX_NONE;
	Request.PlatformWidth = Tier.PlatformWidth;
	FUERLTerrainPatch Patch;
	TestTrue(TEXT("parity generator runs"),
		Heightfield && Heightfield->Generate(Request, Patch, Error));
	TestEqual(TEXT("parity mesh count"), Patch.Meshes.Num(), 1);
	TestEqual(TEXT("atlas mesh count"), AtlasPlans.Num() == 1 ? AtlasPlans[0].Meshes.Num() : -1, 1);
	if (Patch.Meshes.Num() == 1 && AtlasPlans.Num() == 1 && AtlasPlans[0].Meshes.Num() == 1)
	{
		TestTrue(TEXT("heightfield vertices are bit-identical"),
			Patch.Meshes[0].VerticesMeters == AtlasPlans[0].Meshes[0].VerticesMeters);
		TestTrue(TEXT("heightfield triangles are bit-identical"),
			Patch.Meshes[0].Triangles == AtlasPlans[0].Meshes[0].Triangles);
	}

	Config.Tiers[0].Primitive = EUERLTerrainPrimitive::Boxes;
	Config.Tiers[0].Params = BoxesParams(0.4);
	TArray<FUERLTerrainTierPlan> BoxesPlans;
	TestTrue(TEXT("atlas boxes plan builds"),
		FUERLTerrainAtlas::BuildSharedPlan(Config, { FVector::ZeroVector }, BoxesPlans, Error));
	Request = MakeRequest(BoxesParams(0.4), 0.4, Tier.Seed);
	Request.SlotId = INDEX_NONE;
	Request.PlatformWidth = Tier.PlatformWidth;
	TestTrue(TEXT("boxes generator runs"),
		FindSubTerrainGenerator(TEXT("boxes"))->Generate(Request, Patch, Error));
	TestTrue(TEXT("boxes geometry is bit-identical"),
		BoxesPlans.Num() == 1
		&& Patch.Boxes == BoxesPlans[0].Boxes
		&& Patch.CollisionMeshes.Num() == 1
		&& BoxesPlans[0].CollisionMeshes.Num() == 1
		&& Patch.CollisionMeshes[0].VerticesMeters == BoxesPlans[0].CollisionMeshes[0].VerticesMeters
		&& Patch.CollisionMeshes[0].Triangles == BoxesPlans[0].CollisionMeshes[0].Triangles);

	AddInfo(TEXT(
		"[VERIFY] post-move self-consistency: atlas BuildSharedPlan vs generator Generate "
		"(not a pre-migration §6.1 golden corpus)"));
	return true;
}

#endif
