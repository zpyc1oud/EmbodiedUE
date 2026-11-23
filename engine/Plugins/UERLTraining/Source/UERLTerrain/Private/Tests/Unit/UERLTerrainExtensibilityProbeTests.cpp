#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "UERLSubTerrainGenerator.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Test-only primitive in its own translation unit.
 * Proves a new generator needs one TU + RegisterSubTerrainGeneratorUnique,
 * without editing existing product generators.
 */
namespace
{
	class FFlatProbeGenerator final : public IUERLSubTerrainGenerator
	{
	public:
		virtual FName Id() const override { return FName(TEXT("test_flat_plane")); }

		virtual bool ValidateParams(const FJsonObject& Params, FString& OutError) const override
		{
			if (Params.Values.Num() != 0)
			{
				OutError = TEXT("test_flat_plane params must be empty");
				return false;
			}
			OutError.Reset();
			return true;
		}

		virtual bool Generate(
			const FUERLSubTerrainRequest& Request,
			FUERLTerrainPatch& OutPatch,
			FString& OutError) const override
		{
			OutPatch = FUERLTerrainPatch{};
			FUERLTerrainBoxSpec& Box = OutPatch.Boxes.AddDefaulted_GetRef();
			Box.SlotId = Request.SlotId;
			Box.CenterMeters = Request.PatchOriginMeters;
			Box.ExtentMeters = FVector(Request.CellSize[0] * 0.5, Request.CellSize[1] * 0.5, 0.005);
			OutError.Reset();
			return true;
		}
	};

	FUERLSubTerrainRequest MakeProbeRequest()
	{
		FUERLSubTerrainRequest Request;
		Request.Difficulty = 0.25;
		Request.CellSize[0] = 2.0;
		Request.CellSize[1] = 2.0;
		Request.BorderWidth = 0.0;
		Request.PlatformWidth = 0.5;
		Request.Seed = 17;
		Request.SlotId = 0;
		Request.PatchOriginMeters = FVector::ZeroVector;
		Request.ResetCenters = { FVector2D::ZeroVector };
		Request.Params = MakeShared<FJsonObject>();
		return Request;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTerrainExtensibilityProbeTest,
	"UERL.Unit.Terrain.Registry.ExtensibilityProbe",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTerrainExtensibilityProbeTest::RunTest(const FString& Parameters)
{
	FString Error;
	const bool bRegistered =
		RegisterSubTerrainGeneratorUnique(MakeShared<FFlatProbeGenerator>(), Error);
	TestTrue(
		TEXT("test-only flat generator registers once"),
		bRegistered || Error.Contains(TEXT("already registered")));
	const IUERLSubTerrainGenerator* Probe = FindSubTerrainGenerator(TEXT("test_flat_plane"));
	TestNotNull(TEXT("test-only flat generator is findable"), Probe);
	FUERLTerrainPatch Patch;
	TestTrue(TEXT("test-only flat generator produces a patch"),
		Probe && Probe->Generate(MakeProbeRequest(), Patch, Error));
	TestEqual(TEXT("test-only flat generator emits one box"), Patch.Boxes.Num(), 1);
	AddInfo(TEXT("[VERIFY] extensibility: new primitive needs one TU + registration only"));
	return true;
}

#endif
