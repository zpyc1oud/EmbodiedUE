#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UERLPolicyArtifact.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	bool ResolveArtifactCorpusDir(FString& OutDir, FString& OutError)
	{
		TArray<FString> Candidates;
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT(".."), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("artifact"))));
		Candidates.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectDir(), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("artifact"))));

		for (const FString& Candidate : Candidates)
		{
			if (FPaths::DirectoryExists(Candidate))
			{
				OutDir = Candidate;
				return true;
			}
		}

		OutError = TEXT("artifact corpus directory not found; tried:");
		for (const FString& Candidate : Candidates)
		{
			OutError += FString::Printf(TEXT("\n  %s"), *Candidate);
		}
		return false;
	}

	bool ListArtifactCases(const FString& CorpusDir, TArray<FString>& OutArtifactPaths, FString& OutError)
	{
		// Traverse the corpus directory — do not hard-code case names (testing.md §5.2).
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *FPaths::Combine(CorpusDir, TEXT("*.uerlpol2")), true, false);
		Names.Sort();
		OutArtifactPaths.Reset();
		for (const FString& Name : Names)
		{
			OutArtifactPaths.Add(FPaths::Combine(CorpusDir, Name));
		}
		if (OutArtifactPaths.Num() == 0)
		{
			OutError = FString::Printf(TEXT("no *.uerlpol2 cases under '%s'"), *CorpusDir);
			return false;
		}
		return true;
	}

	bool LoadExpected(const FString& Path, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			OutError = FString::Printf(TEXT("could not read expected json '%s'"), *Path);
			return false;
		}
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = TEXT("expected json failed to deserialize");
			return false;
		}
		return true;
	}

	bool ReadArtifactJson(const TArray<uint8>& Bytes, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		if (Bytes.Num() < 16)
		{
			OutError = TEXT("artifact bytes are too short for a JSON segment");
			return false;
		}
		uint32 JsonLength = 0;
		FMemory::Memcpy(&JsonLength, Bytes.GetData() + 12, sizeof(JsonLength));
		if (JsonLength > static_cast<uint32>(Bytes.Num() - 16))
		{
			OutError = TEXT("artifact JSON segment exceeds the input");
			return false;
		}
		const FUTF8ToTCHAR Converted(
			reinterpret_cast<const ANSICHAR*>(Bytes.GetData() + 16), static_cast<int32>(JsonLength));
		const FString JsonText(Converted.Length(), Converted.Get());
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = TEXT("artifact JSON segment failed to deserialize");
			return false;
		}
		return true;
	}

	bool RewriteArtifactJson(
		const TArray<uint8>& Original,
		const TSharedRef<FJsonObject>& Root,
		TArray<uint8>& OutBytes,
		FString& OutError)
	{
		if (Original.Num() < 16)
		{
			OutError = TEXT("artifact bytes are too short for JSON rewrite");
			return false;
		}
		uint32 OriginalJsonLength = 0;
		FMemory::Memcpy(&OriginalJsonLength, Original.GetData() + 12, sizeof(OriginalJsonLength));
		const int64 OnnxLengthOffset = 16ll + OriginalJsonLength;
		if (OnnxLengthOffset < 16 || OnnxLengthOffset + 8 > Original.Num())
		{
			OutError = TEXT("artifact JSON length is invalid for rewrite");
			return false;
		}
		uint64 OnnxLength = 0;
		FMemory::Memcpy(&OnnxLength, Original.GetData() + OnnxLengthOffset, sizeof(OnnxLength));
		if (OnnxLength > static_cast<uint64>(Original.Num()) - static_cast<uint64>(OnnxLengthOffset + 8))
		{
			OutError = TEXT("artifact ONNX length is invalid for rewrite");
			return false;
		}

		FString JsonText;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonText);
		if (!FJsonSerializer::Serialize(Root, Writer))
		{
			OutError = TEXT("artifact JSON rewrite failed to serialize");
			return false;
		}
		const FTCHARToUTF8 JsonUtf8(*JsonText);
		if (static_cast<uint64>(JsonUtf8.Length()) > static_cast<uint64>(TNumericLimits<uint32>::Max()))
		{
			OutError = TEXT("artifact JSON rewrite is too large");
			return false;
		}

		OutBytes.Reset();
		OutBytes.Append(Original.GetData(), 16);
		const uint32 NewJsonLength = static_cast<uint32>(JsonUtf8.Length());
		FMemory::Memcpy(OutBytes.GetData() + 12, &NewJsonLength, sizeof(NewJsonLength));
		OutBytes.Append(reinterpret_cast<const uint8*>(JsonUtf8.Get()), JsonUtf8.Length());
		OutBytes.Append(
			Original.GetData() + OnnxLengthOffset,
			static_cast<int32>(8 + OnnxLength));
		return true;
	}

	void ExpectStringArrayField(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const TArray<FName>& Actual,
		const FString& Label)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		Test.TestTrue(*(Label + TEXT(" array present")), Object->TryGetArrayField(Field, Values) && Values != nullptr);
		if (Values == nullptr)
		{
			return;
		}
		Test.TestEqual(*(Label + TEXT(" count")), Actual.Num(), Values->Num());
		for (int32 Index = 0; Index < Values->Num() && Index < Actual.Num(); ++Index)
		{
			FString Expected;
			(*Values)[Index]->TryGetString(Expected);
			Test.TestEqual(
				*FString::Printf(TEXT("%s[%d]"), *Label, Index),
				Actual[Index],
				FName(*Expected));
		}
	}

	void ExpectIntArrayField(
		FAutomationTestBase& Test,
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Field,
		const TArray<int32>& Actual,
		const FString& Label)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		Test.TestTrue(*(Label + TEXT(" array present")), Object->TryGetArrayField(Field, Values) && Values != nullptr);
		if (Values == nullptr)
		{
			return;
		}
		Test.TestEqual(*(Label + TEXT(" count")), Actual.Num(), Values->Num());
		for (int32 Index = 0; Index < Values->Num() && Index < Actual.Num(); ++Index)
		{
			const int32 Expected = static_cast<int32>((*Values)[Index]->AsNumber());
			Test.TestEqual(*FString::Printf(TEXT("%s[%d]"), *Label, Index), Actual[Index], Expected);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyArtifactLoadCorpusTest,
	"UERL.Unit.Policy.Artifact.AC_UE_UNIT_ARTIFACT_001.LoadPythonCorpus",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyArtifactLoadCorpusTest::RunTest(const FString& Parameters)
{
	FString CorpusDir;
	FString Error;
	TestTrue(TEXT("resolve artifact corpus directory"), ResolveArtifactCorpusDir(CorpusDir, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<FString> ArtifactPaths;
	TestTrue(TEXT("traverse *.uerlpol2 cases"), ListArtifactCases(CorpusDir, ArtifactPaths, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	for (const FString& ArtifactPath : ArtifactPaths)
	{
		const FString Stem = FPaths::GetBaseFilename(ArtifactPath);
		const FString ExpectedPath = FPaths::Combine(CorpusDir, Stem + TEXT(".expected.json"));
		TestTrue(*FString::Printf(TEXT("expected json beside %s"), *Stem), FPaths::FileExists(ExpectedPath));

		TSharedPtr<FJsonObject> Expected;
		TestTrue(*FString::Printf(TEXT("load expected json for %s"), *Stem), LoadExpected(ExpectedPath, Expected, Error));
		if (!Expected.IsValid())
		{
			AddError(Error);
			return false;
		}

		FUERLPolicyArtifact Artifact;
		TestTrue(*FString::Printf(TEXT("Load accepts Python-written corpus %s"), *Stem), Artifact.Load(ArtifactPath, Error));
		if (!Error.IsEmpty())
		{
			AddError(Error);
			return false;
		}

		FString ExpectedTask;
		FString ExpectedRobot;
		Expected->TryGetStringField(TEXT("task_id"), ExpectedTask);
		Expected->TryGetStringField(TEXT("robot_id"), ExpectedRobot);
		TestEqual(*FString::Printf(TEXT("%s task_id"), *Stem), Artifact.TaskId(), FName(*ExpectedTask));
		TestEqual(*FString::Printf(TEXT("%s robot_id"), *Stem), Artifact.RobotId(), FName(*ExpectedRobot));

		const TSharedPtr<FJsonObject>* ObservationExpected = nullptr;
		Expected->TryGetObjectField(TEXT("observation"), ObservationExpected);
		TestTrue(*FString::Printf(TEXT("%s observation object"), *Stem), ObservationExpected && ObservationExpected->IsValid());
		if (ObservationExpected && ObservationExpected->IsValid())
		{
			TArray<FName> OpNames;
			TArray<int32> Widths;
			for (const FUERLPlanOp& Op : Artifact.ObservationPlan().Ops)
			{
				OpNames.Add(Op.Op);
				Widths.Add(Op.Width);
			}
			ExpectStringArrayField(*this, *ObservationExpected, TEXT("ops"), OpNames, Stem + TEXT(" obs.ops"));
			ExpectIntArrayField(*this, *ObservationExpected, TEXT("widths"), Widths, Stem + TEXT(" obs.widths"));

			const TSharedPtr<FJsonObject>* GroupsExpected = nullptr;
			(*ObservationExpected)->TryGetObjectField(TEXT("groups"), GroupsExpected);
			TestTrue(*FString::Printf(TEXT("%s obs.groups object"), *Stem), GroupsExpected && GroupsExpected->IsValid());
			if (GroupsExpected && GroupsExpected->IsValid())
			{
				TestEqual(
					*FString::Printf(TEXT("%s obs.groups count"), *Stem),
					Artifact.ObservationPlan().Groups.Num(),
					(*GroupsExpected)->Values.Num());
				for (const TPair<FName, TArray<FName>>& Pair : Artifact.ObservationPlan().Groups)
				{
					const TArray<TSharedPtr<FJsonValue>>* Members = nullptr;
					(*GroupsExpected)->TryGetArrayField(Pair.Key.ToString(), Members);
					TestTrue(*FString::Printf(TEXT("%s group %s present"), *Stem, *Pair.Key.ToString()), Members != nullptr);
					if (Members)
					{
						TestEqual(
							*FString::Printf(TEXT("%s group %s size"), *Stem, *Pair.Key.ToString()),
							Pair.Value.Num(),
							Members->Num());
						for (int32 Index = 0; Index < Members->Num() && Index < Pair.Value.Num(); ++Index)
						{
							FString ExpectedMember;
							(*Members)[Index]->TryGetString(ExpectedMember);
							TestEqual(
								*FString::Printf(TEXT("%s group %s[%d]"), *Stem, *Pair.Key.ToString(), Index),
								Pair.Value[Index],
								FName(*ExpectedMember));
						}
					}
				}
			}

			const TSharedPtr<FJsonObject>* GroupWidthsExpected = nullptr;
			(*ObservationExpected)->TryGetObjectField(TEXT("group_widths"), GroupWidthsExpected);
			TestTrue(
				*FString::Printf(TEXT("%s obs.group_widths object"), *Stem),
				GroupWidthsExpected && GroupWidthsExpected->IsValid());
			if (GroupWidthsExpected && GroupWidthsExpected->IsValid())
			{
				TestEqual(
					*FString::Printf(TEXT("%s obs.group_widths count"), *Stem),
					Artifact.ObservationPlan().GroupWidths.Num(),
					(*GroupWidthsExpected)->Values.Num());
				for (const TPair<FName, int32>& Pair : Artifact.ObservationPlan().GroupWidths)
				{
					double ExpectedWidth = 0.0;
					TestTrue(
						*FString::Printf(TEXT("%s group_width %s present"), *Stem, *Pair.Key.ToString()),
						(*GroupWidthsExpected)->TryGetNumberField(Pair.Key.ToString(), ExpectedWidth));
					TestEqual(
						*FString::Printf(TEXT("%s group_width %s"), *Stem, *Pair.Key.ToString()),
						Pair.Value,
						static_cast<int32>(ExpectedWidth));
				}
			}
		}

		const TSharedPtr<FJsonObject>* ActionExpected = nullptr;
		Expected->TryGetObjectField(TEXT("action"), ActionExpected);
		TestTrue(*FString::Printf(TEXT("%s action object"), *Stem), ActionExpected && ActionExpected->IsValid());
		if (ActionExpected && ActionExpected->IsValid())
		{
			TArray<FName> OpNames;
			TArray<int32> Widths;
			for (const FUERLPlanOp& Op : Artifact.ActionPlan().Ops)
			{
				OpNames.Add(Op.Op);
				Widths.Add(Op.Width);
			}
			ExpectStringArrayField(*this, *ActionExpected, TEXT("ops"), OpNames, Stem + TEXT(" action.ops"));
			ExpectIntArrayField(*this, *ActionExpected, TEXT("widths"), Widths, Stem + TEXT(" action.widths"));
			ExpectStringArrayField(
				*this,
				*ActionExpected,
				TEXT("command_fields"),
				Artifact.ActionPlan().CommandFields,
				Stem + TEXT(" action.command_fields"));
			double ExpectedPolicyWidth = 0.0;
			TestTrue(
				*FString::Printf(TEXT("%s action.policy_width present"), *Stem),
				(*ActionExpected)->TryGetNumberField(TEXT("policy_width"), ExpectedPolicyWidth));
			TestEqual(
				*FString::Printf(TEXT("%s action.policy_width"), *Stem),
				Artifact.ActionPlan().PolicyWidth,
				static_cast<int32>(ExpectedPolicyWidth));
		}

		const TSharedPtr<FJsonObject>* RobotRuntimeExpected = nullptr;
		Expected->TryGetObjectField(TEXT("robot_runtime"), RobotRuntimeExpected);
		TestTrue(
			*FString::Printf(TEXT("%s robot_runtime object"), *Stem),
			RobotRuntimeExpected && RobotRuntimeExpected->IsValid());
		if (RobotRuntimeExpected && RobotRuntimeExpected->IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* ExpectedActuators = nullptr;
			TestTrue(
				*FString::Printf(TEXT("%s robot_runtime.actuators present"), *Stem),
				(*RobotRuntimeExpected)->TryGetArrayField(TEXT("actuators"), ExpectedActuators)
					&& ExpectedActuators != nullptr);
			if (ExpectedActuators != nullptr)
			{
				TestEqual(
					*FString::Printf(TEXT("%s robot_runtime.actuators count"), *Stem),
					Artifact.RobotRuntime().Actuators.Num(),
					ExpectedActuators->Num());
				for (int32 Index = 0;
					Index < ExpectedActuators->Num() && Index < Artifact.RobotRuntime().Actuators.Num();
					++Index)
				{
					const TSharedPtr<FJsonObject>* ActuatorObject = nullptr;
					TestTrue(
						*FString::Printf(TEXT("%s actuator[%d] object"), *Stem, Index),
						(*ExpectedActuators)[Index].IsValid()
							&& (*ExpectedActuators)[Index]->TryGetObject(ActuatorObject)
							&& ActuatorObject
							&& ActuatorObject->IsValid());
					if (!ActuatorObject || !ActuatorObject->IsValid())
					{
						continue;
					}
					FString ExpectedJoint;
					double ExpectedStiffness = 0.0;
					double ExpectedDamping = 0.0;
					double ExpectedEffort = 0.0;
					double ExpectedDefault = 0.0;
					(*ActuatorObject)->TryGetStringField(TEXT("joint"), ExpectedJoint);
					(*ActuatorObject)->TryGetNumberField(TEXT("stiffness"), ExpectedStiffness);
					(*ActuatorObject)->TryGetNumberField(TEXT("damping"), ExpectedDamping);
					(*ActuatorObject)->TryGetNumberField(TEXT("effort_limit"), ExpectedEffort);
					(*ActuatorObject)->TryGetNumberField(TEXT("default_position"), ExpectedDefault);
					const FUERLPolicyRobotRuntimeActuator& Actual = Artifact.RobotRuntime().Actuators[Index];
					TestEqual(
						*FString::Printf(TEXT("%s actuator[%d].joint"), *Stem, Index),
						Actual.JointName,
						FName(*ExpectedJoint));
					TestEqual(
						*FString::Printf(TEXT("%s actuator[%d].stiffness"), *Stem, Index),
						Actual.Stiffness,
						ExpectedStiffness);
					TestEqual(
						*FString::Printf(TEXT("%s actuator[%d].damping"), *Stem, Index),
						Actual.Damping,
						ExpectedDamping);
					TestEqual(
						*FString::Printf(TEXT("%s actuator[%d].effort_limit"), *Stem, Index),
						Actual.EffortLimit,
						ExpectedEffort);
					TestEqual(
						*FString::Printf(TEXT("%s actuator[%d].default_position"), *Stem, Index),
						Actual.DefaultPosition,
						ExpectedDefault);
				}
			}
		}

		// Trust Load's ONNX payload; compare against reviewed expected length (no re-slice).
		double ExpectedNBytes = 0.0;
		TestTrue(
			*FString::Printf(TEXT("%s onnx_nbytes present"), *Stem),
			Expected->TryGetNumberField(TEXT("onnx_nbytes"), ExpectedNBytes));
		TestEqual(
			*FString::Printf(TEXT("%s onnx byte count"), *Stem),
			Artifact.OnnxBytes().Num(),
			static_cast<int32>(ExpectedNBytes));

		FUERLPolicyArtifact Reloaded;
		TestTrue(*FString::Printf(TEXT("%s reload"), *Stem), Reloaded.Load(ArtifactPath, Error));
		TestEqual(
			*FString::Printf(TEXT("%s reload onnx count"), *Stem),
			Reloaded.OnnxBytes().Num(),
			Artifact.OnnxBytes().Num());
		TestTrue(
			*FString::Printf(TEXT("%s reload onnx bitwise"), *Stem),
			Reloaded.OnnxBytes().Num() == Artifact.OnnxBytes().Num()
				&& FMemory::Memcmp(
					   Reloaded.OnnxBytes().GetData(),
					   Artifact.OnnxBytes().GetData(),
					   Artifact.OnnxBytes().Num())
					== 0);
	}

	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_ARTIFACT_001: traverse corpus; Load plans + ONNX match expected"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyArtifactCorruptionsTest,
	"UERL.Unit.Policy.Artifact.AC_UE_UNIT_ARTIFACT_002.CorruptContainers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyArtifactCorruptionsTest::RunTest(const FString& Parameters)
{
	FString CorpusDir;
	FString Error;
	TestTrue(TEXT("resolve artifact corpus directory"), ResolveArtifactCorpusDir(CorpusDir, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<FString> ArtifactPaths;
	TestTrue(TEXT("traverse *.uerlpol2 cases"), ListArtifactCases(CorpusDir, ArtifactPaths, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<uint8> Good;
	if (!TestTrue(TEXT("read first corpus as corruption baseline"),
		FFileHelper::LoadFileToArray(Good, *ArtifactPaths[0])))
	{
		return false;
	}
	FUERLPolicyArtifact Baseline;
	if (!TestTrue(TEXT("corruption baseline is a valid artifact"), Baseline.LoadFromBytes(Good, Error)))
	{
		AddError(Error);
		return false;
	}

	auto ExpectLoadFails = [this](const TArray<uint8>& Bytes, const TCHAR* Label, const TCHAR* ExpectedToken)
	{
		FUERLPolicyArtifact Artifact;
		FString LoadError;
		const bool bOk = Artifact.LoadFromBytes(Bytes, LoadError);
		TestFalse(*FString::Printf(TEXT("%s must fail Load"), Label), bOk);
		TestFalse(*FString::Printf(TEXT("%s must not mark loaded"), Label), Artifact.IsLoaded());
		TestFalse(*FString::Printf(TEXT("%s must report error"), Label), LoadError.IsEmpty());
		TestTrue(
			*FString::Printf(TEXT("%s error mentions %s"), Label, ExpectedToken),
			LoadError.Contains(ExpectedToken, ESearchCase::IgnoreCase));
	};

	{
		TArray<uint8> BadMagic = Good;
		const ANSICHAR Poison[] = "BADMAGIC";
		FMemory::Memcpy(BadMagic.GetData(), Poison, 8);
		ExpectLoadFails(BadMagic, TEXT("bad_magic"), TEXT("magic"));
	}
	{
		TArray<uint8> BadVersion = Good;
		const uint32 WrongVersion = 99;
		FMemory::Memcpy(BadVersion.GetData() + 8, &WrongVersion, sizeof(WrongVersion));
		ExpectLoadFails(BadVersion, TEXT("bad_version"), TEXT("version"));
	}
	{
		TArray<uint8> Oob = Good;
		uint32 JsonLength = 0;
		FMemory::Memcpy(&JsonLength, Good.GetData() + 12, sizeof(JsonLength));
		const int32 OnnxLenOffset = 16 + static_cast<int32>(JsonLength);
		const uint64 Huge = static_cast<uint64>(Good.Num()) + 1000ull;
		FMemory::Memcpy(Oob.GetData() + OnnxLenOffset, &Huge, sizeof(Huge));
		ExpectLoadFails(Oob, TEXT("onnx_oob"), TEXT("onnx"));
	}
	{
		uint32 JsonLength = 0;
		FMemory::Memcpy(&JsonLength, Good.GetData() + 12, sizeof(JsonLength));
		const int32 TruncateAt = 16 + FMath::Max(1, static_cast<int32>(JsonLength) / 2);
		TArray<uint8> Truncated;
		Truncated.Append(Good.GetData(), TruncateAt);
		ExpectLoadFails(Truncated, TEXT("json_truncated"), TEXT("json"));
	}

	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_ARTIFACT_002: four corruptions fail Load with distinct errors"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyArtifactTimingTest,
	"UERL.Unit.Policy.Artifact.AC_UE_UNIT_TIMING_001.TimingAndDerivedBounds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyArtifactTimingTest::RunTest(const FString& Parameters)
{
	FString CorpusDir;
	FString Error;
	TestTrue(TEXT("resolve artifact corpus directory"), ResolveArtifactCorpusDir(CorpusDir, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<FString> ArtifactPaths;
	TestTrue(TEXT("traverse *.uerlpol2 cases"), ListArtifactCases(CorpusDir, ArtifactPaths, Error));
	if (!Error.IsEmpty() || ArtifactPaths.Num() == 0)
	{
		AddError(Error.IsEmpty() ? TEXT("artifact corpus is empty") : Error);
		return false;
	}

	TArray<uint8> Good;
	TestTrue(TEXT("read timing corpus"), FFileHelper::LoadFileToArray(Good, *ArtifactPaths[0]));
	TSharedPtr<FJsonObject> Root;
	TestTrue(TEXT("parse timing corpus JSON"), ReadArtifactJson(Good, Root, Error));
	if (!Root.IsValid())
	{
		AddError(Error);
		return false;
	}

	FUERLPolicyArtifact Artifact;
	TestTrue(TEXT("timing corpus loads"), Artifact.LoadFromBytes(Good, Error));
	if (!Artifact.IsLoaded())
	{
		AddError(Error);
		return false;
	}
	const TSharedPtr<FJsonObject>* TimingObject = nullptr;
	TestTrue(TEXT("timing object is present"), Root->TryGetObjectField(TEXT("timing"), TimingObject));
	if (TimingObject == nullptr || !TimingObject->IsValid())
	{
		AddError(TEXT("timing object was not present in the corpus JSON"));
		return false;
	}
	double ExpectedPhysicsDt = 0.0;
	double ExpectedMin = 0.0;
	double ExpectedMax = 0.0;
	TestTrue(TEXT("timing physics_dt is present"), (*TimingObject)->TryGetNumberField(TEXT("physics_dt"), ExpectedPhysicsDt));
	TestTrue(TEXT("timing decimation_min is present"), (*TimingObject)->TryGetNumberField(TEXT("decimation_min"), ExpectedMin));
	TestTrue(TEXT("timing decimation_max is present"), (*TimingObject)->TryGetNumberField(TEXT("decimation_max"), ExpectedMax));
	TestTrue(TEXT("physics_dt round-trips"), FMath::IsNearlyEqual(Artifact.Timing().PhysicsDt, ExpectedPhysicsDt, 1.0e-12));
	TestEqual(TEXT("decimation_min round-trips"), Artifact.Timing().DecimationMin, static_cast<int32>(ExpectedMin));
	TestEqual(TEXT("decimation_max round-trips"), Artifact.Timing().DecimationMax, static_cast<int32>(ExpectedMax));
	TestTrue(
		TEXT("DtMin is derived from physics_dt and decimation_min"),
		FMath::IsNearlyEqual(Artifact.Timing().DtMin(), ExpectedPhysicsDt * ExpectedMin, 1.0e-12));
	TestTrue(
		TEXT("DtMax is derived from physics_dt and decimation_max"),
		FMath::IsNearlyEqual(Artifact.Timing().DtMax(), ExpectedPhysicsDt * ExpectedMax, 1.0e-12));

	auto ExpectTimingFailure = [this, &Good](const TSharedRef<FJsonObject>& MutatedRoot, const TCHAR* Label)
	{
		TArray<uint8> Mutated;
		FString RewriteError;
		TestTrue(*FString::Printf(TEXT("%s JSON rewrite"), Label), RewriteArtifactJson(Good, MutatedRoot, Mutated, RewriteError));
		if (!RewriteError.IsEmpty())
		{
			AddError(RewriteError);
			return;
		}
		FUERLPolicyArtifact MutatedArtifact;
		FString LoadError;
		TestFalse(*FString::Printf(TEXT("%s timing must be rejected"), Label), MutatedArtifact.LoadFromBytes(Mutated, LoadError));
		if (!LoadError.Contains(TEXT("timing"), ESearchCase::IgnoreCase))
		{
			AddError(FString::Printf(TEXT("%s load error did not mention timing: %s"), Label, *LoadError));
		}
	};

	TSharedPtr<FJsonObject> MissingTiming;
	TestTrue(TEXT("parse missing-timing baseline"), ReadArtifactJson(Good, MissingTiming, Error));
	if (MissingTiming.IsValid())
	{
		MissingTiming->RemoveField(TEXT("timing"));
		ExpectTimingFailure(MissingTiming.ToSharedRef(), TEXT("missing_timing"));
	}
	TSharedPtr<FJsonObject> NonFiniteTiming;
	TestTrue(TEXT("parse non-finite baseline"), ReadArtifactJson(Good, NonFiniteTiming, Error));
	if (NonFiniteTiming.IsValid())
	{
		TSharedRef<FJsonObject> BadTiming = MakeShared<FJsonObject>();
		BadTiming->SetStringField(TEXT("physics_dt"), TEXT("nan"));
		BadTiming->SetNumberField(TEXT("decimation_min"), ExpectedMin);
		BadTiming->SetNumberField(TEXT("decimation_max"), ExpectedMax);
		NonFiniteTiming->SetObjectField(TEXT("timing"), BadTiming);
		ExpectTimingFailure(NonFiniteTiming.ToSharedRef(), TEXT("non_finite_dt"));
	}
	TSharedPtr<FJsonObject> ReversedTiming;
	TestTrue(TEXT("parse reversed-range baseline"), ReadArtifactJson(Good, ReversedTiming, Error));
	if (ReversedTiming.IsValid())
	{
		TSharedRef<FJsonObject> BadTiming = MakeShared<FJsonObject>();
		BadTiming->SetNumberField(TEXT("physics_dt"), ExpectedPhysicsDt);
		BadTiming->SetNumberField(TEXT("decimation_min"), 2.0);
		BadTiming->SetNumberField(TEXT("decimation_max"), 1.0);
		ReversedTiming->SetObjectField(TEXT("timing"), BadTiming);
		ExpectTimingFailure(ReversedTiming.ToSharedRef(), TEXT("reversed_range"));
	}

	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_TIMING_001: timing and DtMin/DtMax round-trip; missing/non-finite/reversed values reject"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyArtifactIgnoresOnnxGraphTest,
	"UERL.Unit.Policy.Artifact.AC_UE_UNIT_ARTIFACT_003.LoadDoesNotParseOnnx",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyArtifactIgnoresOnnxGraphTest::RunTest(const FString& Parameters)
{
	FString CorpusDir;
	FString Error;
	TestTrue(TEXT("resolve artifact corpus directory"), ResolveArtifactCorpusDir(CorpusDir, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<FString> ArtifactPaths;
	TestTrue(TEXT("traverse *.uerlpol2 cases"), ListArtifactCases(CorpusDir, ArtifactPaths, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
		return false;
	}

	TArray<uint8> Good;
	if (!TestTrue(TEXT("read first corpus"), FFileHelper::LoadFileToArray(Good, *ArtifactPaths[0])))
	{
		return false;
	}
	FUERLPolicyArtifact Baseline;
	if (!TestTrue(TEXT("ONNX replacement baseline is a valid artifact"), Baseline.LoadFromBytes(Good, Error)))
	{
		AddError(Error);
		return false;
	}
	uint32 JsonLength = 0;
	FMemory::Memcpy(&JsonLength, Good.GetData() + 12, sizeof(JsonLength));
	const int32 OnnxLenOffset = 16 + static_cast<int32>(JsonLength);
	const int32 OnnxStart = OnnxLenOffset + 8;

	TArray<uint8> FakeOnnx;
	FakeOnnx.SetNumUninitialized(64);
	for (int32 Index = 0; Index < FakeOnnx.Num(); ++Index)
	{
		FakeOnnx[Index] = static_cast<uint8>(Index * 17 + 3);
	}
	TArray<uint8> Mutated;
	Mutated.Append(Good.GetData(), OnnxStart);
	const uint64 FakeLength = static_cast<uint64>(FakeOnnx.Num());
	FMemory::Memcpy(Mutated.GetData() + OnnxLenOffset, &FakeLength, sizeof(FakeLength));
	Mutated.Append(FakeOnnx);

	FUERLPolicyArtifact Artifact;
	TestTrue(
		TEXT("Load succeeds with structurally valid container even when ONNX is garbage"),
		Artifact.LoadFromBytes(Mutated, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	TestEqual(TEXT("onnx garbage byte count preserved"), Artifact.OnnxBytes().Num(), FakeOnnx.Num());
	TestTrue(
		TEXT("onnx garbage bytes preserved bitwise"),
		Artifact.OnnxBytes().Num() == FakeOnnx.Num()
			&& FMemory::Memcmp(Artifact.OnnxBytes().GetData(), FakeOnnx.GetData(), FakeOnnx.Num()) == 0);

	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_ARTIFACT_003: Load does not parse ONNX graph"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
