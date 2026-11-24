#include "UERLGenericRobotObservationSampler.h"

#include "UERLBatchBinding.h"
#include "UERLGenericRobotContactListener.h"
#include "UERLGenericRobotKinematics.h"
#include "UERLGroundQuery.h"
#include "UERLInterfaceTypes.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

namespace
{
	FORCEINLINE FBodyInstance* ResolveObservationBodyInstance(
		const FUERLRobotObservationSlotView& Slot,
		const FUERLRobotTopology& Topology,
		int32 BodyIndex)
	{
		if (Slot.BodyInstances && Slot.BodyInstances->IsValidIndex(BodyIndex) && (*Slot.BodyInstances)[BodyIndex])
		{
			return (*Slot.BodyInstances)[BodyIndex];
		}
		return Topology.BodyNames.IsValidIndex(BodyIndex)
			? Slot.Component->GetBodyInstance(Topology.BodyNames[BodyIndex])
			: nullptr;
	}

	FORCEINLINE FConstraintInstance* ResolveObservationConstraintInstance(
		const FUERLRobotObservationSlotView& Slot,
		int32 JointIndex)
	{
		if (Slot.Constraints && Slot.Constraints->IsValidIndex(JointIndex) && (*Slot.Constraints)[JointIndex])
		{
			return (*Slot.Constraints)[JointIndex];
		}
		return (Slot.ConstraintIndices && Slot.ConstraintIndices->IsValidIndex(JointIndex))
			? Slot.Component->GetConstraintInstanceByIndex((*Slot.ConstraintIndices)[JointIndex])
			: nullptr;
	}
}

FTransform ObserveGenericRobotBodyWorldTransform(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	int32 BodyIndex)
{
	checkf(Slot.Component, TEXT("generic Robot ground query has no component"));
	checkf(Topology.BodyNames.IsValidIndex(BodyIndex), TEXT("generic Robot ground query has invalid body index"));
	FBodyInstance* BodyInstance = ResolveObservationBodyInstance(Slot, Topology, BodyIndex);
	checkf(BodyInstance, TEXT("generic Robot ground query has no body instance"));
	FTransform WorldTransform = BodyInstance->GetUnrealWorldTransform();
	if (BodyIndex == Topology.RootBodyIndex)
	{
		WorldTransform = RecoverGenericRobotRootWorld(Slot.RootCanonicalSlotTransform, WorldTransform);
	}
	return WorldTransform;
}

bool ReadGenericRobotJointScalar(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	const FUERLRobotObservationPlanEntry& Entry,
	bool bVelocity,
	float& OutValue)
{
	if (!Slot.Component || !Slot.ConstraintIndices
		|| !Slot.ConstraintIndices->IsValidIndex(Entry.JointIndex)
		|| !Topology.Joints.IsValidIndex(Entry.JointIndex))
	{
		return false;
	}
	FConstraintInstance* Constraint = ResolveObservationConstraintInstance(Slot, Entry.JointIndex);
	const FUERLJointTopology& Joint = Topology.Joints[Entry.JointIndex];
	if (!Constraint || !Topology.BodyNames.IsValidIndex(Joint.ChildBodyIndex)
		|| !Topology.BodyNames.IsValidIndex(Joint.ParentBodyIndex))
	{
		return false;
	}

	const EUERLJointCoordinate Coordinate = Joint.Coordinate;
	const bool bAngular = IsGenericAngularCoordinate(Coordinate);

	FBodyInstance* ChildInstance = ResolveObservationBodyInstance(Slot, Topology, Joint.ChildBodyIndex);
	FBodyInstance* ParentInstance = ResolveObservationBodyInstance(Slot, Topology, Joint.ParentBodyIndex);
	if (!ChildInstance || !ParentInstance)
	{
		return false;
	}
	const FTransform ChildFrame = ComposeGenericConstraintFrameWorld(
		Constraint->GetRefFrame(EConstraintFrame::Frame1),
		ChildInstance->GetUnrealWorldTransform());
	const FVector AxisWorld = ChildFrame.TransformVectorNoScale(GenericJointCoordinateAxis(Coordinate)).GetSafeNormal();
	if (AxisWorld.IsNearlyZero())
	{
		return false;
	}
	if (bVelocity)
	{
		if (bAngular)
		{
			const FVector RelativeAngularVelocity = ChildInstance->GetUnrealWorldAngularVelocityInRadians()
				- ParentInstance->GetUnrealWorldAngularVelocityInRadians();
			OutValue = static_cast<float>(FVector::DotProduct(RelativeAngularVelocity, AxisWorld));
		}
		else
		{
			const FTransform ParentFrame = ComposeGenericConstraintFrameWorld(
				Constraint->GetRefFrame(EConstraintFrame::Frame2),
				ParentInstance->GetUnrealWorldTransform());
			const FVector RelativeLinearVelocity = ChildInstance->GetUnrealWorldVelocityAtPoint(
				ChildFrame.GetLocation())
				- ParentInstance->GetUnrealWorldVelocityAtPoint(ParentFrame.GetLocation());
			OutValue = static_cast<float>(FVector::DotProduct(RelativeLinearVelocity, AxisWorld) / 100.0);
		}
		return true;
	}
	const FTransform ParentFrame = ComposeGenericConstraintFrameWorld(
		Constraint->GetRefFrame(EConstraintFrame::Frame2),
		ParentInstance->GetUnrealWorldTransform());
	OutValue = static_cast<float>(MeasureGenericJointPosition(
		ChildFrame, ParentFrame, Coordinate));
	return true;
}

