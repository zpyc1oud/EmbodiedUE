#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "UERLTerrainAtlas.h"
#include "UERLTerrainInternal.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTerrainAtlasDifficultyTest,
	"UERL.Unit.Terrain.Atlas.AC_UE_UNIT_TERRAIN_002.DifficultyInterpolation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTerrainAtlasDifficultyTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("single-level difficulty is zero"), FUERLTerrainAtlas::Difficulty(0, 1), 0.0);
	TestEqual(TEXT("level 0 difficulty is zero"), FUERLTerrainAtlas::Difficulty(0, 5), 0.0);
	TestEqual(TEXT("mid level difficulty is 0.5"), FUERLTerrainAtlas::Difficulty(2, 5), 0.5);
	TestEqual(TEXT("max level difficulty is 1"), FUERLTerrainAtlas::Difficulty(4, 5), 1.0);

	FUERLTerrainConfig Config;
	Config.NumLevels = 5;
	Config.CellSize[0] = 2.0;
	Config.CellSize[1] = 2.0;
	Config.BorderWidth = 0.0;
	for (int32 Level = 0; Level < Config.NumLevels; ++Level)
	{
		FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
		Tier.Level = Level;
		Tier.Primitive = EUERLTerrainPrimitive::Plane;
		Tier.Params = MakeShared<FJsonObject>();
	}

	FString Error;
	FUERLSubTerrainRequest Request;
	TestTrue(
		TEXT("MakeRequest applies structural mid-level difficulty"),
		UERLTerrainInternal::MakeRequest(
			Config,
			Config.Tiers[2],
			0,
			FVector::ZeroVector,
			{ FVector2D::ZeroVector },
			Request,
			Error));
	TestEqual(TEXT("plane MakeRequest difficulty is 0.5"), Request.Difficulty, 0.5);

	Config.Tiers[4].Primitive = EUERLTerrainPrimitive::Boxes;
	Config.Tiers[4].Params->SetNumberField(TEXT("difficulty"), 0.35);
	TestTrue(
		TEXT("MakeRequest accepts authored boxes difficulty"),
		UERLTerrainInternal::MakeRequest(
			Config,
			Config.Tiers[4],
			0,
			FVector::ZeroVector,
			{ FVector2D::ZeroVector },
			Request,
			Error));
	TestEqual(TEXT("authored boxes difficulty overrides structural 1.0"), Request.Difficulty, 0.35);

	AddInfo(TEXT(
		"[VERIFY] AC_UE_UNIT_TERRAIN_002: atlas Difficulty + MakeRequest structural/authored paths"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTerrainAtlasOriginLayoutTest,
	"UERL.Unit.Terrain.Atlas.AC_UE_UNIT_TERRAIN_003.NonOverlappingOrigins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTerrainAtlasOriginLayoutTest::RunTest(const FString& Parameters)
{
	FUERLTerrainConfig Config;
	Config.NumLevels = 3;
	Config.CellSize[0] = 4.0;
	Config.CellSize[1] = 3.0;
	Config.BorderWidth = 0.5;
	for (int32 Level = 0; Level < Config.NumLevels; ++Level)
	{
		FUERLTerrainTierConfig& Tier = Config.Tiers.AddDefaulted_GetRef();
		Tier.Level = Level;
		Tier.Primitive = EUERLTerrainPrimitive::Plane;
		Tier.Params = MakeShared<FJsonObject>();
	}

	FString Error;
	TArray<FVector> Origins;
	for (int32 Level = 0; Level < Config.NumLevels; ++Level)
	{
		for (int32 Column = 0; Column < 2; ++Column)
		{
			FVector Origin = FVector::ZeroVector;
			TestTrue(
				TEXT("origin resolves for level/column"),
				FUERLTerrainAtlas::ResolveOrigin(Config, Level, Column, Origin, Error));
			TestTrue(FString::Printf(TEXT("level %d column %d has its exact origin"), Level, Column),
				Origin.Equals(FVector(5.0 * Level, 4.0 * Column, 0.0), 1.0e-9));
			Origins.Add(Origin);
		}
	}

	const double StrideX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
	const double StrideY = Config.CellSize[1] + 2.0 * Config.BorderWidth;
	bool bNonOverlapping = true;
	for (int32 Index = 0; Index < Origins.Num(); ++Index)
	{
		for (int32 Other = Index + 1; Other < Origins.Num(); ++Other)
		{
			bNonOverlapping = bNonOverlapping && !Origins[Index].Equals(Origins[Other], 1.0e-9);
		}
	}
	TestTrue(TEXT("each (level, column) origin is unique"), bNonOverlapping);
	TestTrue(TEXT("level stride matches cell+border"),
		FMath::IsNearlyEqual(Origins[2].X - Origins[0].X, StrideX));
	TestTrue(TEXT("column stride matches cell+border"),
		FMath::IsNearlyEqual(Origins[1].Y - Origins[0].Y, StrideY));
	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_TERRAIN_003: atlas origins do not overlap"));
	return true;
}

#endif
