#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"
#include "UERLProvider.h"
#include "UERLSlotCollisionPlan.h"

class AActor;
class UWorld;

/** Borrow query bindings independently of Robot topology and observation caches. */
struct FUERLGroundQueryContext
{
	AActor* IgnoredActor = nullptr;
	FUERLSlotCollisionProfile CollisionProfile;
	const TArray<TWeakObjectPtr<AActor>>* PermittedActors = nullptr;
	EUERLTerrainQueryPurpose Purpose = EUERLTerrainQueryPurpose::TrainingOwned;
	TArray<FHitResult>* ScratchHits = nullptr;
};

/** Trace one segment using the existing Slot channel or ground-owner filtering. */
bool QueryUERLGroundHit(
	const FUERLGroundQueryContext& Query,
	UWorld& World,
	const FVector& Start,
	const FVector& End,
	const TCHAR* QueryName,
	FHitResult& OutHit,
	FString& OutError);
