#pragma once

#include "CoreMinimal.h"
#include "UERLGenericRobotContactListener.h"
#include "UERLInterfaceTypes.h"
#include "UERLProvider.h"
#include "UERLSlotCollisionPlan.h"
#include "UObject/StrongObjectPtr.h"

class AActor;
class UPhysicalMaterial;
class USkeletalMeshComponent;
class UWorld;

namespace Chaos
{
	enum class ESleepType : uint8;
}

/** Canonical body pose/velocity sampled in Slot space at spawn. */
struct FUERLGenericRobotCanonicalBody
{
	FTransform SlotTransform = FTransform::Identity;
	FVector SlotLinearVelocity = FVector::ZeroVector;
	FVector SlotAngularVelocity = FVector::ZeroVector;
};

/** Per-body authored iteration settings restored after a component claim. */
struct FUERLGenericRobotClaimedIterations
{
	FName BodyName;
	uint8 Position = 0;
	uint8 Velocity = 0;
	uint8 Projection = 0;
	bool bOverride = false;
};

/** One successfully spawned Slot owned by the AssetLoader until absorbed by the provider. */
struct FUERLGenericRobotSpawnedSlot
{
	TWeakObjectPtr<AActor> Owner;
	TWeakObjectPtr<USkeletalMeshComponent> Component;
	TWeakObjectPtr<UERLGenericRobotContactListener> ContactListener;
	TUniquePtr<FGenericRobotContactState> ContactState;
	TArray<int32> BoneIndices;
	TArray<int32> ConstraintIndices;
	FVector Origin = FVector::ZeroVector;
	FQuat GroundRotation = FQuat::Identity;
	FUERLSlotCollisionProfile CollisionProfile;
	TArray<TWeakObjectPtr<AActor>> TerrainQueryActors;
	EUERLTerrainQueryPurpose TerrainQueryPurpose = EUERLTerrainQueryPurpose::TrainingOwned;
	/** Direct deployment observes body pose/velocity against world Up. */
	bool bWorldUpObservationFrame = false;
	FVector2D TerrainHalfExtent = FVector2D::ZeroVector;
	FVector TerrainBoundsOriginOffset = FVector::ZeroVector;
	bool bTerrainBoundsValid = false;
	TArray<FUERLGenericRobotCanonicalBody> CanonicalBodies;
	TArray<float> Targets;
	bool bOwnsActor = true;
	bool bPreserveOwnerTransform = false;
	/** Claim cleanup restores the authored component settings and transform. */
	FTransform ClaimedTransform = FTransform::Identity;
	TEnumAsByte<EComponentMobility::Type> ClaimedMobility = EComponentMobility::Movable;
	TEnumAsByte<ECollisionEnabled::Type> ClaimedCollisionEnabled = ECollisionEnabled::NoCollision;
	FName ClaimedCollisionProfile = NAME_None;
	bool bClaimedNotifyRigidBodyCollision = false;
	bool bClaimedGravityEnabled = true;
	bool bClaimedTickEnabled = true;
	bool bHasClaimedSettings = false;
	/** Original attachment and drive state restored when a claimed component is released. */
	TWeakObjectPtr<USceneComponent> ClaimedAttachParent;
	FTransform ClaimedRelativeTransform = FTransform::Identity;
	bool bClaimedSimulatePhysics = false;
	TArray<TPair<FName, Chaos::ESleepType>> ClaimedSleepTypes;
	TArray<TPair<int32, bool>> ClaimedProjection;
	TArray<FUERLGenericRobotClaimedIterations> ClaimedIterations;
	TStrongObjectPtr<UPhysicalMaterial> ClaimedPhysMaterialOverride;
};

/** Map each reflected joint to its runtime PhysicsAsset constraint index. */
UERLROBOT_API bool ResolveGenericRobotRuntimeConstraintIndices(
	USkeletalMeshComponent& Component,
	const FUERLRobotTopology& Topology,
	TArray<int32>& OutIndices,
	FString& OutError);

/**
 * Load the SkeletalMesh asset, spawn or claim Slot Actors, and initialize physics.
 * On failure, any partially spawned Actors in OutSlots are destroyed before return.
 */
UERLROBOT_API bool SpawnGenericRobotSlots(
	UWorld& World,
	const FString& AssetPath,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FUERLActuatorConfig> Actuators,
	bool bClaimAuthoredActor,
	const TArray<FUERLSlotContext>& SlotContexts,
	TArray<FUERLGenericRobotSpawnedSlot>& OutSlots,
	FString& OutError,
	USkeletalMeshComponent* ExplicitClaimedComponent = nullptr,
	const FTransform* ExplicitPlacementTransform = nullptr);

/** Destroy spawned Slot Actors / contact listeners created by SpawnGenericRobotSlots. */
UERLROBOT_API void DestroyGenericRobotSpawnedSlots(TArray<FUERLGenericRobotSpawnedSlot>& Slots);
