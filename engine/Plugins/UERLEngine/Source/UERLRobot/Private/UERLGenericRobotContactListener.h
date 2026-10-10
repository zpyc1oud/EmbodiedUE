#pragma once

#include "CoreMinimal.h"
#include "Chaos/Particle/ObjectState.h"
#include "Components/ActorComponent.h"
#include "Engine/HitResult.h"
#include "UERLInterfaceTypes.h"
#include "UERLSlotCollisionPlan.h"

#include "UERLGenericRobotContactListener.generated.h"

class USkeletalMeshComponent;
class AActor;
struct FGenericRobotContactState;

/** Apply the body-Environment event filter at the collision seam. */
UERLROBOT_API bool IsGenericRobotEnvironmentHit(
	const FHitResult& Hit,
	EUERLEnvironmentCollisionScope CollisionScope,
	ECollisionChannel OtherObjectType,
	bool bBelongsToSlotEnvironment);

/** Resolve an event bone name against the reflected body index map. */
UERLROBOT_API bool ResolveGenericRobotContactBodyIndex(
	const TMap<FName, int32>& BodyIndexByName,
	FName BodyName,
	int32& OutBodyIndex);

/** Shared-world force includes static and kinematic supports, not simulated other bodies. */
UERLROBOT_API bool IsGenericRobotContactForceStateAccepted(
	EUERLEnvironmentCollisionScope CollisionScope,
	Chaos::EObjectStateType OtherState);

/** Convert one completed solver-step Chaos impulse from UE centimetres to SI force. */
UERLROBOT_API float ConvertGenericRobotAccumulatedImpulseToForceNewtons(
	const FVector& SolverStepImpulse,
	double SolverStepSeconds);

/**
 * Sample Chaos contact impulses for every reflected body and record the latest SI force.
 * Shared-world mode ignores dynamic-other contacts; isolated mode accepts all non-robot others.
 */
UERLROBOT_API void SampleGenericRobotContactForces(
	USkeletalMeshComponent& Component,
	const TArray<FName>& BodyNames,
	FGenericRobotContactState& ContactState,
	EUERLEnvironmentCollisionScope CollisionScope,
	double SolverStepSeconds);

/**
 * Probe geometric terrain support beneath one body. SharedWorld accepts any
 * blocking WorldStatic hit; isolated mode uses the Slot collision channel.
 */
UERLROBOT_API bool IsGenericRobotBodySupportedByTerrain(
	USkeletalMeshComponent& Component,
	AActor& Owner,
	const FUERLRobotTopology& Topology,
	const FUERLSlotCollisionProfile& CollisionProfile,
	const FQuat& GroundRotation,
	const FTransform& RootCanonicalSlotTransform,
	int32 BodyIndex,
	TArray<FHitResult>& ScratchHits);

/** Hold event flags plus the one completed solver-step contact sample for one Robot Slot. */
struct FGenericRobotContactState
{
	explicit FGenericRobotContactState(int32 BodyCount)
	{
		check(BodyCount >= 0);
		ContactFlags.SetNumZeroed(BodyCount);
		SupportFlags.SetNumZeroed(BodyCount);
		ContactForces.SetNumZeroed(BodyCount);
	}

	void Clear()
	{
		for (uint8& Flag : ContactFlags)
		{
			Flag = 0;
		}
		for (uint8& Flag : SupportFlags)
		{
			Flag = 0;
		}
		for (float& Force : ContactForces)
		{
			Force = 0.0f;
		}
	}

	void MarkBody(int32 BodyIndex)
	{
		checkf(ContactFlags.IsValidIndex(BodyIndex), TEXT("generic Robot contact body index is out of range"));
		ContactFlags[BodyIndex] = 1;
	}

	/** Start the one completed-solver-step sample that will feed the next observation. */
	void BeginPhysicsSample()
	{
		for (uint8& Flag : SupportFlags)
		{
			Flag = 0;
		}
		for (float& Force : ContactForces)
		{
			Force = 0.0f;
		}
	}

	/** Mark a body as geometrically supported in the completed solver-step sample. */
	void MarkBodySupported(int32 BodyIndex)
	{
		checkf(SupportFlags.IsValidIndex(BodyIndex), TEXT("generic Robot support body index is out of range"));
		SupportFlags[BodyIndex] = 1;
	}

	bool HasContact(int32 BodyIndex) const
	{
		return ContactFlags[BodyIndex] != 0;
	}

	/** Return 1 when the body was supported at the observation boundary, otherwise 0. */
	float SupportValue(int32 BodyIndex) const
	{
		checkf(SupportFlags.IsValidIndex(BodyIndex), TEXT("generic Robot support body index is out of range"));
		return SupportFlags[BodyIndex] != 0 ? 1.0f : 0.0f;
	}

	/** Record the net contact force from the latest completed solver step. */
	void RecordContactForce(int32 BodyIndex, float ForceNewtons)
	{
		checkf(ContactForces.IsValidIndex(BodyIndex), TEXT("generic Robot contact force body index is out of range"));
		ContactForces[BodyIndex] = ForceNewtons;
	}

	/** Return the latest net contact force in newtons. */
	float ContactForceNewtons(int32 BodyIndex) const
	{
		checkf(ContactForces.IsValidIndex(BodyIndex), TEXT("generic Robot contact force body index is out of range"));
		return ContactForces[BodyIndex];
	}

	TArray<uint8> ContactFlags;
	TArray<uint8> SupportFlags;
	TArray<float> ContactForces;
};

/** UObject listener needed to bind the dynamic SkeletalMesh OnComponentHit delegate. */
UCLASS()
class UERLROBOT_API UERLGenericRobotContactListener final : public UActorComponent
{
	GENERATED_BODY()

public:
	/** Bind this listener to one Slot's SkeletalMeshComponent and contact state. */
	void Bind(
		USkeletalMeshComponent& InComponent,
		FGenericRobotContactState& InState,
		EUERLEnvironmentCollisionScope InCollisionScope,
		const TArray<TWeakObjectPtr<AActor>>& InEnvironmentActors,
		const TArray<FName>& BodyNames);

	/** Remove the hit delegate and release the state pointer. */
	void Unbind();

	virtual void BeginDestroy() override;

private:
	UFUNCTION()
	void HandleHit(
		UPrimitiveComponent* HitComponent,
		AActor* OtherActor,
		UPrimitiveComponent* OtherComp,
		FVector NormalImpulse,
		const FHitResult& Hit);

	TWeakObjectPtr<USkeletalMeshComponent> BoundComponent;
	EUERLEnvironmentCollisionScope CollisionScope = EUERLEnvironmentCollisionScope::SlotIsolated;
	TArray<TWeakObjectPtr<AActor>> EnvironmentActors;
	FGenericRobotContactState* ContactState = nullptr;
	TMap<FName, int32> BodyIndexByName;
};
