#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UERLInterfaceTypes.h"
#include "UERLPolicyArtifact.h"
#include "UERLPolicyNetwork.h"
#include "UERLPolicyPlanRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Ticket 11 — sequential variable-dt deploy parity (Unit).
 *
 * Injects recorded raw_state into plan runtime + NNE + action plan — no World /
 * robot physics. Named Unit so UERL.Unit+ auto-collects; no Integration whitelist
 * update required (unlike ticket 16 Controller tests).
 */

namespace
{
	bool ResolveVariableDtCorpusRoot(FString& OutDir, FString& OutError)
	{
		TArray<FString> Candidates;
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT(".."), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("deploy_variable_dt"))));
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("deploy_variable_dt"))));

		for (const FString& Candidate : Candidates)
		{
			if (FPaths::DirectoryExists(Candidate))
			{
				OutDir = Candidate;
				return true;
			}
		}

		OutError = TEXT("deploy parity corpus root not found; tried:");
		for (const FString& Candidate : Candidates)
		{
			OutError += FString::Printf(TEXT("\n  %s"), *Candidate);
		}
		return false;
	}

	bool ResolveArtifactPath(const FString& RelativeOrAbsolute, FString& OutPath, FString& OutError)
	{
		TArray<FString> Candidates;
		if (FPaths::IsRelative(RelativeOrAbsolute))
		{
			Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT(".."), RelativeOrAbsolute)));
			Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), RelativeOrAbsolute)));
			Candidates.Add(FPaths::ConvertRelativePathToFull(
				FPaths::ProjectContentDir() / TEXT("UERLHost/Policies") / FPaths::GetCleanFilename(RelativeOrAbsolute)));
		}
		else
		{
			Candidates.Add(RelativeOrAbsolute);
		}

		for (const FString& Candidate : Candidates)
		{
			if (FPaths::FileExists(Candidate))
			{
				OutPath = Candidate;
				return true;
			}
		}

		OutError = FString::Printf(TEXT("artifact '%s' not found; tried:"), *RelativeOrAbsolute);
		for (const FString& Candidate : Candidates)
		{
			OutError += FString::Printf(TEXT("\n  %s"), *Candidate);
		}
		return false;
	}

	bool CollectVariableDtCasePaths(const FString& CorpusRoot, TArray<FString>& OutPaths, FString& OutError)
	{
		OutPaths.Reset();
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(CorpusRoot, TEXT("*.json")), true, false);
		Names.Sort();
		for (const FString& Name : Names)
		{
			OutPaths.Add(FPaths::Combine(CorpusRoot, Name));
		}

		if (OutPaths.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("no deploy parity cases under '%s' (zero cases must fail)"),
				*CorpusRoot);
			return false;
		}
		return true;
	}

	bool LoadJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
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

	bool ParseFloatArray(const TArray<TSharedPtr<FJsonValue>>* Values, TArray<float>& Out, FString& OutError)
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

	FUERLFieldDescriptor MakeLayoutField(const FString& Name, int32 Width)
	{
		FUERLFieldDescriptor Field;
		Field.Name = FName(*Name);
		Field.DType = TEXT("float32");
		Field.Shape = {Width};
		Field.Unit = TEXT("1");
		Field.CoordinateFrame = TEXT("world");
		Field.Semantic = TEXT("deploy_parity");
		Field.Source = TEXT("robot");
		Field.Width = Width;
		return Field;
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
			const float AllowedDelta = Tolerance + Tolerance * FMath::Abs(Expected[Index]);
			const bool bClose = Delta <= AllowedDelta;
			Test.TestTrue(
				*FString::Printf(
					TEXT("%s[%d] |%g - %g| <= atol/rtol %g"),
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

	bool PackRawStateFromNamed(
		const FUERLObservationPlan& Plan,
		const TSharedPtr<FJsonObject>& RawStateObject,
		TArray<FUERLFieldDescriptor>& OutFields,
		TArray<float>& OutFlat,
		FString& OutError)
	{
		OutFields.Reset();
		OutFlat.Reset();
		if (!RawStateObject.IsValid())
		{
			OutError = TEXT("raw_state must be an object");
			return false;
		}

		for (const FName& FieldName : Plan.StateRequirements)
		{
			const FString Name = FieldName.ToString();
			const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
			if (!RawStateObject->TryGetArrayField(Name, Values) || Values == nullptr)
			{
				OutError = FString::Printf(TEXT("raw_state missing field '%s'"), *Name);
				return false;
			}
			TArray<float> Chunk;
			if (!ParseFloatArray(Values, Chunk, OutError))
			{
				OutError = FString::Printf(TEXT("raw_state['%s']: %s"), *Name, *OutError);
				return false;
			}
			OutFields.Add(MakeLayoutField(Name, Chunk.Num()));
			OutFlat.Append(Chunk);
		}
		return true;
	}

	bool RunVariableDtCase(FAutomationTestBase& Test, const FString& CasePath)
	{
		FString Error;
		TSharedPtr<FJsonObject> Root;
		if (!LoadJsonObject(CasePath, Root, Error))
		{
			Test.AddError(Error);
			return false;
		}

		FString ArtifactRelative;
		if (!Root->TryGetStringField(TEXT("artifact"), ArtifactRelative) || ArtifactRelative.IsEmpty())
		{
			Test.AddError(FString::Printf(TEXT("%s missing artifact"), *CasePath));
			return false;
		}

		const TArray<TSharedPtr<FJsonValue>>* StepsValues = nullptr;
		if (!Root->TryGetArrayField(TEXT("steps"), StepsValues) || StepsValues == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("%s missing steps"), *CasePath));
			return false;
		}
		// AC_PARITY_DEPLOY_005/006: multi-step observation/action sequence.
		Test.TestTrue(
			*FString::Printf(TEXT("%s has >= 3 steps"), *CasePath),
			StepsValues->Num() >= 3);
		if (StepsValues->Num() < 3)
		{
			return false;
		}

		double Tolerance = 2.0e-5;
		Root->TryGetNumberField(TEXT("tolerance"), Tolerance);

		FString ArtifactPath;
		if (!ResolveArtifactPath(ArtifactRelative, ArtifactPath, Error))
		{
			Test.AddError(Error);
			return false;
		}

		FUERLPolicyArtifact Artifact;
		if (!Artifact.Load(ArtifactPath, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s load artifact: %s"), *CasePath, *Error));
			return false;
		}

		FUERLPolicyNetwork Network;
		if (!Network.Build(Artifact, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s build network: %s"), *CasePath, *Error));
			return false;
		}

		// Build AvailableState layout from step 0 raw_state (same fields every step).
		const TSharedPtr<FJsonObject> FirstStep = (*StepsValues)[0]->AsObject();
		if (!FirstStep.IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s step 0 not an object"), *CasePath));
			return false;
		}
		const TSharedPtr<FJsonObject>* FirstRaw = nullptr;
		if (!FirstStep->TryGetObjectField(TEXT("raw_state"), FirstRaw) || !FirstRaw || !FirstRaw->IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s step 0 missing raw_state"), *CasePath));
			return false;
		}

		TArray<FUERLFieldDescriptor> StateFields;
		TArray<float> ScratchFlat;
		if (!PackRawStateFromNamed(Artifact.ObservationPlan(), *FirstRaw, StateFields, ScratchFlat, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s layout: %s"), *CasePath, *Error));
			return false;
		}

		TMap<FName, int32> CommandWidths;
		for (const FUERLPlanOp& Op : Artifact.ObservationPlan().Ops)
		{
			if (Op.Op == FName(TEXT("command")))
			{
				const FString* ChannelText = Op.Params.Strings.Find(TEXT("channel"));
				const double* WidthParam = Op.Params.Scalars.Find(TEXT("width"));
				if (ChannelText != nullptr && WidthParam != nullptr)
				{
					CommandWidths.Add(FName(*ChannelText), static_cast<int32>(*WidthParam));
				}
				else if (ChannelText != nullptr)
				{
					CommandWidths.Add(FName(*ChannelText), Op.Width);
				}
			}
		}

		FUERLPlanRuntime ObservationRuntime;
		if (!ObservationRuntime.Compile(Artifact.ObservationPlan(), StateFields, CommandWidths, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s compile obs: %s"), *CasePath, *Error));
			return false;
		}

		TMap<FName, int32> ActionCommandWidths;
		if (Artifact.ActionPlan().CommandFields.Num() == 1)
		{
			ActionCommandWidths.Add(Artifact.ActionPlan().CommandFields[0], Network.OutputWidth());
		}
		FUERLPlanRuntime ActionRuntime;
		if (!ActionRuntime.Compile(Artifact.ActionPlan(), ActionCommandWidths, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s compile action: %s"), *CasePath, *Error));
			return false;
		}

		TArray<float> PreviousAction;
		PreviousAction.Init(0.0f, Network.OutputWidth());

		bool bAllOk = true;
		for (int32 StepIndex = 0; StepIndex < StepsValues->Num(); ++StepIndex)
		{
			const TSharedPtr<FJsonObject> Step = (*StepsValues)[StepIndex]->AsObject();
			if (!Step.IsValid())
			{
				Test.AddError(FString::Printf(TEXT("%s step %d not an object"), *CasePath, StepIndex));
				return false;
			}

			bool bResetHistory = false;
			Step->TryGetBoolField(TEXT("history_reset"), bResetHistory);
			if (bResetHistory)
			{
				PreviousAction.Init(0.0f, Network.OutputWidth());
			}
			const TArray<TSharedPtr<FJsonValue>>* PreviousValues = nullptr;
			if (Step->TryGetArrayField(TEXT("previous_action"), PreviousValues) && PreviousValues != nullptr)
			{
				if (!ParseFloatArray(PreviousValues, PreviousAction, Error)
					|| PreviousAction.Num() != Network.OutputWidth())
				{
					Test.AddError(FString::Printf(
						TEXT("%s step %d previous_action must have width %d: %s"),
						*CasePath,
						StepIndex,
						Network.OutputWidth(),
						*Error));
					return false;
				}
			}

			double ControlFrameDt = 0.0;
			if (!Step->TryGetNumberField(TEXT("control_frame_dt"), ControlFrameDt)
				|| !FMath::IsFinite(ControlFrameDt)
				|| ControlFrameDt <= 0.0)
			{
				Test.AddError(FString::Printf(
					TEXT("%s step %d requires positive control_frame_dt"), *CasePath, StepIndex));
				return false;
			}

			const TSharedPtr<FJsonObject>* RawObject = nullptr;
			if (!Step->TryGetObjectField(TEXT("raw_state"), RawObject) || !RawObject || !RawObject->IsValid())
			{
				Test.AddError(FString::Printf(TEXT("%s step %d missing raw_state"), *CasePath, StepIndex));
				return false;
			}

			TArray<FUERLFieldDescriptor> StepFields;
			TArray<float> RawFlat;
			if (!PackRawStateFromNamed(Artifact.ObservationPlan(), *RawObject, StepFields, RawFlat, Error))
			{
				Test.AddError(FString::Printf(TEXT("%s step %d pack: %s"), *CasePath, StepIndex, *Error));
				return false;
			}

			const TSharedPtr<FJsonObject>* CommandsObject = nullptr;
			if (!Step->TryGetObjectField(TEXT("commands"), CommandsObject) || !CommandsObject || !CommandsObject->IsValid())
			{
				Test.AddError(FString::Printf(TEXT("%s step %d missing commands"), *CasePath, StepIndex));
				return false;
			}

			FUERLPlanInputs ObsInputs;
			ObsInputs.RawState = RawFlat;
			ObsInputs.PreviousAction = PreviousAction;
			ObsInputs.ControlFrameDtSeconds = static_cast<float>(ControlFrameDt);
			for (const TPair<FName, int32>& Channel : CommandWidths)
			{
				const TArray<TSharedPtr<FJsonValue>>* ChannelValues = nullptr;
				if (!(*CommandsObject)->TryGetArrayField(Channel.Key.ToString(), ChannelValues))
				{
					Test.AddError(FString::Printf(
						TEXT("%s step %d missing command '%s'"),
						*CasePath,
						StepIndex,
						*Channel.Key.ToString()));
					return false;
				}
				TArray<float> ChannelFloats;
				if (!ParseFloatArray(ChannelValues, ChannelFloats, Error))
				{
					Test.AddError(Error);
					return false;
				}
				ObsInputs.Commands.Add(Channel.Key, ChannelFloats);
			}

			TArray<float> Observation;
			if (!ObservationRuntime.Execute(ObsInputs, Observation, Error))
			{
				Test.AddError(FString::Printf(TEXT("%s step %d obs: %s"), *CasePath, StepIndex, *Error));
				return false;
			}

			TArray<float> Action;
			if (!Network.Evaluate(Observation, Action, Error))
			{
				Test.AddError(FString::Printf(TEXT("%s step %d net: %s"), *CasePath, StepIndex, *Error));
				return false;
			}

			FUERLPlanInputs ActionInputs;
			ActionInputs.PolicyAction = Action;
			ActionInputs.PreviousAction = PreviousAction;
			TArray<float> Targets;
			if (!ActionRuntime.Execute(ActionInputs, Targets, Error))
			{
				Test.AddError(FString::Printf(TEXT("%s step %d action: %s"), *CasePath, StepIndex, *Error));
				return false;
			}

			TArray<float> ExpectedAction;
			TArray<float> ExpectedTargets;
			TArray<float> ExpectedObservation;
			const TArray<TSharedPtr<FJsonValue>>* ExpectedObservationValues = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* ExpectedActionValues = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* ExpectedTargetsValues = nullptr;
			if (!Step->TryGetArrayField(TEXT("expected_observation"), ExpectedObservationValues)
				|| !Step->TryGetArrayField(TEXT("expected_action"), ExpectedActionValues)
				|| !Step->TryGetArrayField(TEXT("expected_targets"), ExpectedTargetsValues)
				|| !ParseFloatArray(ExpectedObservationValues, ExpectedObservation, Error)
				|| !ParseFloatArray(ExpectedActionValues, ExpectedAction, Error)
				|| !ParseFloatArray(ExpectedTargetsValues, ExpectedTargets, Error))
			{
				Test.AddError(FString::Printf(
					TEXT("%s step %d expected_action/targets: %s"), *CasePath, StepIndex, *Error));
				return false;
			}

			const FString ObservationLabel = FString::Printf(TEXT("%s step %d observation"), *CasePath, StepIndex);
			const FString ActionLabel = FString::Printf(TEXT("%s step %d action"), *CasePath, StepIndex);
			const FString TargetsLabel = FString::Printf(TEXT("%s step %d targets"), *CasePath, StepIndex);
			bAllOk = AssertFloatClose(
					Test, ObservationLabel, Observation, ExpectedObservation, static_cast<float>(Tolerance))
				&& bAllOk;
			bAllOk = AssertFloatClose(
						Test, ActionLabel, Action, ExpectedAction, static_cast<float>(Tolerance))
				&& bAllOk;
			bAllOk = AssertFloatClose(
						Test, TargetsLabel, Targets, ExpectedTargets, static_cast<float>(Tolerance))
				&& bAllOk;

			PreviousAction = Action;
		}

		Test.AddInfo(TEXT(
			"[VERIFY] AC_PARITY_DEPLOY_005/006/007: sequential variable-dt UE NNE and Python ONNX "
			"observations, actions, and physical targets match reviewed expected within atol/rtol 2e-5"));
		return bAllOk;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyVariableDtParityCorpusTest,
	"UERL.Unit.Policy.DeployVariableDtParity.AC_PARITY_DEPLOY_005_006_007.Corpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyVariableDtParityCorpusTest::RunTest(const FString& Parameters)
{
	FString CorpusRoot;
	FString Error;
	TestTrue(TEXT("resolve variable-dt deploy corpus root"), ResolveVariableDtCorpusRoot(CorpusRoot, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<FString> CasePaths;
	TestTrue(TEXT("collect variable-dt deploy cases"), CollectVariableDtCasePaths(CorpusRoot, CasePaths, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	bool bAllOk = true;
	for (const FString& CasePath : CasePaths)
	{
		bAllOk = RunVariableDtCase(*this, CasePath) && bAllOk;
	}
	return bAllOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
