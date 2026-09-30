#pragma once

#include "CoreMinimal.h"
#include "UERLPhysicsSnapshot.h"
#include "UERLPolicyArtifact.h"

/** One actionable deployment-physics prerequisite failure or diagnostic. */
struct UERLPOLICY_API FUERLPhysicsGateItem
{
	FName Item;
	FString Actual;
	FString Requirement;
	FString Change;
};

/** Pure result of checking a deployment World against an artifact timing contract. */
struct UERLPOLICY_API FUERLPhysicsGateReport
{
	bool bPassed = true;
	TArray<FUERLPhysicsGateItem> Failures;
	TArray<FUERLPhysicsGateItem> Diagnostics;

	/** Render all failures with the actual value, requirement, and repair location. */
	FString ToString() const;
	/** Find a failure by its stable item name. */
	const FUERLPhysicsGateItem* FindFailure(FName Item) const;
};

/** Check the independent deployment physics contract without changing World settings. */
UERLPOLICY_API FUERLPhysicsGateReport CheckDeployPhysicsRequirements(
	const FUERLPhysicsSnapshot& Snapshot,
	const FUERLPolicyArtifactTiming& Timing);
