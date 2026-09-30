#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "UERLInterfaceTypes.h"
#include "UERLPhysicsSnapshot.h"

class AActor;
class USkeletalMeshComponent;
class UWorld;

/** Describe one named actuator used by an in-game SkeletalMesh robot. */
struct FUERLSkeletalMeshRuntimeActuator
{
	FName JointName;
	double Stiffness = 0.0;
	double Damping = 0.0;
	double EffortLimit = 0.0;
	double DefaultPosition = 0.0;
};

/** Select one generic robot observation by semantic type and reflected target name. */
struct FUERLSkeletalMeshRuntimeObservation
{
	EUERLObservationType Type = EUERLObservationType::None;
	FName TargetName;
};

/** Configure one authored SkeletalMesh robot for direct in-game control. */
struct FUERLSkeletalMeshRobotRuntimeConfig
{
	FString AssetPath;
	/** Optional explicit authored component for direct deployment claim. */
	USkeletalMeshComponent* ClaimedMesh = nullptr;
	/** Optional transform used only when the runtime spawns a new Actor. */
	FTransform PlacementTransform = FTransform::Identity;
	bool bHasPlacementTransform = false;
	TArray<FUERLSkeletalMeshRuntimeActuator> Actuators;
	TArray<FUERLSkeletalMeshRuntimeObservation> Observations;
	FVector GroundOrigin = FVector::ZeroVector;
	FVector GroundNormal = FVector::UpVector;
	TArray<TWeakObjectPtr<AActor>> TerrainQueryActors;
	double InitialRootHeightMeters = 0.0;
	bool bClaimAuthoredActor = true;
};

/**
 * Run the existing generic SkeletalMesh physics adapter without a Worker session.
 *
 * The caller supplies named actuator and observation selections. This module owns
 * topology reflection, schema binding, reset, state collection, and Chaos drive
 * application behind one in-game interface.
 */
class UERLROBOT_API FUERLSkeletalMeshRobotRuntime
{
public:
	FUERLSkeletalMeshRobotRuntime();
	~FUERLSkeletalMeshRobotRuntime();

	FUERLSkeletalMeshRobotRuntime(const FUERLSkeletalMeshRobotRuntime&) = delete;
	FUERLSkeletalMeshRobotRuntime& operator=(const FUERLSkeletalMeshRobotRuntime&) = delete;

	bool Initialize(UWorld& World, const FUERLSkeletalMeshRobotRuntimeConfig& Config, FString& OutError);
	/** Sample the completed solver step; the caller must be in TG_PostPhysics. */
	void SamplePhysicsContacts(double SolverStepSeconds);
	/** Read the last completed dt from this Robot's World solver. */
	double GetLastSolverStepSeconds();
	/** Read the completed solver clock; the caller decides whether Frame advanced. */
	bool ReadCompletedSolverClock(FUERLSolverClockSnapshot& OutClock, FString& OutError) const;
	/** Clear the shared Robot contact sample without destroying its actor. */
	void ClearContactState();
	/** Clear contact and cached terrain samples without destroying its actor. */
	void ClearObservationCaches();
	bool CollectState(TArray<float>& OutState, FString& OutError) const;
	/** Live Slot actor transform; spawn path returns the spawned Actor, claim path returns the owner. */
	bool GetPrimaryActorTransform(FTransform& OutTransform, FString& OutError) const;
	/** Claimed mesh component when the claim path is active; null for spawned robots. */
	USkeletalMeshComponent* GetControlledMeshComponent() const;
	bool ApplyActuatorTargets(TConstArrayView<float> Targets, FString& OutError);
	/** Reset the live Robot to its captured reference pose without destroying it. */
	bool ResetToReferencePose(FString& OutError);
	void Reset();

	int32 StateWidth() const;
	int32 ActuatorCount() const;
	bool IsInitialized() const;
	const TArray<FUERLFieldDescriptor>& SelectedStateFields() const;

private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
