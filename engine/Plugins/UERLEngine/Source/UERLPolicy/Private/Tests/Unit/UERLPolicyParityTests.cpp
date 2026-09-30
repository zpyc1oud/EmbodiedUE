#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UERLInterfaceTypes.h"
#include "UERLPlan.h"
#include "UERLPolicyOperators.h"
#include "UERLPolicyPlanRuntime.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	bool ResolveOperatorCorpusRoot(FString& OutDir, FString& OutError)
	{
		TArray<FString> Candidates;
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT(".."), TEXT("tests"), TEXT("parity"), TEXT("cases"))));
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT("tests"), TEXT("parity"), TEXT("cases"))));

		for (const FString& Candidate : Candidates)
		{
			if (FPaths::DirectoryExists(Candidate))
			{
				OutDir = Candidate;
				return true;
			}
		}

		OutError = TEXT("operator parity corpus root not found; tried:");
		for (const FString& Candidate : Candidates)
		{
			OutError += FString::Printf(TEXT("\n  %s"), *Candidate);
		}
		return false;
	}

	bool IsSkippedCorpusCategory(const FString& DirName)
	{
		return DirName.Equals(TEXT("structure"), ESearchCase::IgnoreCase)
			|| DirName.Equals(TEXT("artifact"), ESearchCase::IgnoreCase)
			|| DirName.Equals(TEXT("action_plan"), ESearchCase::IgnoreCase)
			|| DirName.Equals(TEXT("deploy"), ESearchCase::IgnoreCase)
			|| DirName.Equals(TEXT("deploy_variable_dt"), ESearchCase::IgnoreCase)
			|| DirName.Equals(TEXT("controller"), ESearchCase::IgnoreCase)
			|| DirName.Equals(TEXT("policynet"), ESearchCase::IgnoreCase);
	}

	bool CollectOperatorCasePaths(const FString& CorpusRoot, TArray<FString>& OutPaths, FString& OutError)
	{
		// Traverse cases/<category>/*.json — skip structure/artifact (different schema).
		TArray<FString> CategoryDirs;
		IFileManager::Get().FindFiles(CategoryDirs, *FPaths::Combine(CorpusRoot, TEXT("*")), false, true);
		CategoryDirs.Sort();

		OutPaths.Reset();
		for (const FString& Category : CategoryDirs)
		{
			if (IsSkippedCorpusCategory(Category))
			{
				continue;
			}
			const FString CategoryPath = FPaths::Combine(CorpusRoot, Category);
			TArray<FString> Names;
			IFileManager::Get().FindFiles(Names, *FPaths::Combine(CategoryPath, TEXT("*.json")), true, false);
			Names.Sort();
			for (const FString& Name : Names)
			{
				if (Name.EndsWith(TEXT(".expected.json")))
				{
					continue;
				}
				OutPaths.Add(FPaths::Combine(CategoryPath, Name));
			}
		}

		if (OutPaths.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("no operator parity cases under '%s' (zero cases must fail)"),
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
		Field.Semantic = TEXT("parity");
		Field.Source = TEXT("robot");
		Field.Width = Width;
		return Field;
	}

	bool ParseNameWidthLayout(
		const TArray<TSharedPtr<FJsonValue>>* LayoutValues,
		TArray<FUERLFieldDescriptor>* OutFields,
		TMap<FName, int32>* OutChannels,
		const FString& CasePath,
		const TCHAR* FieldLabel,
		FString& OutError)
	{
		if (LayoutValues == nullptr)
		{
			OutError = FString::Printf(TEXT("%s missing '%s'"), *CasePath, FieldLabel);
			return false;
		}
		for (const TSharedPtr<FJsonValue>& Entry : *LayoutValues)
		{
			const TSharedPtr<FJsonObject> Obj = Entry.IsValid() ? Entry->AsObject() : nullptr;
			if (!Obj.IsValid())
			{
				OutError = FString::Printf(TEXT("%s %s entry not an object"), *CasePath, FieldLabel);
				return false;
			}
			FString Name;
			double WidthNumber = 0.0;
			if (!Obj->TryGetStringField(TEXT("name"), Name) || !Obj->TryGetNumberField(TEXT("width"), WidthNumber))
			{
				OutError = FString::Printf(TEXT("%s %s entry missing name/width"), *CasePath, FieldLabel);
				return false;
			}
			const int32 Width = static_cast<int32>(WidthNumber);
			if (OutFields != nullptr)
			{
				OutFields->Add(MakeLayoutField(Name, Width));
			}
			if (OutChannels != nullptr)
			{
				OutChannels->Add(FName(*Name), Width);
			}
		}
		return true;
	}

	bool RunOperatorCase(FAutomationTestBase& Test, const FString& CasePath)
	{
		FString Error;
		TSharedPtr<FJsonObject> Root;
		if (!LoadJsonObject(CasePath, Root, Error))
		{
			Test.AddError(Error);
			return false;
		}

		const TSharedPtr<FJsonObject>* PlanObject = nullptr;
		if (!Root->TryGetObjectField(TEXT("plan"), PlanObject) || !PlanObject || !PlanObject->IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s missing 'plan' object"), *CasePath));
			return false;
		}

		const bool bHasExpected = Root->HasField(TEXT("expected"));
		const bool bHasExpectedError = Root->HasField(TEXT("expected_error"));
		if (bHasExpected == bHasExpectedError)
		{
			Test.AddError(FString::Printf(
				TEXT("%s must have exactly one of 'expected' / 'expected_error'"), *CasePath));
			return false;
		}

		FString ErrorPhase;
		FString ErrorContains;
		TArray<float> Expected;
		double Tolerance = 2.0e-5;
		if (bHasExpectedError)
		{
			const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
			if (!Root->TryGetObjectField(TEXT("expected_error"), ErrorObject) || !ErrorObject || !ErrorObject->IsValid())
			{
				Test.AddError(FString::Printf(TEXT("%s expected_error must be an object"), *CasePath));
				return false;
			}
			if (!(*ErrorObject)->TryGetStringField(TEXT("phase"), ErrorPhase)
				|| !(*ErrorObject)->TryGetStringField(TEXT("contains"), ErrorContains)
				|| ErrorContains.IsEmpty()
				|| (!ErrorPhase.Equals(TEXT("compile")) && !ErrorPhase.Equals(TEXT("execute"))))
			{
				Test.AddError(FString::Printf(
					TEXT("%s expected_error needs phase (compile|execute) and non-empty contains"),
					*CasePath));
				return false;
			}
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* ExpectedValues = nullptr;
			if (!Root->TryGetArrayField(TEXT("expected"), ExpectedValues) || ExpectedValues == nullptr)
			{
				Test.AddError(FString::Printf(TEXT("%s missing required 'expected' array"), *CasePath));
				return false;
			}
			if (!ParseFloatArray(ExpectedValues, Expected, Error))
			{
				Test.AddError(FString::Printf(TEXT("%s expected: %s"), *CasePath, *Error));
				return false;
			}
			Root->TryGetNumberField(TEXT("tolerance"), Tolerance);
		}

		const TArray<TSharedPtr<FJsonValue>>* LayoutValues = nullptr;
		if (!Root->TryGetArrayField(TEXT("state_layout"), LayoutValues) || LayoutValues == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("%s missing 'state_layout'"), *CasePath));
			return false;
		}
		TArray<FUERLFieldDescriptor> Available;
		if (!ParseNameWidthLayout(LayoutValues, &Available, nullptr, CasePath, TEXT("state_layout"), Error))
		{
			Test.AddError(Error);
			return false;
		}

		TMap<FName, int32> AvailableCommands;
		const TArray<TSharedPtr<FJsonValue>>* CommandLayoutValues = nullptr;
		if (Root->TryGetArrayField(TEXT("command_layout"), CommandLayoutValues) && CommandLayoutValues != nullptr)
		{
			if (!ParseNameWidthLayout(
					CommandLayoutValues, nullptr, &AvailableCommands, CasePath, TEXT("command_layout"), Error))
			{
				Test.AddError(Error);
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* RawValues = nullptr;
		if (!Root->TryGetArrayField(TEXT("raw_state"), RawValues) || RawValues == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("%s missing 'raw_state'"), *CasePath));
			return false;
		}
		TArray<float> RawState;
		if (!ParseFloatArray(RawValues, RawState, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s raw_state: %s"), *CasePath, *Error));
			return false;
		}

		FUERLObservationPlan Plan;
		if (!Plan.ParseJson(PlanObject->ToSharedRef(), Error))
		{
			Test.AddError(FString::Printf(TEXT("%s plan parse: %s"), *CasePath, *Error));
			return false;
		}

		FUERLPlanRuntime Runtime;
		const bool bCompiled = Runtime.Compile(Plan, Available, AvailableCommands, Error);
		if (bHasExpectedError && ErrorPhase.Equals(TEXT("compile")))
		{
			if (bCompiled)
			{
				Test.AddError(FString::Printf(TEXT("%s expected Compile failure"), *CasePath));
				return false;
			}
			if (!Error.Contains(ErrorContains))
			{
				Test.AddError(FString::Printf(
					TEXT("%s Compile error '%s' missing '%s'"), *CasePath, *Error, *ErrorContains));
				return false;
			}
			return true;
		}
		if (!bCompiled)
		{
			Test.AddError(FString::Printf(TEXT("%s Compile: %s"), *CasePath, *Error));
			return false;
		}

		FUERLPlanInputs Inputs;
		Inputs.RawState = RawState;
		double ControlFrameDtSeconds = 0.0;
		if (Root->TryGetNumberField(TEXT("control_frame_dt"), ControlFrameDtSeconds))
		{
			Inputs.ControlFrameDtSeconds = static_cast<float>(ControlFrameDtSeconds);
		}
		const TSharedPtr<FJsonObject>* CommandsObject = nullptr;
		if (Root->TryGetObjectField(TEXT("commands"), CommandsObject) && CommandsObject && CommandsObject->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*CommandsObject)->Values)
			{
				const TArray<TSharedPtr<FJsonValue>>* CommandValues = nullptr;
				if (!Pair.Value.IsValid() || !Pair.Value->TryGetArray(CommandValues) || CommandValues == nullptr)
				{
					Test.AddError(FString::Printf(TEXT("%s commands.%s not an array"), *CasePath, *Pair.Key));
					return false;
				}
				TArray<float> Command;
				if (!ParseFloatArray(CommandValues, Command, Error))
				{
					Test.AddError(Error);
					return false;
				}
				Inputs.Commands.Add(FName(*Pair.Key), MoveTemp(Command));
			}
		}

		TArray<float> PreviousActionStorage;
		const TArray<TSharedPtr<FJsonValue>>* PreviousValues = nullptr;
		if (Root->TryGetArrayField(TEXT("previous_action"), PreviousValues) && PreviousValues != nullptr)
		{
			if (!ParseFloatArray(PreviousValues, PreviousActionStorage, Error))
			{
				Test.AddError(Error);
				return false;
			}
			Inputs.PreviousAction = PreviousActionStorage;
		}

		TArray<float> Actual;
		const bool bExecuted = Runtime.Execute(Inputs, Actual, Error);
		if (bHasExpectedError && ErrorPhase.Equals(TEXT("execute")))
		{
			if (bExecuted)
			{
				Test.AddError(FString::Printf(TEXT("%s expected Execute failure"), *CasePath));
				return false;
			}
			if (!Error.Contains(ErrorContains))
			{
				Test.AddError(FString::Printf(
					TEXT("%s Execute error '%s' missing '%s'"), *CasePath, *Error, *ErrorContains));
				return false;
			}
			return true;
		}
		if (!bExecuted)
		{
			Test.AddError(FString::Printf(TEXT("%s Execute: %s"), *CasePath, *Error));
			return false;
		}

		const FString Label = FPaths::GetBaseFilename(CasePath);
		Test.TestEqual(*FString::Printf(TEXT("%s width"), *Label), Actual.Num(), Expected.Num());
		const int32 Count = FMath::Min(Actual.Num(), Expected.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Delta = FMath::Abs(Actual[Index] - Expected[Index]);
			Test.TestTrue(
				*FString::Printf(TEXT("%s[%d] |%g - %g| <= %g"), *Label, Index, Actual[Index], Expected[Index], Tolerance),
				Delta <= static_cast<float>(Tolerance));
		}
		return true;
	}

	bool CollectActionPlanCasePaths(const FString& CorpusRoot, TArray<FString>& OutPaths, FString& OutError)
	{
		const FString CategoryPath = FPaths::Combine(CorpusRoot, TEXT("action_plan"));
		if (!FPaths::DirectoryExists(CategoryPath))
		{
			OutError = FString::Printf(TEXT("missing action_plan corpus dir '%s'"), *CategoryPath);
			return false;
		}
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(CategoryPath, TEXT("*.json")), true, false);
		Names.Sort();
		OutPaths.Reset();
		for (const FString& Name : Names)
		{
			if (Name.EndsWith(TEXT(".expected.json")))
			{
				continue;
			}
			OutPaths.Add(FPaths::Combine(CategoryPath, Name));
		}
		if (OutPaths.Num() == 0)
		{
			OutError = FString::Printf(TEXT("no action_plan parity cases under '%s'"), *CategoryPath);
			return false;
		}
		return true;
	}

	bool RunActionPlanCase(FAutomationTestBase& Test, const FString& CasePath)
	{
		FString Error;
		TSharedPtr<FJsonObject> Root;
		if (!LoadJsonObject(CasePath, Root, Error))
		{
			Test.AddError(Error);
			return false;
		}

		const TSharedPtr<FJsonObject>* PlanObject = nullptr;
		if (!Root->TryGetObjectField(TEXT("plan"), PlanObject) || !PlanObject || !PlanObject->IsValid())
		{
			Test.AddError(FString::Printf(TEXT("%s missing 'plan' object"), *CasePath));
			return false;
		}

		const bool bHasExpected = Root->HasField(TEXT("expected"));
		const bool bHasExpectedError = Root->HasField(TEXT("expected_error"));
		if (bHasExpected == bHasExpectedError)
		{
			Test.AddError(FString::Printf(
				TEXT("%s must have exactly one of 'expected' / 'expected_error'"), *CasePath));
			return false;
		}

		FString ErrorPhase;
		FString ErrorContains;
		TArray<float> Expected;
		double Tolerance = 1.0e-6;
		if (bHasExpectedError)
		{
			const TSharedPtr<FJsonObject>* ErrorObject = nullptr;
			if (!Root->TryGetObjectField(TEXT("expected_error"), ErrorObject) || !ErrorObject || !ErrorObject->IsValid())
			{
				Test.AddError(FString::Printf(TEXT("%s expected_error must be an object"), *CasePath));
				return false;
			}
			if (!(*ErrorObject)->TryGetStringField(TEXT("phase"), ErrorPhase)
				|| !(*ErrorObject)->TryGetStringField(TEXT("contains"), ErrorContains)
				|| ErrorContains.IsEmpty()
				|| (!ErrorPhase.Equals(TEXT("compile")) && !ErrorPhase.Equals(TEXT("execute"))))
			{
				Test.AddError(FString::Printf(
					TEXT("%s expected_error needs phase (compile|execute) and non-empty contains"),
					*CasePath));
				return false;
			}
		}
		else
		{
			const TArray<TSharedPtr<FJsonValue>>* ExpectedValues = nullptr;
			if (!Root->TryGetArrayField(TEXT("expected"), ExpectedValues) || ExpectedValues == nullptr)
			{
				Test.AddError(FString::Printf(TEXT("%s missing required 'expected' array"), *CasePath));
				return false;
			}
			if (!ParseFloatArray(ExpectedValues, Expected, Error))
			{
				Test.AddError(FString::Printf(TEXT("%s expected: %s"), *CasePath, *Error));
				return false;
			}
			Root->TryGetNumberField(TEXT("tolerance"), Tolerance);
		}

		TMap<FName, int32> AvailableCommands;
		const TArray<TSharedPtr<FJsonValue>>* CommandLayoutValues = nullptr;
		if (Root->TryGetArrayField(TEXT("command_layout"), CommandLayoutValues) && CommandLayoutValues != nullptr)
		{
			if (!ParseNameWidthLayout(
					CommandLayoutValues, nullptr, &AvailableCommands, CasePath, TEXT("command_layout"), Error))
			{
				Test.AddError(Error);
				return false;
			}
		}

		const TArray<TSharedPtr<FJsonValue>>* PolicyValues = nullptr;
		if (!Root->TryGetArrayField(TEXT("policy_action"), PolicyValues) || PolicyValues == nullptr)
		{
			Test.AddError(FString::Printf(TEXT("%s missing 'policy_action'"), *CasePath));
			return false;
		}
		TArray<float> PolicyAction;
		if (!ParseFloatArray(PolicyValues, PolicyAction, Error))
		{
			Test.AddError(FString::Printf(TEXT("%s policy_action: %s"), *CasePath, *Error));
			return false;
		}

		FUERLActionPlan Plan;
		if (!Plan.ParseJson(PlanObject->ToSharedRef(), Error))
		{
			Test.AddError(FString::Printf(TEXT("%s plan parse: %s"), *CasePath, *Error));
			return false;
		}

		FUERLPlanRuntime Runtime;
		const bool bCompiled = Runtime.Compile(Plan, AvailableCommands, Error);
		if (bHasExpectedError && ErrorPhase.Equals(TEXT("compile")))
		{
			if (bCompiled)
			{
				Test.AddError(FString::Printf(TEXT("%s expected Compile failure"), *CasePath));
				return false;
			}
			if (!Error.Contains(ErrorContains))
			{
				Test.AddError(FString::Printf(
					TEXT("%s Compile error '%s' missing '%s'"), *CasePath, *Error, *ErrorContains));
				return false;
			}
			return true;
		}
		if (!bCompiled)
		{
			Test.AddError(FString::Printf(TEXT("%s Compile: %s"), *CasePath, *Error));
			return false;
		}

		FUERLPlanInputs Inputs;
		Inputs.PolicyAction = PolicyAction;
		TArray<float> Actual;
		const bool bExecuted = Runtime.Execute(Inputs, Actual, Error);
		if (bHasExpectedError && ErrorPhase.Equals(TEXT("execute")))
		{
			if (bExecuted)
			{
				Test.AddError(FString::Printf(TEXT("%s expected Execute failure"), *CasePath));
				return false;
			}
			if (!Error.Contains(ErrorContains))
			{
				Test.AddError(FString::Printf(
					TEXT("%s Execute error '%s' missing '%s'"), *CasePath, *Error, *ErrorContains));
				return false;
			}
			return true;
		}
		if (!bExecuted)
		{
			Test.AddError(FString::Printf(TEXT("%s Execute: %s"), *CasePath, *Error));
			return false;
		}

		const FString Label = FPaths::GetBaseFilename(CasePath);
		Test.TestEqual(*FString::Printf(TEXT("%s width"), *Label), Actual.Num(), Expected.Num());
		const int32 Count = FMath::Min(Actual.Num(), Expected.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Delta = FMath::Abs(Actual[Index] - Expected[Index]);
			Test.TestTrue(
				*FString::Printf(TEXT("%s[%d] |%g - %g| <= %g"), *Label, Index, Actual[Index], Expected[Index], Tolerance),
				Delta <= static_cast<float>(Tolerance));
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyOperatorParityCorpusTest,
	"UERL.Unit.Policy.Parity.AC_PARITY_OP.OperatorCorpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyOperatorParityCorpusTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();

	FString CorpusRoot;
	FString Error;
	TestTrue(TEXT("resolve operator parity corpus root"), ResolveOperatorCorpusRoot(CorpusRoot, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<FString> CasePaths;
	TestTrue(TEXT("traverse operator parity cases"), CollectOperatorCasePaths(CorpusRoot, CasePaths, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	bool bAllOk = true;
	for (const FString& CasePath : CasePaths)
	{
		if (!RunOperatorCase(*this, CasePath))
		{
			bAllOk = false;
		}
	}
	return bAllOk;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyActionPlanParityCorpusTest,
	"UERL.Unit.Policy.Parity.AC_PARITY_ACTION.ActionPlanCorpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyActionPlanParityCorpusTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();

	FString CorpusRoot;
	FString Error;
	TestTrue(TEXT("resolve action-plan parity corpus root"), ResolveOperatorCorpusRoot(CorpusRoot, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<FString> CasePaths;
	TestTrue(TEXT("traverse action_plan cases"), CollectActionPlanCasePaths(CorpusRoot, CasePaths, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	bool bAllOk = true;
	for (const FString& CasePath : CasePaths)
	{
		if (!RunActionPlanCase(*this, CasePath))
		{
			bAllOk = false;
		}
	}
	return bAllOk;
}

#endif // WITH_DEV_AUTOMATION_TESTS
