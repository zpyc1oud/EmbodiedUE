#pragma once

#include "CoreMinimal.h"
#include "UERLGenericRobotAssetLoader.h"
#include "UERLGenericRobotContactListener.h"
#include "UERLInterfaceTypes.h"

class AActor;
class USkeletalMeshComponent;

/**
 * Mutable Slot handles for one reset pass. The provider owns Slot storage; the
 * reset applier never allocates Actor or contact lifetime.
 */
struct FUERLRobotResetSlotRef
{
	AActor* Owner = nullptr;
	USkeletalMeshComponent* Component = nullptr;
	const TArray<int32>* BoneIndices = nullptr;
	const TArray<int32>* ConstraintIndices = nullptr;
	FVector* Origin = nullptr;
	FQuat* GroundRotation = nullptr;
	const TArray<FUERLGenericRobotCanonicalBody>* CanonicalBodies = nullptr;
	TArray<float>* Targets = nullptr;
	FGenericRobotContactState* ContactState = nullptr;
	bool* bTerrainBoundary = nullptr;
	bool bPreserveOwnerTransform = false;
};

/**
 * Validate then apply one Reset batch. SI→cm conversions for root/joint pose and
 * linear velocity live only here on the reset path.
 */
UERLROBOT_API bool ResetGenericRobotSlots(
	TArrayView<FUERLRobotResetSlotRef> Slots,
	const FUERLRobotTopology& Topology,
	TConstArrayView<int32> ParentBodyIndices,
	TConstArrayView<FUERLActuatorConfig> Actuators,
	TConstArrayView<FUERLResetBinding> ResetBindings,
	const FUERLResetBatch& Reset,
	FString& OutError);
