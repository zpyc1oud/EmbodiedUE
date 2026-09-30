#include "UERLGenericRobotObservationSampler.h"

#include "UERLBatchBinding.h"
#include "UERLGenericRobotContactListener.h"
#include "UERLGenericRobotKinematics.h"
#include "UERLInterfaceTypes.h"

#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"

FTransform ObserveGenericRobotBodyWorldTransform(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	int32 BodyIndex)
{
	checkf(Slot.Component, TEXT("generic Robot ground query has no component"));
	checkf(Topology.BodyNames.IsValidIndex(BodyIndex), TEXT("generic Robot ground query has invalid body index"));
	FBodyInstance* BodyInstance = Slot.Component->GetBodyInstance(Topology.BodyNames[BodyIndex]);
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
	FConstraintInstance* Constraint = Slot.Component->GetConstraintInstanceByIndex(
		(*Slot.ConstraintIndices)[Entry.JointIndex]);
	const FUERLJointTopology& Joint = Topology.Joints[Entry.JointIndex];
	if (!Constraint || !Topology.BodyNames.IsValidIndex(Joint.ChildBodyIndex)
		|| !Topology.BodyNames.IsValidIndex(Joint.ParentBodyIndex))
	{
		return false;
	}

	const EUERLJointCoordinate Coordinate = Joint.Coordinate;
	const bool bAngular = IsGenericAngularCoordinate(Coordinate);

	const FName ChildName = Topology.BodyNames[Joint.ChildBodyIndex];
	const FName ParentName = Topology.BodyNames[Joint.ParentBodyIndex];
	FBodyInstance* ChildInstance = Slot.Component->GetBodyInstance(ChildName);
	FBodyInstance* ParentInstance = Slot.Component->GetBodyInstance(ParentName);
	if (!ChildInstance || !ParentInstance)
	{
		return false;
	}
	const FTransform ChildFrame = ComposeGenericConstraintFrameWorld(
		Constraint->GetRefFrame(EConstraintFrame::Frame1),
		ChildInstance->GetUnrealWorldTransform());
	const FTransform ParentFrame = ComposeGenericConstraintFrameWorld(
		Constraint->GetRefFrame(EConstraintFrame::Frame2),
		ParentInstance->GetUnrealWorldTransform());
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
			const FVector RelativeLinearVelocity = ChildInstance->GetUnrealWorldVelocityAtPoint(
				ChildFrame.GetLocation())
				- ParentInstance->GetUnrealWorldVelocityAtPoint(ParentFrame.GetLocation());
			OutValue = static_cast<float>(FVector::DotProduct(RelativeLinearVelocity, AxisWorld) / 100.0);
		}
		return true;
	}
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

	bool FindGenericRobotGroundHit(
		const FUERLRobotObservationSlotView& Slot,
		UWorld& World,
		const FVector& Start,
		const FVector& End,
		const TCHAR* QueryName,
		FHitResult& OutHit,
		FString& OutError)
	{
		const bool bSharedWorld = Slot.CollisionProfile.Scope() == EUERLEnvironmentCollisionScope::SharedWorld;
		const bool bDeploymentWorldStatic =
			Slot.TerrainQueryPurpose == EUERLTerrainQueryPurpose::DeploymentWorldStatic;
		FCollisionQueryParams QueryParams(FCollisionQueryParams::DefaultQueryParam);
		QueryParams.TraceTag = FName(QueryName);
		QueryParams.bTraceComplex = false;
		QueryParams.AddIgnoredActor(Slot.Owner);
		if (!bSharedWorld)
		{
			if (!World.LineTraceSingleByChannel(
				OutHit, Start, End, Slot.CollisionProfile.Channel(), QueryParams))
			{
				OutError = FString::Printf(
					TEXT("generic Robot %s query found no Slot-isolated ground"), QueryName);
				return false;
			}
			return true;
		}
		if (!bDeploymentWorldStatic && (!Slot.TerrainQueryActors || Slot.TerrainQueryActors->IsEmpty()))
		{
			OutError = FString::Printf(
				TEXT("generic Robot %s query has no training terrain owner"), QueryName);
			return false;
		}

		TArray<FHitResult> LocalHits;
		TArray<FHitResult>* Hits = Slot.TerrainTraceHits ? Slot.TerrainTraceHits : &LocalHits;
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
					|| (Slot.TerrainQueryActors && Slot.TerrainQueryActors->ContainsByPredicate(
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
}

bool MeasureGenericRobotGroundClearance(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
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
	const FVector BodyLocation = ObserveGenericRobotBodyWorldTransform(Slot, Topology, BodyIndex).GetLocation();
	const double StartHeightCm = Slot.TerrainQueryPurpose == EUERLTerrainQueryPurpose::DeploymentWorldStatic
		? DeploymentProbeStartHeightCm : 200.0;
	const FVector Start = BodyLocation + FVector::UpVector * StartHeightCm;
	const FVector End = BodyLocation - FVector::UpVector * 200.0;
	FHitResult Hit;
	if (FindGenericRobotGroundHit(
		Slot, *World, Start, End, TEXT("ground-clearance"), Hit, OutError))
	{
		(*Slot.GroundClearanceWorldHits)[BodyIndex] = Hit.ImpactPoint;
		(*Slot.GroundClearanceHitValid)[BodyIndex] = 1;
	}
	else if ((*Slot.GroundClearanceHitValid)[BodyIndex])
	{
		Hit.ImpactPoint = (*Slot.GroundClearanceWorldHits)[BodyIndex];
	}
	else
	{
		OutError = FString::Printf(
			TEXT("generic Robot ground-clearance query has no initial hit for body %d: %s"),
			BodyIndex, *OutError);
		return false;
	}
	OutValue = static_cast<float>((BodyLocation.Z - Hit.ImpactPoint.Z) / 100.0);
	OutError.Reset();
	return true;
}

bool MeasureGenericRobotTerrainHeightScan(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
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

	const FTransform BodyTransform = ObserveGenericRobotBodyWorldTransform(Slot, Topology, BodyIndex);
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
	constexpr double ForwardStartM = -1.0;
	constexpr double ForwardStepM = 0.5;
	constexpr double LateralStartM = -0.6;
	constexpr double LateralStepM = 0.3;
	OutValues.SetNumUninitialized(UERLTerrainHeightScanWidth);
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
			FHitResult Hit;
			FString QueryError;
			if (FindGenericRobotGroundHit(
				Slot, *World, Start, End, TEXT("terrain-height"), Hit, QueryError))
			{
				(*Slot.TerrainHeightWorldHits)[Index] = Hit.ImpactPoint;
				(*Slot.TerrainHeightHitValid)[Index] = 1;
			}
			else if ((*Slot.TerrainHeightHitValid)[Index])
			{
				Hit.ImpactPoint = (*Slot.TerrainHeightWorldHits)[Index];
			}
			else
			{
				OutError = FString::Printf(
					TEXT("generic Robot terrain-height query has no initial hit at scan index %d: %s"),
					Index, *QueryError);
				return false;
			}
			OutValues[Index] = static_cast<float>((Hit.ImpactPoint.Z - RootLocation.Z) / 100.0);
		}
	}
	OutError.Reset();
	return true;
}

