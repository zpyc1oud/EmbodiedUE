#include "UERLGroundQuery.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

bool QueryUERLGroundHit(
	const FUERLGroundQueryContext& Query,
	UWorld& World,
	const FVector& Start,
	const FVector& End,
	const TCHAR* QueryName,
	FHitResult& OutHit,
	FString& OutError)
{
	const bool bSharedWorld = Query.CollisionProfile.Scope() == EUERLEnvironmentCollisionScope::SharedWorld;
	const bool bDeploymentWorldStatic =
		Query.Purpose == EUERLTerrainQueryPurpose::DeploymentWorldStatic;
	FCollisionQueryParams QueryParams(FCollisionQueryParams::DefaultQueryParam);
	QueryParams.TraceTag = FName(QueryName);
	QueryParams.bTraceComplex = false;
	QueryParams.AddIgnoredActor(Query.IgnoredActor);
	if (!bSharedWorld)
	{
		if (!World.LineTraceSingleByChannel(
			OutHit, Start, End, Query.CollisionProfile.Channel(), QueryParams))
		{
			OutError = FString::Printf(
				TEXT("generic Robot %s query found no Slot-isolated ground"), QueryName);
			return false;
		}
		return true;
	}
	if (!bDeploymentWorldStatic && (!Query.PermittedActors || Query.PermittedActors->IsEmpty()))
	{
		OutError = FString::Printf(
			TEXT("generic Robot %s query has no training terrain owner"), QueryName);
		return false;
	}

	TArray<FHitResult> LocalHits;
	TArray<FHitResult>* Hits = Query.ScratchHits ? Query.ScratchHits : &LocalHits;
	Hits->Reset();
	const bool bHit = World.LineTraceMultiByObjectType(
		*Hits, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), QueryParams);
	const FHitResult* MatchedHit = nullptr;
	if (bHit)
	{
		for (const FHitResult& Candidate : *Hits)
		{
			if (!Candidate.bBlockingHit)
			{
				continue;
			}
			if (bDeploymentWorldStatic
				|| (Query.PermittedActors && Query.PermittedActors->ContainsByPredicate(
					[&Candidate](const TWeakObjectPtr<AActor>& Owner)
					{
						return Owner.Get() == Candidate.GetActor();
					})))
			{
				MatchedHit = &Candidate;
				break;
			}
		}
	}
	if (!MatchedHit)
	{
		OutError = FString::Printf(
			TEXT("generic Robot %s query found no permitted WorldStatic ground"), QueryName);
		return false;
	}
	OutHit = *MatchedHit;
	return true;
}
