#pragma once

#include "CoreMinimal.h"
#include "Engine/HitResult.h"
#include "UERLRobotObservationPlan.h"
#include "UERLProvider.h"
#include "UERLSlotCollisionPlan.h"

class AActor;
class FUERLNamedStateWriter;
class USkeletalMeshComponent;
struct FGenericRobotContactState;

/**
 * Slot-local inputs for one observation sample. The provider fills this view from
 * its Slot storage; the sampler never owns Actor or contact lifetime.
 */
struct FUERLRobotObservationSlotView
{
	USkeletalMeshComponent* Component = nullptr;
	AActor* Owner = nullptr;
	const TArray<int32>* ConstraintIndices = nullptr;
	FVector Origin = FVector::ZeroVector;
	FQuat GroundRotation = FQuat::Identity;
	/** Observation reference rotation; direct deployment uses world Up (identity). */
	FQuat ObservationRotation = FQuat::Identity;
	FUERLSlotCollisionProfile CollisionProfile;
	const TArray<TWeakObjectPtr<AActor>>* TerrainQueryActors = nullptr;
	EUERLTerrainQueryPurpose TerrainQueryPurpose = EUERLTerrainQueryPurpose::TrainingOwned;
	FGenericRobotContactState* ContactState = nullptr;
	/** Canonical root body transform in Slot space (for root pose recovery). */
	FTransform RootCanonicalSlotTransform = FTransform::Identity;
	TArray<FHitResult>* TerrainTraceHits = nullptr;
	TArray<float>* TerrainHeightValues = nullptr;
	TArray<FVector>* TerrainHeightWorldHits = nullptr;
	TArray<uint8>* TerrainHeightHitValid = nullptr;
	TArray<FVector>* GroundClearanceWorldHits = nullptr;
	TArray<uint8>* GroundClearanceHitValid = nullptr;
};

/** Recover the observed world transform for one body, rebasing the root when needed. */
UERLROBOT_API FTransform ObserveGenericRobotBodyWorldTransform(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	int32 BodyIndex);

/** Read one scalar joint position (m/rad) or velocity (m/s / rad/s). */
UERLROBOT_API bool ReadGenericRobotJointScalar(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	const FUERLRobotObservationPlanEntry& Entry,
	bool bVelocity,
	float& OutValue);

/** Measure vertical ground clearance in metres beneath one body. */
UERLROBOT_API bool MeasureGenericRobotGroundClearance(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	int32 BodyIndex,
	float& OutValue,
	FString& OutError);

/** Fill the fixed-width terrain height scan in Slot-local metres. */
UERLROBOT_API bool MeasureGenericRobotTerrainHeightScan(
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	int32 BodyIndex,
	TArray<float>& OutValues,
	FString& OutError);

/**
 * Write every planned observation for one Slot and clear the completed contact sample.
 * Unit and frame conversions for body/joint readings live only here.
 */
UERLROBOT_API bool WriteGenericRobotObservationFields(
	int32 SlotId,
	const FUERLRobotObservationSlotView& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FUERLRobotObservationPlanEntry> Plan,
	FUERLNamedStateWriter& Writer,
	FString& OutError);
