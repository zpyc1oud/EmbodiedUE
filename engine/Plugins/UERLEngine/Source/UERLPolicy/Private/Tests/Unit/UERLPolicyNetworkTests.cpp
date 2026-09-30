#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UERLPolicyArtifact.h"
#include "UERLPolicyNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	bool ResolvePolicyNetCorpusDir(FString& OutDir, FString& OutError)
	{
		TArray<FString> Candidates;
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT(".."), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("policynet"))));
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("policynet"))));

		for (const FString& Candidate : Candidates)
		{
			if (FPaths::DirectoryExists(Candidate))
			{
				OutDir = Candidate;
				return true;
			}
		}

		OutError = TEXT("policynet corpus directory not found; tried:");
		for (const FString& Candidate : Candidates)
		{
			OutError += FString::Printf(TEXT("\n  %s"), *Candidate);
		}
		return false;
	}

	bool LoadPolicyNetJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read '%s'"), *Path);
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = FString::Printf(TEXT("JSON deserialize failed for '%s'"), *Path);
			return false;
		}
		return true;
	}

	bool ParsePolicyNetFloatArray(const TArray<TSharedPtr<FJsonValue>>* Values, TArray<float>& Out, FString& OutError)
	{
		if (Values == nullptr)
		{
			OutError = TEXT("expected a JSON array of numbers");
			return false;
		}
		Out.Reset();
		Out.Reserve(Values->Num());
		for (const TSharedPtr<FJsonValue>& Value : *Values)
		{
			if (!Value.IsValid() || Value->Type != EJson::Number)
			{
				OutError = TEXT("array entry is not a number");
				return false;
			}
			Out.Add(static_cast<float>(Value->AsNumber()));
		}
		return true;
	}

	bool LoadArtifactAndBuild(
		const FString& ArtifactPath,
		FUERLPolicyArtifact& OutArtifact,
		FUERLPolicyNetwork& OutNetwork,
		FString& OutError)
	{
		if (!OutArtifact.Load(ArtifactPath, OutError))
		{
			return false;
		}
		return OutNetwork.Build(OutArtifact, OutError);
	}

	bool AssertFloatClose(
		FAutomationTestBase& Test,
		const FString& Label,
		TConstArrayView<float> Actual,
		TConstArrayView<float> Expected,
		float Tolerance)
	{
		Test.TestEqual(*(Label + TEXT(" width")), Actual.Num(), Expected.Num());
		const int32 Count = FMath::Min(Actual.Num(), Expected.Num());
		bool bOk = Actual.Num() == Expected.Num();
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Delta = FMath::Abs(Actual[Index] - Expected[Index]);
			const bool bClose = Delta <= Tolerance;
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s[%d] |%g - %g| <= %g"),
					*Label,
					Index,
					Actual[Index],
					Expected[Index],
					Tolerance),
				bClose);
			bOk = bOk && bClose;
		}
		return bOk;
	}

	bool RunEvaluateCase(
		FAutomationTestBase& Test,
		const FString& CorpusDir,
		const FString& Stem)
	{
		FString Error;
		FUERLPolicyArtifact Artifact;
		FUERLPolicyNetwork Network;
		const FString ArtifactPath = FPaths::Combine(CorpusDir, Stem + TEXT(".uerlpol2"));
		if (!LoadArtifactAndBuild(ArtifactPath, Artifact, Network, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s build: %s"), *Stem, *Error));
			return false;
		}

		TSharedPtr<FJsonObject> ExpectedObject;
		const FString ExpectedPath = FPaths::Combine(CorpusDir, Stem + TEXT(".expected.json"));
		if (!LoadPolicyNetJsonObject(ExpectedPath, ExpectedObject, Error))
		{
			Test.AddError(Error);
			return false;
		}

		TArray<float> Input;
		TArray<float> Expected;
		const TArray<TSharedPtr<FJsonValue>>* InputValues = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ExpectedValues = nullptr;
		if (!ExpectedObject->TryGetArrayField(TEXT("input"), InputValues)
			|| !ExpectedObject->TryGetArrayField(TEXT("expected"), ExpectedValues)
			|| !ParsePolicyNetFloatArray(InputValues, Input, Error)
			|| !ParsePolicyNetFloatArray(ExpectedValues, Expected, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s expected json: %s"), *Stem, *Error));
			return false;
		}

		double Tolerance = 2.0e-5;
		ExpectedObject->TryGetNumberField(TEXT("tolerance"), Tolerance);

		TArray<float> Actual;
		if (!Network.Evaluate(Input, Actual, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s evaluate: %s"), *Stem, *Error));
			return false;
		}
		return AssertFloatClose(Test, Stem, Actual, Expected, static_cast<float>(Tolerance));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyNetworkBuildWidthsTest,
	"UERL.Unit.Policy.Network.AC_UE_UNIT_POLICYNET_001.BuildWidthsFromDescriptors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyNetworkBuildWidthsTest::RunTest(const FString& Parameters)
{
	FString CorpusDir;
	FString Error;
	TestTrue(TEXT("resolve policynet corpus"), ResolvePolicyNetCorpusDir(CorpusDir, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	FUERLPolicyArtifact Artifact;
	FUERLPolicyNetwork Network;
	const FString ArtifactPath = FPaths::Combine(CorpusDir, TEXT("mlp_2x64.uerlpol2"));
	TestTrue(TEXT("build mlp_2x64"), LoadArtifactAndBuild(ArtifactPath, Artifact, Network, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	// Widths must come from model descriptors, not from plan/config constants.
	// Plan group width happens to match, but the network reports descriptor widths.
	TestEqual(TEXT("input width from descriptor"), Network.InputWidth(), 8);
	TestEqual(TEXT("output width from descriptor"), Network.OutputWidth(), 4);
	TestTrue(TEXT("network is built"), Network.IsBuilt());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyNetworkTwoShapesTest,
	"UERL.Unit.Policy.Network.AC_UE_UNIT_POLICYNET_002.TwoDifferentOnnxShapes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyNetworkTwoShapesTest::RunTest(const FString& Parameters)
{
	FString CorpusDir;
	FString Error;
	TestTrue(TEXT("resolve policynet corpus"), ResolvePolicyNetCorpusDir(CorpusDir, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	bool bOk = true;
	{
		FUERLPolicyArtifact Artifact;
		FUERLPolicyNetwork Network;
		const FString Path = FPaths::Combine(CorpusDir, TEXT("mlp_2x64.uerlpol2"));
		if (!LoadArtifactAndBuild(Path, Artifact, Network, Error))
		{
			AddError(FString::Printf(TEXT("mlp_2x64: %s"), *Error));
			bOk = false;
		}
		else
		{
			TestEqual(TEXT("2x64 input"), Network.InputWidth(), 8);
			TestEqual(TEXT("2x64 output"), Network.OutputWidth(), 4);
			TArray<float> Obs;
			Obs.SetNumZeroed(Network.InputWidth());
			TArray<float> Action;
			TestTrue(TEXT("2x64 evaluate"), Network.Evaluate(Obs, Action, Error));
			if (!Error.IsEmpty())
			{
				AddError(Error);
				bOk = false;
			}
			TestEqual(TEXT("2x64 action width"), Action.Num(), 4);
		}
	}
	{
		FUERLPolicyArtifact Artifact;
		FUERLPolicyNetwork Network;
		const FString Path = FPaths::Combine(CorpusDir, TEXT("mlp_5x256.uerlpol2"));
		if (!LoadArtifactAndBuild(Path, Artifact, Network, Error))
		{
			AddError(FString::Printf(TEXT("mlp_5x256: %s"), *Error));
			bOk = false;
		}
		else
		{
			TestEqual(TEXT("5x256 input"), Network.InputWidth(), 16);
			TestEqual(TEXT("5x256 output"), Network.OutputWidth(), 6);
			TArray<float> Obs;
			Obs.SetNumZeroed(Network.InputWidth());
			TArray<float> Action;
			TestTrue(TEXT("5x256 evaluate"), Network.Evaluate(Obs, Action, Error));
			if (!Error.IsEmpty())
			{
				AddError(Error);
				bOk = false;
			}
			TestEqual(TEXT("5x256 action width"), Action.Num(), 6);
		}
	}
	return bOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyNetworkNoDoubleNormTest,
	"UERL.Unit.Policy.Network.AC_UE_UNIT_POLICYNET_003.NoDoubleNormalization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyNetworkNoDoubleNormTest::RunTest(const FString& Parameters)
{
	// Critical: mean≠0 / std≠1 fixture. Double-normalization in C++ would miss expected.
	FString CorpusDir;
	FString Error;
	TestTrue(TEXT("resolve policynet corpus"), ResolvePolicyNetCorpusDir(CorpusDir, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}
	return RunEvaluateCase(*this, CorpusDir, TEXT("mlp_norm_nontrivial"));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyNetworkWrongInputWidthTest,
	"UERL.Unit.Policy.Network.AC_UE_UNIT_POLICYNET_004.WrongInputWidthError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyNetworkWrongInputWidthTest::RunTest(const FString& Parameters)
{
	FString CorpusDir;
	FString Error;
	TestTrue(TEXT("resolve policynet corpus"), ResolvePolicyNetCorpusDir(CorpusDir, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	FUERLPolicyArtifact Artifact;
	FUERLPolicyNetwork Network;
	const FString ArtifactPath = FPaths::Combine(CorpusDir, TEXT("mlp_2x64.uerlpol2"));
	TestTrue(TEXT("build mlp_2x64"), LoadArtifactAndBuild(ArtifactPath, Artifact, Network, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<float> Wrong;
	Wrong.SetNumZeroed(Network.InputWidth() + 3);
	TArray<float> Action;
	TestFalse(TEXT("wrong width evaluate fails"), Network.Evaluate(Wrong, Action, Error));
	TestTrue(
		TEXT("error mentions observation width"),
		Error.Contains(FString::Printf(TEXT("%d"), Wrong.Num())));
	TestTrue(
		TEXT("error mentions model input width"),
		Error.Contains(FString::Printf(TEXT("%d"), Network.InputWidth())));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyNetworkPhantomXParityTest,
	"UERL.Unit.Policy.Network.PhantomXParity.ZeroObservation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyNetworkPhantomXParityTest::RunTest(const FString& Parameters)
{
	// Same 18 expected floats as the NNE parity fixture, tolerance 2e-5.
	FString CorpusDir;
	FString Error;
	TestTrue(TEXT("resolve policynet corpus"), ResolvePolicyNetCorpusDir(CorpusDir, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	const bool bOk = RunEvaluateCase(*this, CorpusDir, TEXT("phantomx_115"));
	const double ElapsedMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	AddInfo(FString::Printf(
		TEXT("[VERIFY] phantomx_115 Build+Evaluate wall time %.3f ms (50 Hz budget 20 ms; not a gate)"),
		ElapsedMs));
	return bOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
