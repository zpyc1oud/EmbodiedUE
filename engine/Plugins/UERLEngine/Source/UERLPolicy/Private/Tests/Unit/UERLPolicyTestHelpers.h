#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UERLInterfaceTypes.h"
#include "UERLPlan.h"

namespace UERLPolicyTestHelpers
{
	inline bool LoadJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
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

	inline bool ParseFloatArray(const TArray<TSharedPtr<FJsonValue>>* Values, TArray<float>& Out, FString& OutError)
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

	inline FUERLFieldDescriptor MakeLayoutField(
		const FString& Name,
		int32 Width,
		const FString& Semantic = TEXT("parity"))
	{
		FUERLFieldDescriptor Field;
		Field.Name = FName(*Name);
		Field.DType = TEXT("float32");
		Field.Shape = {Width};
		Field.Unit = TEXT("1");
		Field.CoordinateFrame = TEXT("world");
		Field.Semantic = Semantic;
		Field.Source = TEXT("robot");
		Field.Width = Width;
		return Field;
	}

	inline bool ResolveArtifactPath(const FString& RelativeOrAbsolute, FString& OutPath, FString& OutError)
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

	inline bool AssertFloatCloseAtolRtol(
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

	inline bool PackRawStateFromNamed(
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
			OutFields.Add(MakeLayoutField(Name, Chunk.Num(), TEXT("deploy_parity")));
			OutFlat.Append(Chunk);
		}
		return true;
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
