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

bool IsGenericRobotContactForceStateAccepted(
	EUERLEnvironmentCollisionScope CollisionScope,
	Chaos::EObjectStateType OtherState)
{
	return CollisionScope == EUERLEnvironmentCollisionScope::SlotIsolated
		|| OtherState == Chaos::EObjectStateType::Static
		|| OtherState == Chaos::EObjectStateType::Kinematic;
}

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
					// Movable non-simulated supports also have rigid particle handles.
					// Object state distinguishes them from simulated bodies.
					if (!Other || RobotParticles.Contains(Other)
						|| !IsGenericRobotContactForceStateAccepted(CollisionScope, Other->ObjectState()))
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

namespace
{
	bool TestFootSupportRay(
		UWorld& World,
		const FVector& Start,
		const FVector& End,
		const FVector& GroundUp,
		bool bSharedWorld,
		ECollisionChannel TraceChannel,
		const FCollisionQueryParams& QueryParams,
		TArray<FHitResult>& ScratchHits)
	{
		auto IsSupportHit = [GroundUp](const FHitResult& Hit)
		{
			return Hit.bBlockingHit && FVector::DotProduct(Hit.ImpactNormal, GroundUp) >= 0.5;
		};
		if (!bSharedWorld)
		{
			FHitResult Hit;
			return World.LineTraceSingleByChannel(Hit, Start, End, TraceChannel, QueryParams)
				&& IsSupportHit(Hit);
		}
		static const FCollisionObjectQueryParams SharedObjectParams(ECC_WorldStatic);
		FHitResult SingleHit;
		if (!World.LineTraceSingleByObjectType(SingleHit, Start, End, SharedObjectParams, QueryParams))
		{
			return false;
		}
		if (IsSupportHit(SingleHit))
		{
			return true;
		}
		ScratchHits.Reset();
		if (!World.LineTraceMultiByObjectType(ScratchHits, Start, End, SharedObjectParams, QueryParams))
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
	static const FCollisionQueryParams BaseQueryParams(SCENE_QUERY_STAT(UERLRobotFootSupport), false);
	FCollisionQueryParams QueryParams(BaseQueryParams);
	QueryParams.AddIgnoredActor(&Owner);
	return TestFootSupportRay(
		*World, Start, End, GroundUp, bSharedWorld, TraceChannel, QueryParams, ScratchHits);
}

void ReadGenericRobotSlotPhysicsContacts_AssumesLocked(
	USkeletalMeshComponent& Component,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FBodyInstance*> BodyInstances,
	const FTransform& RootCanonicalSlotTransform,
	TConstArrayView<int32> SupportBodyIndices,
	TConstArrayView<int32> ContactForceBodyIndices,
	FGenericRobotContactState& ContactState,
	EUERLEnvironmentCollisionScope CollisionScope,
	double SolverStepSeconds,
	TArray<FGenericRobotSupportProbeInput, TInlineAllocator<16>>& OutSupportInputs)
{
	OutSupportInputs.Reset();
	OutSupportInputs.Reserve(SupportBodyIndices.Num());
	if (SupportBodyIndices.IsEmpty() && ContactForceBodyIndices.IsEmpty())
	{
		return;
	}

	auto ResolveBody = [&](int32 BodyIndex) -> FBodyInstance*
	{
		if (BodyInstances.IsValidIndex(BodyIndex) && BodyInstances[BodyIndex])
		{
			return BodyInstances[BodyIndex];
		}
		return Topology.BodyNames.IsValidIndex(BodyIndex)
			? Component.GetBodyInstance(Topology.BodyNames[BodyIndex])
			: nullptr;
	};

	for (const int32 BodyIndex : SupportBodyIndices)
	{
		checkf(Topology.BodyNames.IsValidIndex(BodyIndex), TEXT("generic Robot support query has invalid body"));
		FBodyInstance* Body = ResolveBody(BodyIndex);
		checkf(Body, TEXT("generic Robot support query has no body instance"));
		FTransform BodyWorld = Body->GetUnrealWorldTransform_AssumesLocked(true, true);
		if (BodyIndex == Topology.RootBodyIndex)
		{
			BodyWorld = RecoverGenericRobotRootWorld(RootCanonicalSlotTransform, BodyWorld);
		}
		const FPhysicsActorHandle ActorHandle = Body->GetPhysicsActorHandle();
		const FBox BodyBounds = ActorHandle
			? FPhysicsInterface::GetBounds_AssumesLocked(ActorHandle)
			: FBox(EForceInit::ForceInitToZero);
		OutSupportInputs.Add({ BodyIndex, BodyWorld, BodyBounds });
	}

	if (!ContactForceBodyIndices.IsEmpty())
	{
		const int32 NumBodies = Topology.BodyNames.Num();
		TArray<Chaos::FGeometryParticleHandle*, TInlineAllocator<32>> RobotParticles;
		TArray<Chaos::FPBDRigidParticleHandle*, TInlineAllocator<32>> BodyParticles;
		RobotParticles.Reserve(NumBodies);
		BodyParticles.SetNumZeroed(NumBodies);
		for (int32 BodyIndex = 0; BodyIndex < NumBodies; ++BodyIndex)
		{
			FBodyInstance* Body = ResolveBody(BodyIndex);
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

		for (const int32 BodyIndex : ContactForceBodyIndices)
		{
			if (!BodyParticles.IsValidIndex(BodyIndex))
			{
				continue;
			}
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
						|| !IsGenericRobotContactForceStateAccepted(CollisionScope, Other->ObjectState()))
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
	}
}

void ResolveGenericRobotSlotSupportContacts(
	USkeletalMeshComponent& Component,
	AActor& Owner,
	const FUERLSlotCollisionProfile& CollisionProfile,
	const FQuat& GroundRotation,
	TConstArrayView<FGenericRobotSupportProbeInput> SupportInputs,
	FGenericRobotContactState& ContactState,
	TArray<FHitResult>& ScratchHits)
{
	if (SupportInputs.IsEmpty())
	{
		return;
	}
	UWorld* World = Component.GetWorld();
	checkf(World, TEXT("generic Robot support query has no World"));
	const FVector GroundUp = GroundRotation.RotateVector(FVector::UpVector).GetSafeNormal();
	const bool bSharedWorld = CollisionProfile.Scope() == EUERLEnvironmentCollisionScope::SharedWorld;
	const ECollisionChannel TraceChannel = bSharedWorld
		? ECC_WorldStatic
		: CollisionProfile.Channel();
	static const FCollisionQueryParams BaseQueryParams(SCENE_QUERY_STAT(UERLRobotFootSupport), false);
	FCollisionQueryParams QueryParams(BaseQueryParams);
	QueryParams.AddIgnoredActor(&Owner);

	for (const FGenericRobotSupportProbeInput& Probe : SupportInputs)
	{
		const FVector BodyLocation = Probe.BodyWorld.GetLocation();
		const FVector BoundsCenter = Probe.BodyBounds.GetCenter();
		const FVector BoundsExtent = Probe.BodyBounds.GetExtent();
		const float LowestBoundsProjection = static_cast<float>(
			FVector::DotProduct(BoundsCenter, GroundUp)
			- FMath::Abs(GroundUp.X) * BoundsExtent.X
			- FMath::Abs(GroundUp.Y) * BoundsExtent.Y
			- FMath::Abs(GroundUp.Z) * BoundsExtent.Z);
		const float BodyProjection = static_cast<float>(FVector::DotProduct(BodyLocation, GroundUp));
		const FVector BottomProbe = BodyLocation + GroundUp * (LowestBoundsProjection - BodyProjection);
		const FVector Start = BottomProbe + GroundUp * 4.0;
		const FVector End = BottomProbe - GroundUp * 3.0;
		if (TestFootSupportRay(
			*World, Start, End, GroundUp, bSharedWorld, TraceChannel, QueryParams, ScratchHits))
		{
			ContactState.MarkBodySupported(Probe.BodyIndex);
		}
	}
}

void SampleGenericRobotSlotPhysicsContacts(
	USkeletalMeshComponent& Component,
	AActor& Owner,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FBodyInstance*> BodyInstances,
	const FUERLSlotCollisionProfile& CollisionProfile,
	const FQuat& GroundRotation,
	const FTransform& RootCanonicalSlotTransform,
	TConstArrayView<int32> SupportBodyIndices,
	TConstArrayView<int32> ContactForceBodyIndices,
	FGenericRobotContactState& ContactState,
	EUERLEnvironmentCollisionScope CollisionScope,
	double SolverStepSeconds,
	TArray<FHitResult>& ScratchHits)
{
	if (SupportBodyIndices.IsEmpty() && ContactForceBodyIndices.IsEmpty())
	{
		return;
	}
	TRACE_CPUPROFILER_EVENT_SCOPE(UERL_GenericRobot_SlotPhysicsContacts);

	TArray<FGenericRobotSupportProbeInput, TInlineAllocator<16>> SupportInputs;
	FPhysicsCommand::ExecuteRead(&Component, [&]()
	{
		ReadGenericRobotSlotPhysicsContacts_AssumesLocked(
			Component,
			Topology,
			BodyInstances,
			RootCanonicalSlotTransform,
			SupportBodyIndices,
			ContactForceBodyIndices,
			ContactState,
			CollisionScope,
			SolverStepSeconds,
			SupportInputs);
	});

	ResolveGenericRobotSlotSupportContacts(
		Component,
		Owner,
		CollisionProfile,
		GroundRotation,
		SupportInputs,
		ContactState,
		ScratchHits);
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
