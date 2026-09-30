#pragma once

#include "CoreMinimal.h"

class UUERLPolicyArtifactAsset;
class UWorld;

/** One actionable project-health finding reported by the editor checks. */
struct FUERLProjectCheckIssue
{
	FName Category;
	FString Message;
};

namespace UERLPolicyEditorChecks
{
	/** Pure packaging readiness decision for projects on installed engine builds. */
	bool EvaluatePackagingReadiness(bool bHasGameModule, bool bHasPrebuiltGameBinaries, FString& OutMessage);

	/** Whether any prebuilt game-target UERL binary exists in the project or plugin Binaries. */
	bool HasPrebuiltGameBinaries(FString& OutFoundPath);

	/** Report artifact integrity issues (bytes/mesh) for every policy artifact asset. */
	int32 CollectArtifactIssues(TArray<FUERLProjectCheckIssue>& OutIssues);

	/** Report deploy-physics gate failures for every policy artifact asset in the project. */
	int32 CollectDeployPhysicsIssues(UWorld& World, TArray<FUERLProjectCheckIssue>& OutIssues);
}
