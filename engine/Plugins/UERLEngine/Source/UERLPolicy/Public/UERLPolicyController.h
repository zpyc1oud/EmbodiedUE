#pragma once

#include "CoreMinimal.h"
#include "UERLPolicyArtifact.h"
#include "UERLPolicyNetwork.h"
#include "UERLPolicyPlanRuntime.h"
#include "UERLSkeletalMeshRobotRuntime.h"

/**
 * Host-supplied command channel values for one control step.
 *
 * Channels are declared by the observation plan's ``command`` operators.
 * Missing or wrong-width channels fail ``Step`` — never silent zero-fill.
 */
struct UERLPOLICY_API FUERLPolicyCommands
{
	void Set(FName Channel, TConstArrayView<float> Values);
	const TArray<float>* Find(FName Channel) const;
	void Reset();

private:
	TMap<FName, TArray<float>> Channels;
};

/** One required host command channel (name + width) derived from the artifact. */
struct UERLPOLICY_API FUERLPolicyCommandChannel
{
	FName Name;
	int32 Width = 0;
};

/**
 * Controller config: artifact + asset + world placement only.
 *
 * Intentionally has no actuator or observation enumeration fields — those are
 * derived from the artifact's plans and ``robot_runtime`` segment so misuse
 * cannot be expressed.
 */
struct UERLPOLICY_API FUERLPolicyControllerConfig
{
	FString ArtifactPath;
	FString AssetPath;
	FVector GroundOrigin = FVector::ZeroVector;
	FVector GroundNormal = FVector::UpVector;
	TArray<TWeakObjectPtr<AActor>> TerrainQueryActors;
	/** Host actor excluded from deployment start-clearance and pose-reset probes. */
	TWeakObjectPtr<AActor> GroundQueryIgnoreActor;
	double InitialRootHeightMeters = 0.0;
	/** When true, claim an authored SkeletalMesh on the owner. */
	bool bClaimAuthoredActor = false;
	/** Direct runtime owner mesh; null preserves provider search behavior. */
	USkeletalMeshComponent* ClaimedMesh = nullptr;
	/** Spawn placement supplied by a component owner. */
	FTransform PlacementTransform = FTransform::Identity;
	bool bHasPlacementTransform = false;
};

/** Timing for one completed control window and its last solver sub-step. */
struct UERLPOLICY_API FUERLControlTiming
{
	/** Current completed-window duration, clamped only for policy observation. */
	double ObservationDtSeconds = 0.0;
	/** Actual dt of the latest completed solver sub-step, used for contact force conversion. */
	double LastSolverStepSeconds = 0.0;

	bool IsValid() const
	{
		return FMath::IsFinite(ObservationDtSeconds) && ObservationDtSeconds > 0.0
			&& FMath::IsFinite(LastSolverStepSeconds) && LastSolverStepSeconds > 0.0;
	}
};

/**
 * Robot-agnostic policy control loop:
 * collect state → observation plan → NNE → action plan → apply commands.
 */
class UERLPOLICY_API FUERLPolicyController
{
public:
	bool Initialize(UWorld& World, const FUERLPolicyControllerConfig& Config, FString& OutError);
	/** Initialize from cooked asset bytes; no source .uerlpol2 file is required. */
	bool InitializeFromBytes(
		UWorld& World,
		TConstArrayView<uint8> ArtifactBytes,
		const FUERLPolicyControllerConfig& Config,
		FString& OutError);

	/** One control step. Missing command channels error (no silent zero-fill). */
	bool Step(
		const FUERLPolicyCommands& Commands,
		const FUERLControlTiming& Timing,
		FString& OutError);

	/** Clear previous-action / working buffers; robot pose is left to the host. */
	void Reset();
	/** Release spawned/claimed robot resources while retaining no running state. */
	void Shutdown();
	/** Reset the live robot pose to the runtime reference pose. */
	bool ResetToReferencePose(FString& OutError);

	/** Current raw robot state for host command math (e.g. pursuit). */
	bool CollectState(TArray<float>& OutState, FString& OutError) const;
	/** Live robot Actor transform after Start (spawned Slot owner or claimed owner). */
	bool GetRobotTransform(FTransform& OutTransform, FString& OutError) const;
	/** Controlled mesh for explicit physics sync; null for the spawned path. */
	USkeletalMeshComponent* GetControlledMeshComponent() const
	{
		return Robot.GetControlledMeshComponent();
	}
	/** Read the World solver clock associated with the initialized Robot. */
	bool ReadCompletedSolverClock(FUERLSolverClockSnapshot& OutClock, FString& OutError) const
	{
		return Robot.ReadCompletedSolverClock(OutClock, OutError);
	}

	/** Artifact-required command channels and widths for host self-check. */
	const TArray<FUERLPolicyCommandChannel>& RequiredCommands() const { return RequiredCommandChannels; }
	/** Timing consumed by the most recent successful control step. */
	const FUERLControlTiming& LastControlTiming() const { return LastTiming; }

	/** State field layout matching ``CollectState`` (same order as observation plan requirements). */
	const TArray<FUERLFieldDescriptor>& GetSelectedStateFields() const { return SelectedStateFields; }

	bool IsInitialized() const { return bInitialized; }

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FUERLPolicyControllerResetBuffersTest;
#endif

	bool bInitialized = false;
	FUERLControlTiming LastTiming;

	FUERLPolicyArtifact Artifact;
	FUERLPlanRuntime ObservationRuntime;
	FUERLPlanRuntime ActionRuntime;
	FUERLPolicyNetwork Network;
	FUERLSkeletalMeshRobotRuntime Robot;
	TArray<FUERLFieldDescriptor> SelectedStateFields;
	TArray<FUERLPolicyCommandChannel> RequiredCommandChannels;

	TArray<float> RawState;
	TArray<float> Observation;
	TArray<float> Action;
	TArray<float> Targets;
	TArray<float> PreviousAction;
};