bool WriteGenericRobotObservationFields(
	int32 SlotId,
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FUERLRobotObservationPlanEntry> Plan,
	FUERLNamedStateWriter& Writer,
	FString& OutError)
{
	checkf(Slot.Component, TEXT("generic Robot cannot collect State without a component"));
	OutError.Reset();
	const USkeletalMeshComponent& Component = *Slot.Component;
	for (const FUERLRobotObservationPlanEntry& Entry : Plan)
	{
		checkf(
			Topology.BodyNames.IsValidIndex(Entry.BodyIndex),
			TEXT("generic Robot State field '%s' has invalid body index %d"),
			*Entry.FieldName.ToString(), Entry.BodyIndex);
		const FName BodyName = Topology.BodyNames[Entry.BodyIndex];
		switch (Entry.Type)
		{
		case EUERLObservationType::BodyPose:
			{
				FBodyInstance* BodyInstance = Component.GetBodyInstance(BodyName);
				checkf(
					BodyInstance,
					TEXT("generic Robot body pose field '%s' has no body instance for '%s'"),
					*Entry.FieldName.ToString(), *BodyName.ToString());
				const FTransform WorldTransform = ObserveGenericRobotBodyWorldTransform(Slot, Topology, Entry.BodyIndex);
				const FTransform LocalTransform = WorldTransform.GetRelativeTransform(
					FTransform(Slot.ObservationRotation, Slot.Origin));
				const FVector Position = LocalTransform.GetLocation() / 100.0;
				const FQuat Rotation = LocalTransform.GetRotation().GetNormalized();
				const float Values[] = {
					static_cast<float>(Position.X), static_cast<float>(Position.Y), static_cast<float>(Position.Z),
					static_cast<float>(Rotation.X), static_cast<float>(Rotation.Y),
					static_cast<float>(Rotation.Z), static_cast<float>(Rotation.W),
				};
				checkf(
					Writer.WriteVector(SlotId, Entry.FieldName, TConstArrayView<float>(Values, UE_ARRAY_COUNT(Values))),
					TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
			}
			break;
		case EUERLObservationType::BodyLinearVelocity:
			{
				const FVector Velocity = Slot.ObservationRotation.Inverse().RotateVector(
					Component.GetPhysicsLinearVelocity(BodyName)) / 100.0;
				const float Values[] = {
					static_cast<float>(Velocity.X), static_cast<float>(Velocity.Y), static_cast<float>(Velocity.Z) };
				checkf(
					Writer.WriteVector(SlotId, Entry.FieldName, TConstArrayView<float>(Values, UE_ARRAY_COUNT(Values))),
					TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
			}
			break;
		case EUERLObservationType::BodyAngularVelocity:
			{
				const FVector Velocity = Slot.ObservationRotation.Inverse().RotateVector(
					Component.GetPhysicsAngularVelocityInRadians(BodyName));
				const float Values[] = {
					static_cast<float>(Velocity.X), static_cast<float>(Velocity.Y), static_cast<float>(Velocity.Z) };
				checkf(
					Writer.WriteVector(SlotId, Entry.FieldName, TConstArrayView<float>(Values, UE_ARRAY_COUNT(Values))),
					TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
			}
			break;
		case EUERLObservationType::JointPosition:
		case EUERLObservationType::JointVelocity:
			{
				float Value = 0.0f;
				checkf(
					ReadGenericRobotJointScalar(
						Slot, Topology, Entry, Entry.Type == EUERLObservationType::JointVelocity, Value),
					TEXT("generic Robot failed to read joint State field '%s'"), *Entry.FieldName.ToString());
				checkf(
					Writer.WriteScalar(SlotId, Entry.FieldName, Value),
					TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
			}
			break;
		case EUERLObservationType::GroundClearance:
			{
				float Value = 0.0f;
				if (!MeasureGenericRobotGroundClearance(
					Slot, Topology, Entry.BodyIndex, Value, OutError))
				{
					return false;
				}
				checkf(
					Writer.WriteScalar(SlotId, Entry.FieldName, Value),
					TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
			}
			break;
		case EUERLObservationType::TerrainHeight:
			{
				checkf(Slot.TerrainHeightValues, TEXT("generic Robot terrain height buffer is missing"));
				if (!MeasureGenericRobotTerrainHeightScan(
					Slot, Topology, Entry.BodyIndex, *Slot.TerrainHeightValues, OutError))
				{
					return false;
				}
				checkf(
					Writer.WriteVector(SlotId, Entry.FieldName, *Slot.TerrainHeightValues),
					TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
			}
			break;
		case EUERLObservationType::Contact:
			checkf(Slot.ContactState, TEXT("generic Robot contact state is missing"));
			checkf(
				Writer.WriteScalar(
					SlotId, Entry.FieldName, Slot.ContactState->SupportValue(Entry.BodyIndex)),
				TEXT("generic Robot failed to write State field '%s'"), *Entry.FieldName.ToString());
			break;
		case EUERLObservationType::ContactForce:
			checkf(Slot.ContactState, TEXT("generic Robot contact state is missing"));
			checkf(
				Writer.WriteScalar(
					SlotId, Entry.FieldName, Slot.ContactState->ContactForceNewtons(Entry.BodyIndex)),
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
