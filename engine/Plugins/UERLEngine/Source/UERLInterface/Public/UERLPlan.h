#pragma once

#include "CoreMinimal.h"

class FJsonObject;

/**
 * Hold typed plan operator parameters.
 *
 * Numbers (including JSON bools as 0/1) land in Scalars; numeric arrays in
 * Vectors; strings in Strings. Compile-time operator checks live in later
 * tickets — this type only carries JSON data.
 */
struct FUERLPlanParams
{
	TMap<FName, double> Scalars;
	TMap<FName, TArray<double>> Vectors;
	TMap<FName, FString> Strings;

	void Reset()
	{
		Scalars.Reset();
		Vectors.Reset();
		Strings.Reset();
	}
};

/** One operator node before compile-time slot-index resolution. */
struct FUERLPlanOp
{
	FName Op;
	TArray<FName> InputNames;
	FName OutputName;
	int32 Width = 0;
	FUERLPlanParams Params;
};

/**
 * Versioned observation plan (data only).
 *
 * Lives in UERLInterface until ticket 05 introduces UERLPolicy; migrate the
 * header/implementation into that module then without changing the JSON
 * contract.
 */
struct FUERLObservationPlan
{
	int32 PlanVersion = 1;
	TArray<FName> StateRequirements;
	TArray<FUERLPlanOp> Ops;
	TMap<FName, TArray<FName>> Groups;
	TMap<FName, int32> GroupWidths;

	UERLINTERFACE_API void Reset();
	UERLINTERFACE_API bool ParseJson(const TSharedRef<FJsonObject>& Json, FString& OutError);
	UERLINTERFACE_API bool Validate(FString& OutError) const;
};

/** Versioned action plan (data only). Same migration path as observation plans. */
struct FUERLActionPlan
{
	int32 PlanVersion = 1;
	TArray<FUERLPlanOp> Ops;
	TArray<FName> CommandFields;
	int32 PolicyWidth = 0;

	UERLINTERFACE_API void Reset();
	UERLINTERFACE_API bool ParseJson(const TSharedRef<FJsonObject>& Json, FString& OutError);
	UERLINTERFACE_API bool Validate(FString& OutError) const;
};
