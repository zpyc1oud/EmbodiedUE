#include "UERLGroundQuery.h"

#include "Chaos/ParticleHandle.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"

namespace
{
	FName ResolveGroundQueryTag(const TCHAR* QueryName)
	{
		static const FName GroundClearanceTag(TEXT("ground-clearance"));
		static const FName TerrainHeightTag(TEXT("terrain-height"));
		if (QueryName && QueryName[0] == TEXT('g') && FCString::Strcmp(QueryName, TEXT("ground-clearance")) == 0)
		{
			return GroundClearanceTag;
		}
		if (QueryName && QueryName[0] == TEXT('t') && FCString::Strcmp(QueryName, TEXT("terrain-height")) == 0)
		{
			return TerrainHeightTag;
		}
		return FName(QueryName);
	}

	void CollectActorGroundComponents(
		AActor* Actor,
		AActor* IgnoredActor,
		ECollisionChannel TargetChannel,
		TArray<UPrimitiveComponent*, TInlineAllocator<8>>& OutComponents)
	{
		if (!Actor || Actor == IgnoredActor || !IsValid(Actor))
		{
			return;
		}
		for (UActorComponent* Component : Actor->GetComponents())
		{
			UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Component);
			if (PrimComp
				&& PrimComp->IsRegistered()
				&& PrimComp->IsQueryCollisionEnabled()
				&& PrimComp->GetCollisionObjectType() == TargetChannel
				&& PrimComp->GetCollisionResponseToChannel(TargetChannel) == ECR_Block)
			{
				OutComponents.Add(PrimComp);
			}
		}
	}
}

void ResolveUERLPermittedGroundComponents(
	const FUERLGroundQueryContext& Query,
	TArray<UPrimitiveComponent*, TInlineAllocator<8>>& OutComponents)
{
	OutComponents.Reset();
	const bool bSharedWorld = Query.CollisionProfile.Scope() == EUERLEnvironmentCollisionScope::SharedWorld;
	if ((!bSharedWorld && Query.Purpose != EUERLTerrainQueryPurpose::ExplicitBinding)
		|| Query.Purpose == EUERLTerrainQueryPurpose::DeploymentWorldStatic)
	{
		return;
	}
	const ECollisionChannel TargetChannel = bSharedWorld
		? ECC_WorldStatic
		: Query.CollisionProfile.Channel();
	if (!Query.PermittedRawActors.IsEmpty())
	{
		for (AActor* Actor : Query.PermittedRawActors)
		{
			CollectActorGroundComponents(Actor, Query.IgnoredActor, TargetChannel, OutComponents);
		}
		return;
	}
	if (Query.PermittedActors)
	{
		for (const TWeakObjectPtr<AActor>& WeakActor : *Query.PermittedActors)
		{
			CollectActorGroundComponents(WeakActor.Get(), Query.IgnoredActor, TargetChannel, OutComponents);
		}
	}
}

void PrepareUERLPermittedGroundTargets_AssumesLocked(
	TConstArrayView<UPrimitiveComponent*> PermittedComponents,
	FUERLPreparedGroundTargets& OutTargets)
{
	OutTargets.BodyTargets.Reset();
	OutTargets.FallbackComponents.Reset();
	TArray<FPhysicsShapeHandle> Shapes;
	for (UPrimitiveComponent* Comp : PermittedComponents)
	{
		if (!Comp)
		{
			continue;
		}
		if (FBodyInstance* BI = Comp->GetBodyInstance(); BI && BI->IsValidBodyInstance())
		{
			const FBodyInstance* TargetBI = BI->WeldParent ? BI->WeldParent : BI;
			if (const FPhysicsActorHandle RigidBody = TargetBI->GetPhysicsActor())
			{
				Shapes.Reset();
				TargetBI->GetAllShapes_AssumesLocked(Shapes);
				FUERLPreparedGroundBodyTarget& Target = OutTargets.BodyTargets.AddDefaulted_GetRef();
				Target.OwnerActor = Comp->GetOwner();
				const auto& ActorAPI = RigidBody->GetGameThreadAPI();
				Target.WorldTM = FTransform(ActorAPI.R(), ActorAPI.X());
				Target.SimpleGeometries.Reserve(Shapes.Num());
				for (const FPhysicsShapeHandle& ShapeRef : Shapes)
				{
					if (!ShapeRef.IsValid() || !TargetBI->IsShapeBoundToBody(ShapeRef))
					{
						continue;
					}
					const Chaos::FPerShapeData* Shape = ShapeRef.Shape;
					if (!Shape || !Shape->GetQueryEnabled())
					{
						continue;
					}
					const Chaos::Filter::FShapeFilterData& ShapeFilter = Shape->GetShapeFilterData();
					if (!ShapeFilter.HasFlag(Chaos::EFilterFlags::SimpleCollision))
					{
						continue;
					}
					if (const Chaos::FImplicitObject* Geom = Shape->GetGeometry())
					{
						Target.SimpleGeometries.Add(Geom);
					}
				}
				continue;
			}
		}
		OutTargets.FallbackComponents.Add(Comp);
	}
}

