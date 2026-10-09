#include "UERLGenericRobotContactListener.h"

#include "UERLGenericRobotKinematics.h"

#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodyInstance.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/Collision/ParticleCollisions.h"
#include "Chaos/ParticleHandle.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

float ConvertGenericRobotAccumulatedImpulseToForceNewtons(
	const FVector& SolverStepImpulse,
	double SolverStepSeconds)
{
	checkf(
		FMath::IsFinite(SolverStepSeconds) && SolverStepSeconds > 0.0,
		TEXT("generic Robot contact force requires a positive solver-step dt"));
	return static_cast<float>(SolverStepImpulse.Size() / (100.0 * SolverStepSeconds));
}

void SampleGenericRobotContactForces(
	USkeletalMeshComponent& Component,
	const TArray<FName>& BodyNames,
	FGenericRobotContactState& ContactState,
	EUERLEnvironmentCollisionScope CollisionScope,
	double SolverStepSeconds)
{
	const bool bSharedWorld = CollisionScope == EUERLEnvironmentCollisionScope::SharedWorld;
	FPhysicsCommand::ExecuteRead(&Component, [&]()
	{
		// The shipped Robots fit inline. Larger topologies retain dynamic capacity.
		TArray<Chaos::FGeometryParticleHandle*, TInlineAllocator<32>> RobotParticles;
		TArray<Chaos::FPBDRigidParticleHandle*, TInlineAllocator<32>> BodyParticles;
		RobotParticles.Reserve(BodyNames.Num());
		BodyParticles.SetNumZeroed(BodyNames.Num());
		for (int32 BodyIndex = 0; BodyIndex < BodyNames.Num(); ++BodyIndex)
		{
			FBodyInstance* Body = Component.GetBodyInstance(BodyNames[BodyIndex]);
			if (!Body)
			{
				continue;
			}
			FPhysicsActorHandle Handle = Body->GetPhysicsActorHandle();
			Chaos::FGeometryParticleHandle* ParticleHandle = Handle != nullptr
				? Handle->GetHandle_LowLevel() : nullptr;
			if (!ParticleHandle)
			{
				continue;
			}
			RobotParticles.Add(ParticleHandle);
			BodyParticles[BodyIndex] = ParticleHandle->CastToRigidParticle();
		}

		for (int32 BodyIndex = 0; BodyIndex < BodyParticles.Num(); ++BodyIndex)
		{
			Chaos::FPBDRigidParticleHandle* Particle = BodyParticles[BodyIndex];
			if (!Particle)
			{
				continue;
			}

			FVector AccumulatedImpulse = FVector::ZeroVector;
			Particle->ParticleCollisions().VisitConstCollisions(
				[&](const Chaos::FPBDCollisionConstraint& Constraint)
				{
					Chaos::FGeometryParticleHandle* Particle0 = Constraint.GetParticle0();
					Chaos::FGeometryParticleHandle* Particle1 = Constraint.GetParticle1();
					if (Particle0 != Particle && Particle1 != Particle)
					{
						return Chaos::ECollisionVisitorResult::Continue;
					}
					Chaos::FGeometryParticleHandle* Other = Particle0 == Particle ? Particle1 : Particle0;
					if (!Other || RobotParticles.Contains(Other)
						|| (bSharedWorld && Other->CastToRigidParticle() != nullptr))
					{
						return Chaos::ECollisionVisitorResult::Continue;
					}
					const Chaos::FVec3f& Impulse = Constraint.AccumulatedImpulse;
					AccumulatedImpulse += FVector(
						static_cast<double>(Impulse.X),
						static_cast<double>(Impulse.Y),
						static_cast<double>(Impulse.Z));
					return Chaos::ECollisionVisitorResult::Continue;
				});
			ContactState.RecordContactForce(
				BodyIndex,
				ConvertGenericRobotAccumulatedImpulseToForceNewtons(AccumulatedImpulse, SolverStepSeconds));
		}
	});
}