namespace
{
	/**
	 * Deployment accepts any WorldStatic hit, so its probes start this far above
	 * the root or body instead of above everything: high enough for the <=0.12 m
	 * terrain relief seen in training, low enough to stay under indoor ceilings.
	 */
	constexpr double DeploymentProbeStartHeightCm = 100.0;

	FUERLGroundQueryContext GroundQueryContext(
		const FUERLRobotObservationSlotView& Slot,
		TConstArrayView<AActor*> ResolvedPermittedActors,
		TConstArrayView<UPrimitiveComponent*> PermittedComponents)
	{
		return { Slot.Owner, Slot.CollisionProfile, Slot.TerrainQueryActors,
			Slot.TerrainQueryPurpose, Slot.TerrainTraceHits,
			!ResolvedPermittedActors.IsEmpty() ? ResolvedPermittedActors : Slot.ResolvedTerrainQueryActors,
			PermittedComponents };
	}

	bool MeasureGroundClearanceWithTransform(
		const FUERLRobotObservationSlotView& Slot,
		const FTransform& BodyTransform,
		TConstArrayView<AActor*> ResolvedPermittedActors,
		TConstArrayView<UPrimitiveComponent*> PermittedComponents,
		const FUERLPreparedGroundTargets* PreparedTargets,
		int32 BodyIndex,
		float& OutValue,
		FString& OutError)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UERL_GenericRobot_GroundClearanceQuery);
		if (!Slot.Component || !Slot.Owner)
		{
			OutError = TEXT("generic Robot ground-clearance query has no component or owner");
			return false;
		}
		UWorld* World = Slot.Component->GetWorld();
		if (!World)
		{
			OutError = TEXT("generic Robot ground-clearance query has no World");
			return false;
		}
		if (!Slot.GroundClearanceWorldHits || !Slot.GroundClearanceHitValid
			|| !Slot.GroundClearanceHitValid->IsValidIndex(BodyIndex)
			|| !Slot.GroundClearanceWorldHits->IsValidIndex(BodyIndex))
		{
			OutError = TEXT("generic Robot ground-clearance cache is missing");
			return false;
		}
		const FVector BodyLocation = BodyTransform.GetLocation();
		const double StartHeightCm = Slot.TerrainQueryPurpose == EUERLTerrainQueryPurpose::DeploymentWorldStatic
			? DeploymentProbeStartHeightCm : 200.0;
		const FVector Start = BodyLocation + FVector::UpVector * StartHeightCm;
		const FVector End = BodyLocation - FVector::UpVector * 200.0;
		const FUERLGroundQueryContext QueryCtx = GroundQueryContext(
			Slot, ResolvedPermittedActors, PermittedComponents);
		FVector HitImpactPoint = FVector::ZeroVector;
		bool bHit = false;
		if (PreparedTargets)
		{
			bHit = QueryUERLGroundHit_PreparedLocked(
				QueryCtx, *PreparedTargets, Start, End, TEXT("ground-clearance"), HitImpactPoint, OutError);
		}
		else
		{
			FHitResult Hit;
			bHit = QueryUERLGroundHit(
				QueryCtx, *World, Start, End, TEXT("ground-clearance"), Hit, OutError);
			if (bHit)
			{
				HitImpactPoint = Hit.ImpactPoint;
			}
		}
		if (bHit)
		{
			(*Slot.GroundClearanceWorldHits)[BodyIndex] = HitImpactPoint;
			(*Slot.GroundClearanceHitValid)[BodyIndex] = 1;
		}
		else if ((*Slot.GroundClearanceHitValid)[BodyIndex])
		{
			HitImpactPoint = (*Slot.GroundClearanceWorldHits)[BodyIndex];
		}
		else
		{
			OutError = FString::Printf(
				TEXT("generic Robot ground-clearance query has no initial hit for body %d: %s"),
				BodyIndex, *OutError);
			return false;
		}
		OutValue = static_cast<float>((BodyLocation.Z - HitImpactPoint.Z) / 100.0);
		OutError.Reset();
		return true;
	}

	bool MeasureTerrainHeightScanWithTransform(
		const FUERLRobotObservationSlotView& Slot,
		const FUERLRobotTopology& Topology,
		const FTransform& BodyTransform,
		TConstArrayView<AActor*> ResolvedPermittedActors,
		TConstArrayView<UPrimitiveComponent*> PermittedComponents,
		const FUERLPreparedGroundTargets* PreparedTargets,
		int32 BodyIndex,
		TArray<float>& OutValues,
		FString& OutError)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UERL_GenericRobot_TerrainHeightQuery);
		if (!Slot.Component || !Slot.Owner)
		{
			OutError = TEXT("generic Robot terrain scan has no component or owner");
			return false;
		}
		UWorld* World = Slot.Component->GetWorld();
		if (!World)
		{
			OutError = TEXT("generic Robot terrain scan has no World");
			return false;
		}
		if (BodyIndex != Topology.RootBodyIndex)
		{
			OutError = TEXT("generic Robot terrain scan must be bound to the root body");
			return false;
		}
		if (!Slot.TerrainHeightWorldHits || !Slot.TerrainHeightHitValid
			|| Slot.TerrainHeightWorldHits->Num() != UERLTerrainHeightScanWidth
			|| Slot.TerrainHeightHitValid->Num() != UERLTerrainHeightScanWidth)
		{
			OutError = TEXT("generic Robot terrain-height cache is missing");
			return false;
		}

		const FVector WorldUp = FVector::UpVector;
		FVector Forward = BodyTransform.GetRotation().GetForwardVector();
		Forward = (Forward - WorldUp * FVector::DotProduct(Forward, WorldUp)).GetSafeNormal();
		if (Forward.IsNearlyZero())
		{
			Forward = FVector::ForwardVector;
		}
		const FVector Right = FVector::CrossProduct(WorldUp, Forward).GetSafeNormal();
		const FVector RootLocation = BodyTransform.GetLocation();
		const double StartHeightCm = Slot.TerrainQueryPurpose == EUERLTerrainQueryPurpose::DeploymentWorldStatic
			? DeploymentProbeStartHeightCm : 10000.0;
		const FUERLGroundQueryContext QueryCtx = GroundQueryContext(
			Slot, ResolvedPermittedActors, PermittedComponents);
		constexpr double ForwardStartM = -1.0;
		constexpr double ForwardStepM = 0.5;
		constexpr double LateralStartM = -0.6;
		constexpr double LateralStepM = 0.3;
		OutValues.SetNumUninitialized(UERLTerrainHeightScanWidth);
		FHitResult Hit;
		FString QueryError;
		for (int32 ForwardIndex = 0; ForwardIndex < UERLTerrainHeightScanForwardCount; ++ForwardIndex)
		{
			for (int32 LateralIndex = 0; LateralIndex < UERLTerrainHeightScanLateralCount; ++LateralIndex)
			{
				const int32 Index = ForwardIndex * UERLTerrainHeightScanLateralCount + LateralIndex;
				const FVector2D Offset(
					ForwardStartM + static_cast<double>(ForwardIndex) * ForwardStepM,
					LateralStartM + static_cast<double>(LateralIndex) * LateralStepM);
				const FVector Probe = RootLocation + (Forward * Offset.X + Right * Offset.Y) * 100.0;
				const FVector Start = Probe + WorldUp * StartHeightCm;
				const FVector End = Probe - WorldUp * 10000.0;
				FVector HitImpactPoint = FVector::ZeroVector;
				const bool bHit = PreparedTargets
					? QueryUERLGroundHit_PreparedLocked(
						QueryCtx, *PreparedTargets, Start, End, TEXT("terrain-height"), HitImpactPoint, QueryError)
					: (QueryUERLGroundHit(
						QueryCtx, *World, Start, End, TEXT("terrain-height"), Hit, QueryError)
						&& ((HitImpactPoint = Hit.ImpactPoint), true));
				if (bHit)
				{
					(*Slot.TerrainHeightWorldHits)[Index] = HitImpactPoint;
					(*Slot.TerrainHeightHitValid)[Index] = 1;
				}
				else if ((*Slot.TerrainHeightHitValid)[Index])
				{
					HitImpactPoint = (*Slot.TerrainHeightWorldHits)[Index];
				}
				else
				{
					OutError = FString::Printf(
						TEXT("generic Robot terrain-height query has no initial hit at scan index %d: %s"),
						Index, *QueryError);
					return false;
				}
				OutValues[Index] = static_cast<float>((HitImpactPoint.Z - RootLocation.Z) / 100.0);
			}
		}
		OutError.Reset();
		return true;
	}

	struct FPreparedGroundCache
	{
		bool bValid = false;
		bool bSharedWorld = false;
		ECollisionChannel TargetChannel = ECC_WorldStatic;
		TArray<AActor*, TInlineAllocator<8>> Actors;
		TArray<UPrimitiveComponent*, TInlineAllocator<8>> Components;
		FUERLPreparedGroundTargets PreparedTargets;

		const FUERLPreparedGroundTargets* GetOrPrepare_AssumesLocked(
			const FUERLRobotObservationSlotView& Slot,
			TConstArrayView<AActor*> PermittedActorsView)
		{
			const bool bSlotShared = Slot.CollisionProfile.Scope() == EUERLEnvironmentCollisionScope::SharedWorld;
			const ECollisionChannel SlotChannel = bSlotShared
				? ECC_WorldStatic : Slot.CollisionProfile.Channel();
			bool bMatches = bValid && bSharedWorld == bSlotShared && TargetChannel == SlotChannel
				&& Actors.Num() == PermittedActorsView.Num();
			if (bMatches)
			{
				for (int32 I = 0; I < Actors.Num(); ++I)
				{
					if (Actors[I] != PermittedActorsView[I])
					{
						bMatches = false;
						break;
					}
				}
			}
			if (!bMatches)
			{
				bValid = true;
				bSharedWorld = bSlotShared;
				TargetChannel = SlotChannel;
				Actors.Reset(PermittedActorsView.Num());
				Actors.Append(PermittedActorsView.GetData(), PermittedActorsView.Num());
				ResolveUERLPermittedGroundComponents(
					GroundQueryContext(Slot, PermittedActorsView, {}), Components);
				PrepareUERLPermittedGroundTargets_AssumesLocked(Components, PreparedTargets);
			}
			return &PreparedTargets;
		}
	};

	FORCEINLINE bool SlotUsesPreparedGroundTraces(const FUERLRobotObservationSlotView& Slot)
	{
		return Slot.TerrainQueryPurpose != EUERLTerrainQueryPurpose::DeploymentWorldStatic
			&& (Slot.CollisionProfile.Scope() == EUERLEnvironmentCollisionScope::SharedWorld
				|| Slot.TerrainQueryPurpose == EUERLTerrainQueryPurpose::ExplicitBinding);
	}
}

