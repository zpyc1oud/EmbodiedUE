#pragma once

#include "CoreMinimal.h"
#include "Chaos/ImplicitObject.h"
#include "Engine/HitResult.h"
#include "UERLProvider.h"
#include "UERLSlotCollisionPlan.h"

class AActor;
class UPrimitiveComponent;
class UWorld;

/** Borrow query bindings independently of Robot topology and observation caches. */
struct FUERLGroundQueryContext
{
	AActor* IgnoredActor = nullptr;
	FUERLSlotCollisionProfile CollisionProfile;
	const TArray<TWeakObjectPtr<AActor>>* PermittedActors = nullptr;
	EUERLTerrainQueryPurpose Purpose = EUERLTerrainQueryPurpose::TrainingOwned;
	TArray<FHitResult>* ScratchHits = nullptr;
	TConstArrayView<AActor*> PermittedRawActors;
	TConstArrayView<UPrimitiveComponent*> PermittedComponents;
};

/** Pre-resolved Chaos simple-shape target for lock-free raycasts within an active ExecuteRead scope. */
struct FUERLPreparedGroundBodyTarget
{
	AActor* OwnerActor = nullptr;
	FTransform WorldTM = FTransform::Identity;
	TArray<const Chaos::FImplicitObject*, TInlineAllocator<4>> SimpleGeometries;
};

/** Snapshot of query-eligible permitted ground shapes valid for the enclosing physics read lock. */
struct FUERLPreparedGroundTargets
{
	TArray<FUERLPreparedGroundBodyTarget, TInlineAllocator<4>> BodyTargets;
	TArray<UPrimitiveComponent*, TInlineAllocator<4>> FallbackComponents;
};

/** Resolve query-eligible ground primitives owned by permitted terrain Actors. */
void ResolveUERLPermittedGroundComponents(
	const FUERLGroundQueryContext& Query,
	TArray<UPrimitiveComponent*, TInlineAllocator<8>>& OutComponents);

/** Populate Chaos simple-shape raycast targets for PermittedComponents inside ExecuteRead. */
void PrepareUERLPermittedGroundTargets_AssumesLocked(
	TConstArrayView<UPrimitiveComponent*> PermittedComponents,
	FUERLPreparedGroundTargets& OutTargets);

/** Raycast prepared ground shapes inside ExecuteRead, returning World ImpactPoint without SQ locks. */
bool QueryUERLGroundHit_PreparedLocked(
	const FUERLGroundQueryContext& Query,
	const FUERLPreparedGroundTargets& Targets,
	const FVector& Start,
	const FVector& End,
	const TCHAR* QueryName,
	FVector& OutImpactPoint,
	FString& OutError);

/** Trace one segment using the existing Slot channel or ground-owner filtering. */
bool QueryUERLGroundHit(
	const FUERLGroundQueryContext& Query,
	UWorld& World,
	const FVector& Start,
	const FVector& End,
	const TCHAR* QueryName,
	FHitResult& OutHit,
	FString& OutError);