bool IsGenericRobotBodySupportedByTerrain(
	USkeletalMeshComponent& Component,
	AActor& Owner,
	const FUERLRobotTopology& Topology,
	const FUERLSlotCollisionProfile& CollisionProfile,
	const FQuat& GroundRotation,
	const FTransform& RootCanonicalSlotTransform,
	int32 BodyIndex,
	TArray<FHitResult>& ScratchHits)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UERL_GenericRobot_FootSupportQuery);
	UWorld* World = Component.GetWorld();
	checkf(World, TEXT("generic Robot support query has no World"));
	checkf(Topology.BodyNames.IsValidIndex(BodyIndex), TEXT("generic Robot support query has invalid body"));
	FBodyInstance* Body = Component.GetBodyInstance(Topology.BodyNames[BodyIndex]);
	checkf(Body, TEXT("generic Robot support query has no body instance"));

	FTransform BodyWorld = Body->GetUnrealWorldTransform();
	if (BodyIndex == Topology.RootBodyIndex)
	{
		BodyWorld = RecoverGenericRobotRootWorld(RootCanonicalSlotTransform, BodyWorld);
	}
	const FVector GroundUp = GroundRotation.RotateVector(FVector::UpVector).GetSafeNormal();
	const FVector BodyLocation = BodyWorld.GetLocation();
	const FBox BodyBounds = Body->GetBodyBounds();
	const FVector BoundsCenter = BodyBounds.GetCenter();
	const FVector BoundsExtent = BodyBounds.GetExtent();
	const float LowestBoundsProjection = static_cast<float>(
		FVector::DotProduct(BoundsCenter, GroundUp)
		- FMath::Abs(GroundUp.X) * BoundsExtent.X
		- FMath::Abs(GroundUp.Y) * BoundsExtent.Y
		- FMath::Abs(GroundUp.Z) * BoundsExtent.Z);
	const float BodyProjection = static_cast<float>(FVector::DotProduct(BodyLocation, GroundUp));
	const FVector BottomProbe = BodyLocation + GroundUp * (LowestBoundsProjection - BodyProjection);
	// Probe just around the lowest point of the physics body.  This measures
	// geometric support even when the optimized terrain uses one merged
	// static mesh and therefore emits no repeated hit events.
	const FVector Start = BottomProbe + GroundUp * 4.0;
	const FVector End = BottomProbe - GroundUp * 3.0;
	const bool bSharedWorld = CollisionProfile.Scope() == EUERLEnvironmentCollisionScope::SharedWorld;
	const ECollisionChannel TraceChannel = bSharedWorld
		? ECC_WorldStatic
		: CollisionProfile.Channel();
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(UERLRobotFootSupport), false, &Owner);
	const auto IsSupportHit = [GroundUp](const FHitResult& Hit)
	{
		return Hit.bBlockingHit && FVector::DotProduct(Hit.ImpactNormal, GroundUp) >= 0.5;
	};

	if (!bSharedWorld)
	{
		FHitResult Hit;
		return World->LineTraceSingleByChannel(Hit, Start, End, TraceChannel, QueryParams)
			&& IsSupportHit(Hit);
	}

	// SharedWorld contact accepts any blocking WorldStatic support.  The
	// TerrainQueryActors whitelist is reserved for terrain-height scans.
	ScratchHits.Reset();
	if (!World->LineTraceMultiByObjectType(
		ScratchHits, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), QueryParams))
	{
		return false;
	}
	for (const FHitResult& Hit : ScratchHits)
	{
		if (IsSupportHit(Hit))
		{
			return true;
		}
	}
	return false;
}

bool IsGenericRobotEnvironmentHit(
	const FHitResult& Hit,
	EUERLEnvironmentCollisionScope CollisionScope,
	ECollisionChannel OtherObjectType,
	bool bBelongsToSlotEnvironment)
{
	return Hit.bBlockingHit && (CollisionScope == EUERLEnvironmentCollisionScope::SlotIsolated
		? bBelongsToSlotEnvironment
		: OtherObjectType == ECC_WorldStatic);
}

bool ResolveGenericRobotContactBodyIndex(
	const TMap<FName, int32>& BodyIndexByName,
	FName BodyName,
	int32& OutBodyIndex)
{
	const int32* BodyIndex = BodyIndexByName.Find(BodyName);
	if (BodyIndex == nullptr)
	{
		OutBodyIndex = INDEX_NONE;
		return false;
	}
	OutBodyIndex = *BodyIndex;
	return true;
}

void UERLGenericRobotContactListener::Bind(
	USkeletalMeshComponent& InComponent,
	FGenericRobotContactState& InState,
	EUERLEnvironmentCollisionScope InCollisionScope,
	const TArray<TWeakObjectPtr<AActor>>& InEnvironmentActors,
	const TArray<FName>& BodyNames)
{
	checkf(!BoundComponent.IsValid(), TEXT("generic Robot contact listener is already bound"));
	checkf(
		InCollisionScope == EUERLEnvironmentCollisionScope::SharedWorld || !InEnvironmentActors.IsEmpty(),
		TEXT("isolated generic Robot contact listener requires Environment actors"));
	BoundComponent = &InComponent;
	CollisionScope = InCollisionScope;
	EnvironmentActors = InEnvironmentActors;
	ContactState = &InState;
	BodyIndexByName.Reset();
	for (int32 BodyIndex = 0; BodyIndex < BodyNames.Num(); ++BodyIndex)
	{
		BodyIndexByName.Add(BodyNames[BodyIndex], BodyIndex);
	}
	InComponent.OnComponentHit.AddDynamic(this, &UERLGenericRobotContactListener::HandleHit);
}

void UERLGenericRobotContactListener::Unbind()
{
	if (BoundComponent.IsValid())
	{
		BoundComponent->OnComponentHit.RemoveDynamic(this, &UERLGenericRobotContactListener::HandleHit);
	}
	BoundComponent.Reset();
	CollisionScope = EUERLEnvironmentCollisionScope::SlotIsolated;
	EnvironmentActors.Reset();
	ContactState = nullptr;
	BodyIndexByName.Reset();
}

void UERLGenericRobotContactListener::BeginDestroy()
{
	Unbind();
	Super::BeginDestroy();
}

void UERLGenericRobotContactListener::HandleHit(
	UPrimitiveComponent* HitComponent,
	AActor* OtherActor,
	UPrimitiveComponent* OtherComp,
	FVector,
	const FHitResult& Hit)
{
	if (HitComponent != BoundComponent.Get() || !OtherComp || ContactState == nullptr)
	{
		return;
	}
	const bool bBelongsToSlotEnvironment = EnvironmentActors.ContainsByPredicate(
		[OtherActor](const TWeakObjectPtr<AActor>& Actor)
		{
			return Actor.Get() == OtherActor;
		});
	if (!IsGenericRobotEnvironmentHit(
		Hit, CollisionScope, OtherComp->GetCollisionObjectType(), bBelongsToSlotEnvironment))
	{
		return;
	}
	int32 BodyIndex = INDEX_NONE;
	if (ResolveGenericRobotContactBodyIndex(BodyIndexByName, Hit.MyBoneName, BodyIndex))
	{
		ContactState->MarkBody(BodyIndex);
	}
}