bool QueryUERLGroundHit_PreparedLocked(
	const FUERLGroundQueryContext& Query,
	const FUERLPreparedGroundTargets& Targets,
	const FVector& Start,
	const FVector& End,
	const TCHAR* QueryName,
	FVector& OutImpactPoint,
	FString& OutError)
{
	const FVector Delta = End - Start;
	const double DeltaMag = Delta.Size();
	if (DeltaMag <= UE_KINDA_SMALL_NUMBER)
	{
		OutError = FString::Printf(
			TEXT("generic Robot %s query found no permitted WorldStatic ground"), QueryName);
		return false;
	}
	const FVector WorldDir = Delta / DeltaMag;
	double BestDistance = TNumericLimits<double>::Max();
	FVector BestImpactPoint = FVector::ZeroVector;
	bool bFound = false;

	for (const FUERLPreparedGroundBodyTarget& Target : Targets.BodyTargets)
	{
		if (Target.OwnerActor == Query.IgnoredActor || Target.SimpleGeometries.IsEmpty())
		{
			continue;
		}
		const FVector LocalStart = Target.WorldTM.InverseTransformPositionNoScale(Start);
		const FVector LocalDir = Target.WorldTM.InverseTransformVectorNoScale(WorldDir);
		for (const Chaos::FImplicitObject* Geom : Target.SimpleGeometries)
		{
			Chaos::FReal Distance = 0.0;
			Chaos::FVec3 LocalPosition = Chaos::FVec3::ZeroVector;
			Chaos::FVec3 LocalNormal = Chaos::FVec3::ZeroVector;
			int32 FaceIndex = INDEX_NONE;
			if (Geom->Raycast(LocalStart, LocalDir, DeltaMag, 0, Distance, LocalPosition, LocalNormal, FaceIndex)
				&& Distance < BestDistance)
			{
				BestDistance = Distance;
				BestImpactPoint = Target.WorldTM.TransformPositionNoScale(LocalPosition);
				bFound = true;
			}
		}
	}

	if (!Targets.FallbackComponents.IsEmpty())
	{
		FCollisionQueryParams QueryParams(FCollisionQueryParams::DefaultQueryParam);
		QueryParams.TraceTag = ResolveGroundQueryTag(QueryName);
		QueryParams.bTraceComplex = false;
		QueryParams.AddIgnoredActor(Query.IgnoredActor);
		FHitResult CandidateHit;
		for (UPrimitiveComponent* Comp : Targets.FallbackComponents)
		{
			if (!Comp || Comp->GetOwner() == Query.IgnoredActor)
			{
				continue;
			}
			CandidateHit.Reset();
			if (Comp->LineTraceComponent(CandidateHit, Start, End, QueryParams)
				&& CandidateHit.Distance < BestDistance)
			{
				BestDistance = CandidateHit.Distance;
				BestImpactPoint = CandidateHit.ImpactPoint;
				bFound = true;
			}
		}
	}

	if (!bFound)
	{
		OutError = FString::Printf(
			TEXT("generic Robot %s query found no permitted WorldStatic ground"), QueryName);
		return false;
	}
	OutImpactPoint = BestImpactPoint;
	return true;
}

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
	QueryParams.TraceTag = ResolveGroundQueryTag(QueryName);
	QueryParams.bTraceComplex = false;
	QueryParams.AddIgnoredActor(Query.IgnoredActor);
	if (!bSharedWorld && Query.Purpose != EUERLTerrainQueryPurpose::ExplicitBinding)
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
	const bool bHasPermittedComponents = !Query.PermittedComponents.IsEmpty();
	const bool bHasPermittedRaw = !Query.PermittedRawActors.IsEmpty();
	const bool bHasPermittedWeak = Query.PermittedActors && !Query.PermittedActors->IsEmpty();
	if (!bDeploymentWorldStatic && !bHasPermittedComponents && !bHasPermittedRaw && !bHasPermittedWeak)
	{
		OutError = FString::Printf(
			TEXT("generic Robot %s query has no training terrain owner"), QueryName);
		return false;
	}

	if (!bDeploymentWorldStatic)
	{
		TArray<UPrimitiveComponent*, TInlineAllocator<8>> LocalComponents;
		TConstArrayView<UPrimitiveComponent*> Components = Query.PermittedComponents;
		if (Components.IsEmpty())
		{
			ResolveUERLPermittedGroundComponents(Query, LocalComponents);
			Components = LocalComponents;
		}

		bool bFound = false;
		float BestTime = TNumericLimits<float>::Max();
		FHitResult BestHit;
		FHitResult CandidateHit;
		for (UPrimitiveComponent* Comp : Components)
		{
			if (!Comp || Comp->GetOwner() == Query.IgnoredActor)
			{
				continue;
			}
			CandidateHit.Reset();
			if (Comp->LineTraceComponent(CandidateHit, Start, End, QueryParams)
				&& CandidateHit.Time < BestTime)
			{
				BestTime = CandidateHit.Time;
				BestHit = CandidateHit;
				BestHit.bBlockingHit = true;
				bFound = true;
			}
		}
		if (!bFound)
		{
			OutError = FString::Printf(
				TEXT("generic Robot %s query found no permitted WorldStatic ground"), QueryName);
			return false;
		}
		OutHit = BestHit;
		return true;
	}

	TArray<FHitResult> LocalHits;
	TArray<FHitResult>* Hits = Query.ScratchHits ? Query.ScratchHits : &LocalHits;
	Hits->Reset();
	const bool bHit = World.LineTraceMultiByObjectType(
		*Hits, Start, End, FCollisionObjectQueryParams(bSharedWorld ? ECC_WorldStatic : Query.CollisionProfile.Channel()), QueryParams);
	const FHitResult* MatchedHit = nullptr;
	if (bHit)
	{
		for (const FHitResult& Candidate : *Hits)
		{
			if (Candidate.bBlockingHit)
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