bool MeasureGenericRobotGroundClearance(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	int32 BodyIndex,
	float& OutValue,
	FString& OutError)
{
	if (!Slot.Component || !Slot.Owner)
	{
		OutError = TEXT("generic Robot ground-clearance query has no component or owner");
		return false;
	}
	const FTransform BodyTransform = ObserveGenericRobotBodyWorldTransform(Slot, Topology, BodyIndex);
	return MeasureGroundClearanceWithTransform(Slot, BodyTransform, {}, {}, nullptr, BodyIndex, OutValue, OutError);
}

bool MeasureGenericRobotTerrainHeightScan(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	int32 BodyIndex,
	TArray<float>& OutValues,
	FString& OutError)
{
	if (!Slot.Component || !Slot.Owner)
	{
		OutError = TEXT("generic Robot terrain scan has no component or owner");
		return false;
	}
	if (BodyIndex != Topology.RootBodyIndex)
	{
		OutError = TEXT("generic Robot terrain scan must be bound to the root body");
		return false;
	}
	const FTransform BodyTransform = ObserveGenericRobotBodyWorldTransform(Slot, Topology, BodyIndex);
	return MeasureTerrainHeightScanWithTransform(
		Slot, Topology, BodyTransform, {}, {}, nullptr, BodyIndex, OutValues, OutError);
}

namespace
{
	struct FObservationPlanMask
	{
		TArray<uint8, TInlineAllocator<32>> NeedTransform;
		TArray<uint8, TInlineAllocator<32>> NeedLinVel;
		TArray<uint8, TInlineAllocator<32>> NeedAngVel;
		bool bAnyPhysicsRead = false;
		bool bHasPrismaticVel = false;
		bool bNeedsGroundTrace = false;
	};

	FObservationPlanMask BuildObservationPlanMask(
		const FUERLRobotTopology& Topology,
		const TArray<int32>* ConstraintIndices,
		TConstArrayView<FUERLRobotObservationPlanEntry> Plan)
	{
		FObservationPlanMask Mask;
		const int32 NumBodies = Topology.BodyNames.Num();
		Mask.NeedTransform.SetNumZeroed(NumBodies);
		Mask.NeedLinVel.SetNumZeroed(NumBodies);
		Mask.NeedAngVel.SetNumZeroed(NumBodies);
		for (const FUERLRobotObservationPlanEntry& Entry : Plan)
		{
			checkf(
				Topology.BodyNames.IsValidIndex(Entry.BodyIndex),
				TEXT("generic Robot State field '%s' has invalid body index %d"),
				*Entry.FieldName.ToString(), Entry.BodyIndex);
			switch (Entry.Type)
			{
			case EUERLObservationType::GroundClearance:
			case EUERLObservationType::TerrainHeight:
				Mask.bNeedsGroundTrace = true;
				Mask.NeedTransform[Entry.BodyIndex] = 1;
				Mask.bAnyPhysicsRead = true;
				break;
			case EUERLObservationType::BodyPose:
				Mask.NeedTransform[Entry.BodyIndex] = 1;
				Mask.bAnyPhysicsRead = true;
				break;
			case EUERLObservationType::BodyLinearVelocity:
				Mask.NeedLinVel[Entry.BodyIndex] = 1;
				Mask.bAnyPhysicsRead = true;
				break;
			case EUERLObservationType::BodyAngularVelocity:
				Mask.NeedAngVel[Entry.BodyIndex] = 1;
				Mask.bAnyPhysicsRead = true;
				break;
			case EUERLObservationType::JointPosition:
			case EUERLObservationType::JointVelocity:
			{
				checkf(
					(!ConstraintIndices || ConstraintIndices->IsValidIndex(Entry.JointIndex))
					&& Topology.Joints.IsValidIndex(Entry.JointIndex),
					TEXT("generic Robot failed to read joint State field '%s'"), *Entry.FieldName.ToString());
				const FUERLJointTopology& Joint = Topology.Joints[Entry.JointIndex];
				checkf(
					Topology.BodyNames.IsValidIndex(Joint.ChildBodyIndex)
					&& Topology.BodyNames.IsValidIndex(Joint.ParentBodyIndex),
					TEXT("generic Robot failed to read joint State field '%s'"), *Entry.FieldName.ToString());
				Mask.NeedTransform[Joint.ChildBodyIndex] = 1;
				if (Entry.Type == EUERLObservationType::JointPosition)
				{
					Mask.NeedTransform[Joint.ParentBodyIndex] = 1;
				}
				else if (IsGenericAngularCoordinate(Joint.Coordinate))
				{
					Mask.NeedAngVel[Joint.ChildBodyIndex] = 1;
					Mask.NeedAngVel[Joint.ParentBodyIndex] = 1;
				}
				else
				{
					Mask.NeedTransform[Joint.ParentBodyIndex] = 1;
					Mask.bHasPrismaticVel = true;
				}
				Mask.bAnyPhysicsRead = true;
				break;
			}
			default:
				break;
			}
		}
		return Mask;
	}

	bool WriteGenericRobotObservationFieldsImpl(
		int32 SlotId,
		const FUERLRobotObservationSlotView& Slot,
		const FUERLRobotTopology& Topology,
		TConstArrayView<FUERLRobotObservationPlanEntry> Plan,
		const FObservationPlanMask& Mask,
		FUERLNamedStateWriter& Writer,
		FString& OutError,
		TConstArrayView<const FUERLBatchFieldBinding*> PlanBindings,
		bool bLockAlreadyHeld,
		FPreparedGroundCache* SharedGroundCache)
	{
		checkf(Slot.Component, TEXT("generic Robot cannot collect State without a component"));
		OutError.Reset();

		const int32 NumBodies = Topology.BodyNames.Num();
		TArray<FTransform, TInlineAllocator<32>> RawBodyTransforms;
		TArray<FVector, TInlineAllocator<32>> BodyLinVels;
		TArray<FVector, TInlineAllocator<32>> BodyAngVels;
		TArray<float, TInlineAllocator<8>> PrismaticVelocities;
		TArray<uint8, TInlineAllocator<8>> PrismaticValid;
		RawBodyTransforms.SetNum(NumBodies);
		BodyLinVels.SetNumZeroed(NumBodies);
		BodyAngVels.SetNumZeroed(NumBodies);
		if (Mask.bHasPrismaticVel)
		{
			PrismaticVelocities.SetNumZeroed(Plan.Num());
			PrismaticValid.SetNumZeroed(Plan.Num());
		}

		TArray<AActor*, TInlineAllocator<8>> LocalPermittedActors;
		TConstArrayView<AActor*> PermittedActorsView;
		if (Mask.bNeedsGroundTrace)
		{
			PermittedActorsView = Slot.ResolvedTerrainQueryActors;
			if (PermittedActorsView.IsEmpty() && Slot.TerrainQueryActors && !Slot.TerrainQueryActors->IsEmpty())
			{
				LocalPermittedActors.Reserve(Slot.TerrainQueryActors->Num());
				for (const TWeakObjectPtr<AActor>& WeakActor : *Slot.TerrainQueryActors)
				{
					if (AActor* RawActor = WeakActor.Get())
					{
						LocalPermittedActors.Add(RawActor);
					}
				}
				PermittedActorsView = LocalPermittedActors;
			}
		}

		const bool bCanTraceInsideLock = Mask.bNeedsGroundTrace && SlotUsesPreparedGroundTraces(Slot)
			&& (!PermittedActorsView.IsEmpty());
		bool bGroundedInsideLock = false;
		float PrecomputedGroundClearance = 0.0f;
		bool bHasPrecomputedClearance = false;
		bool bHasPrecomputedTerrainScan = false;
		FString LockedGroundError;
		bool bLockedGroundFailed = false;

		auto ReadPhysicsAndPreparedGround = [&]()
		{
			for (int32 BodyIndex = 0; BodyIndex < NumBodies; ++BodyIndex)
			{
				const bool bWantT = Mask.NeedTransform[BodyIndex] != 0;
				const bool bWantL = Mask.NeedLinVel[BodyIndex] != 0;
				const bool bWantA = Mask.NeedAngVel[BodyIndex] != 0;
				if (!bWantT && !bWantL && !bWantA)
				{
					continue;
				}
				FBodyInstance* Body = ResolveObservationBodyInstance(Slot, Topology, BodyIndex);
				if (bWantT)
				{
					checkf(Body, TEXT("generic Robot body '%s' has no body instance"), *Topology.BodyNames[BodyIndex].ToString());
					RawBodyTransforms[BodyIndex] = Body->GetUnrealWorldTransform_AssumesLocked(true, true);
				}
				if (bWantL && Body)
				{
					BodyLinVels[BodyIndex] = Body->GetUnrealWorldVelocity_AssumesLocked();
				}
				if (bWantA && Body)
				{
					BodyAngVels[BodyIndex] = Body->GetUnrealWorldAngularVelocityInRadians_AssumesLocked();
				}
			}

			if (Mask.bHasPrismaticVel)
			{
				for (int32 PlanIndex = 0; PlanIndex < Plan.Num(); ++PlanIndex)
				{
					const FUERLRobotObservationPlanEntry& Entry = Plan[PlanIndex];
					if (Entry.Type != EUERLObservationType::JointVelocity)
					{
						continue;
					}
					const FUERLJointTopology& Joint = Topology.Joints[Entry.JointIndex];
					if (IsGenericAngularCoordinate(Joint.Coordinate))
					{
						continue;
					}
					FConstraintInstance* Constraint = ResolveObservationConstraintInstance(Slot, Entry.JointIndex);
					FBodyInstance* ChildInstance = ResolveObservationBodyInstance(Slot, Topology, Joint.ChildBodyIndex);
					FBodyInstance* ParentInstance = ResolveObservationBodyInstance(Slot, Topology, Joint.ParentBodyIndex);
					if (!Constraint || !ChildInstance || !ParentInstance)
					{
						continue;
					}
					const FTransform ChildFrame = ComposeGenericConstraintFrameWorld(
						Constraint->GetRefFrame(EConstraintFrame::Frame1),
						RawBodyTransforms[Joint.ChildBodyIndex]);
					const FVector AxisWorld = ChildFrame.TransformVectorNoScale(
						GenericJointCoordinateAxis(Joint.Coordinate)).GetSafeNormal();
					if (AxisWorld.IsNearlyZero())
					{
						continue;
					}
					const FTransform ParentFrame = ComposeGenericConstraintFrameWorld(
						Constraint->GetRefFrame(EConstraintFrame::Frame2),
						RawBodyTransforms[Joint.ParentBodyIndex]);
					const FVector RelativeLinearVelocity =
						ChildInstance->GetUnrealWorldVelocityAtPoint_AssumesLocked(ChildFrame.GetLocation())
						- ParentInstance->GetUnrealWorldVelocityAtPoint_AssumesLocked(ParentFrame.GetLocation());
					PrismaticVelocities[PlanIndex] = static_cast<float>(
						FVector::DotProduct(RelativeLinearVelocity, AxisWorld) / 100.0);
					PrismaticValid[PlanIndex] = 1;
				}
			}

			if (bCanTraceInsideLock)
			{
				FPreparedGroundCache LocalCache;
				FPreparedGroundCache& Cache = SharedGroundCache ? *SharedGroundCache : LocalCache;
				const FUERLPreparedGroundTargets* PreparedTargets =
					Cache.GetOrPrepare_AssumesLocked(Slot, PermittedActorsView);
				const FTransform RootWorld = Topology.BodyNames.IsValidIndex(Topology.RootBodyIndex)
					&& Mask.NeedTransform[Topology.RootBodyIndex]
					? RecoverGenericRobotRootWorld(
						Slot.RootCanonicalSlotTransform,
						RawBodyTransforms[Topology.RootBodyIndex])
					: FTransform::Identity;
				for (const FUERLRobotObservationPlanEntry& Entry : Plan)
				{
					if (Entry.Type == EUERLObservationType::GroundClearance && !bHasPrecomputedClearance)
					{
						const FTransform& BodyTransform = (Entry.BodyIndex == Topology.RootBodyIndex)
							? RootWorld : RawBodyTransforms[Entry.BodyIndex];
						if (!MeasureGroundClearanceWithTransform(
							Slot, BodyTransform, PermittedActorsView, Cache.Components, PreparedTargets,
							Entry.BodyIndex, PrecomputedGroundClearance, LockedGroundError))
						{
							bLockedGroundFailed = true;
							return;
						}
						bHasPrecomputedClearance = true;
					}
					else if (Entry.Type == EUERLObservationType::TerrainHeight && !bHasPrecomputedTerrainScan)
					{
						checkf(Slot.TerrainHeightValues, TEXT("generic Robot terrain height buffer is missing"));
						if (!MeasureTerrainHeightScanWithTransform(
							Slot, Topology, RootWorld, PermittedActorsView, Cache.Components, PreparedTargets,
							Entry.BodyIndex, *Slot.TerrainHeightValues, LockedGroundError))
						{
							bLockedGroundFailed = true;
							return;
						}
						bHasPrecomputedTerrainScan = true;
					}
				}
				bGroundedInsideLock = true;
			}
		};

		if (Mask.bAnyPhysicsRead)
		{
			if (bLockAlreadyHeld)
			{
				ReadPhysicsAndPreparedGround();
			}
			else
			{
				FPhysicsCommand::ExecuteRead(Slot.Component, ReadPhysicsAndPreparedGround);
			}
		}

		if (bLockedGroundFailed)
		{
			OutError = MoveTemp(LockedGroundError);
			return false;
		}

		FTransform ObservedRootTransform = FTransform::Identity;
		if (Topology.BodyNames.IsValidIndex(Topology.RootBodyIndex) && Mask.NeedTransform[Topology.RootBodyIndex])
		{
			ObservedRootTransform = RecoverGenericRobotRootWorld(
				Slot.RootCanonicalSlotTransform,
				RawBodyTransforms[Topology.RootBodyIndex]);
			if (Slot.OutRootTransformChecked && Slot.OutRootTransformValid)
			{
				*Slot.OutRootTransformValid =
					!RawBodyTransforms[Topology.RootBodyIndex].ContainsNaN() && !ObservedRootTransform.ContainsNaN();
				*Slot.OutRootTransformChecked = true;
			}
		}

		TArray<UPrimitiveComponent*, TInlineAllocator<8>> FallbackPermittedComponents;
		if (Mask.bNeedsGroundTrace && !bGroundedInsideLock)
		{
			ResolveUERLPermittedGroundComponents(
				GroundQueryContext(Slot, PermittedActorsView, {}), FallbackPermittedComponents);
		}

		const FTransform ObservationFrame(Slot.ObservationRotation, Slot.Origin);
		const FQuat InvObservationRotation = Slot.ObservationRotation.Inverse();

		for (int32 PlanIndex = 0; PlanIndex < Plan.Num(); ++PlanIndex)
		{
			const FUERLRobotObservationPlanEntry& Entry = Plan[PlanIndex];
			const FUERLBatchFieldBinding* Binding = PlanBindings.IsValidIndex(PlanIndex)
				? PlanBindings[PlanIndex] : nullptr;
			auto WriteScalarValue = [&](float Value) -> bool
			{
				return Binding
					? Writer.WriteScalar(SlotId, *Binding, Value)
					: Writer.WriteScalar(SlotId, Entry.FieldName, Value);
			};
			auto WriteVectorValue = [&](TConstArrayView<float> Values) -> bool
			{
				return Binding
					? Writer.WriteVector(SlotId, *Binding, Values)
					: Writer.WriteVector(SlotId, Entry.FieldName, Values);
			};

			switch (Entry.Type)
			{
			case EUERLObservationType::BodyPose:
				{
					const FTransform& WorldTransform = (Entry.BodyIndex == Topology.RootBodyIndex)
						? ObservedRootTransform : RawBodyTransforms[Entry.BodyIndex];
					const FTransform LocalTransform = WorldTransform.GetRelativeTransform(ObservationFrame);
					const FVector Position = LocalTransform.GetLocation() / 100.0;
					const FQuat Rotation = LocalTransform.GetRotation().GetNormalized();
					const float Values[] = {
						static_cast<float>(Position.X), static_cast<float>(Position.Y), static_cast<float>(Position.Z),
						static_cast<float>(Rotation.X), static_cast<float>(Rotation.Y),
						static_cast<float>(Rotation.Z), static_cast<float>(Rotation.W),
					};
					checkf(
						WriteVectorValue(TConstArrayView<float>(Values, UE_ARRAY_COUNT(Values))),
						TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
				}
				break;
			case EUERLObservationType::BodyLinearVelocity:
				{
					const FVector Velocity = InvObservationRotation.RotateVector(BodyLinVels[Entry.BodyIndex]) / 100.0;
					const float Values[] = {
						static_cast<float>(Velocity.X), static_cast<float>(Velocity.Y), static_cast<float>(Velocity.Z) };
					checkf(
						WriteVectorValue(TConstArrayView<float>(Values, UE_ARRAY_COUNT(Values))),
						TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
				}
				break;
			case EUERLObservationType::BodyAngularVelocity:
				{
					const FVector Velocity = InvObservationRotation.RotateVector(BodyAngVels[Entry.BodyIndex]);
					const float Values[] = {
						static_cast<float>(Velocity.X), static_cast<float>(Velocity.Y), static_cast<float>(Velocity.Z) };
					checkf(
						WriteVectorValue(TConstArrayView<float>(Values, UE_ARRAY_COUNT(Values))),
						TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
				}
				break;
			case EUERLObservationType::JointPosition:
				{
					FConstraintInstance* Constraint = ResolveObservationConstraintInstance(Slot, Entry.JointIndex);
					const FUERLJointTopology& Joint = Topology.Joints[Entry.JointIndex];
					checkf(Constraint, TEXT("generic Robot failed to read joint State field '%s'"), *Entry.FieldName.ToString());
					const FTransform ChildFrame = ComposeGenericConstraintFrameWorld(
						Constraint->GetRefFrame(EConstraintFrame::Frame1),
						RawBodyTransforms[Joint.ChildBodyIndex]);
					const FVector AxisWorld = ChildFrame.TransformVectorNoScale(
						GenericJointCoordinateAxis(Joint.Coordinate)).GetSafeNormal();
					checkf(!AxisWorld.IsNearlyZero(), TEXT("generic Robot failed to read joint State field '%s'"), *Entry.FieldName.ToString());
					const FTransform ParentFrame = ComposeGenericConstraintFrameWorld(
						Constraint->GetRefFrame(EConstraintFrame::Frame2),
						RawBodyTransforms[Joint.ParentBodyIndex]);
					const float Value = static_cast<float>(MeasureGenericJointPosition(
						ChildFrame, ParentFrame, Joint.Coordinate));
					checkf(
						WriteScalarValue(Value),
						TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
				}
				break;
			case EUERLObservationType::JointVelocity:
				{
					const FUERLJointTopology& Joint = Topology.Joints[Entry.JointIndex];
					float Value = 0.0f;
					if (IsGenericAngularCoordinate(Joint.Coordinate))
					{
						FConstraintInstance* Constraint = ResolveObservationConstraintInstance(Slot, Entry.JointIndex);
						checkf(Constraint, TEXT("generic Robot failed to read joint State field '%s'"), *Entry.FieldName.ToString());
						const FTransform ChildFrame = ComposeGenericConstraintFrameWorld(
							Constraint->GetRefFrame(EConstraintFrame::Frame1),
							RawBodyTransforms[Joint.ChildBodyIndex]);
						const FVector AxisWorld = ChildFrame.TransformVectorNoScale(
							GenericJointCoordinateAxis(Joint.Coordinate)).GetSafeNormal();
						checkf(!AxisWorld.IsNearlyZero(), TEXT("generic Robot failed to read joint State field '%s'"), *Entry.FieldName.ToString());
						const FVector RelativeAngularVelocity =
							BodyAngVels[Joint.ChildBodyIndex] - BodyAngVels[Joint.ParentBodyIndex];
						Value = static_cast<float>(FVector::DotProduct(RelativeAngularVelocity, AxisWorld));
					}
					else
					{
						checkf(PrismaticValid[PlanIndex], TEXT("generic Robot failed to read joint State field '%s'"), *Entry.FieldName.ToString());
						Value = PrismaticVelocities[PlanIndex];
					}
					checkf(
						WriteScalarValue(Value),
						TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
				}
				break;
			case EUERLObservationType::GroundClearance:
				{
					float Value = PrecomputedGroundClearance;
					if (!bHasPrecomputedClearance)
					{
						const FTransform& BodyTransform = (Entry.BodyIndex == Topology.RootBodyIndex)
							? ObservedRootTransform : RawBodyTransforms[Entry.BodyIndex];
						if (!MeasureGroundClearanceWithTransform(
							Slot, BodyTransform, PermittedActorsView, FallbackPermittedComponents, nullptr,
							Entry.BodyIndex, Value, OutError))
						{
							return false;
						}
					}
					checkf(
						WriteScalarValue(Value),
						TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
				}
				break;
			case EUERLObservationType::TerrainHeight:
				{
					checkf(Slot.TerrainHeightValues, TEXT("generic Robot terrain height buffer is missing"));
					if (!bHasPrecomputedTerrainScan)
					{
						if (!MeasureTerrainHeightScanWithTransform(
							Slot, Topology, ObservedRootTransform, PermittedActorsView, FallbackPermittedComponents,
							nullptr, Entry.BodyIndex, *Slot.TerrainHeightValues, OutError))
						{
							return false;
						}
					}
					checkf(
						WriteVectorValue(*Slot.TerrainHeightValues),
						TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
				}
				break;
			case EUERLObservationType::Contact:
				checkf(Slot.ContactState, TEXT("generic Robot contact state is missing"));
				checkf(
					WriteScalarValue(Slot.ContactState->SupportValue(Entry.BodyIndex)),
					TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
				break;
			case EUERLObservationType::ContactForce:
				checkf(Slot.ContactState, TEXT("generic Robot contact state is missing"));
				checkf(
					WriteScalarValue(Slot.ContactState->ContactForceNewtons(Entry.BodyIndex)),
					TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
				break;
			default:
				checkNoEntry();
				break;
			}
		}
		checkf(Slot.ContactState, TEXT("generic Robot contact state is missing"));
		Slot.ContactState->Clear();
		return true;
	}
}

bool WriteGenericRobotObservationFields(
	int32 SlotId,
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FUERLRobotObservationPlanEntry> Plan,
	FUERLNamedStateWriter& Writer,
	FString& OutError,
	TConstArrayView<const FUERLBatchFieldBinding*> PlanBindings)
{
	checkf(Slot.Component, TEXT("generic Robot cannot collect State without a component"));
	const FObservationPlanMask Mask = BuildObservationPlanMask(Topology, Slot.ConstraintIndices, Plan);
	return WriteGenericRobotObservationFieldsImpl(
		SlotId, Slot, Topology, Plan, Mask, Writer, OutError, PlanBindings, false, nullptr);
}

bool WriteGenericRobotObservationBatch(
	UWorld* World,
	TConstArrayView<int32> SlotIds,
	TConstArrayView<FUERLRobotObservationSlotView> SlotViews,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FUERLRobotObservationPlanEntry> Plan,
	FUERLNamedStateWriter& Writer,
	FString& OutError,
	TConstArrayView<const FUERLBatchFieldBinding*> PlanBindings)
{
	check(SlotIds.Num() == SlotViews.Num());
	if (SlotIds.IsEmpty())
	{
		OutError.Reset();
		return true;
	}
	const FObservationPlanMask Mask = BuildObservationPlanMask(Topology, SlotViews[0].ConstraintIndices, Plan);
	bool bCanBatchInOneLock = World && World->GetPhysicsScene() != nullptr;
	if (bCanBatchInOneLock && Mask.bNeedsGroundTrace)
	{
		for (const FUERLRobotObservationSlotView& View : SlotViews)
		{
			if (!SlotUsesPreparedGroundTraces(View)
				|| (View.ResolvedTerrainQueryActors.IsEmpty()
					&& (!View.TerrainQueryActors || View.TerrainQueryActors->IsEmpty())))
			{
				bCanBatchInOneLock = false;
				break;
			}
		}
	}

	if (!bCanBatchInOneLock)
	{
		for (int32 I = 0; I < SlotIds.Num(); ++I)
		{
			if (!WriteGenericRobotObservationFieldsImpl(
				SlotIds[I], SlotViews[I], Topology, Plan, Mask, Writer, OutError, PlanBindings, false, nullptr))
			{
				return false;
			}
		}
		OutError.Reset();
		return true;
	}

	bool bSuccess = true;
	FPreparedGroundCache SharedGroundCache;
	FPhysicsCommand::ExecuteRead(World->GetPhysicsScene(), [&]()
	{
		for (int32 I = 0; I < SlotIds.Num(); ++I)
		{
			if (!WriteGenericRobotObservationFieldsImpl(
				SlotIds[I], SlotViews[I], Topology, Plan, Mask, Writer, OutError, PlanBindings,
				true, &SharedGroundCache))
			{
				bSuccess = false;
				return;
			}
		}
	});
	return bSuccess;
}
