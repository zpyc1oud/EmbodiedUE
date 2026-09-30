#pragma once

#include "CoreMinimal.h"

class AActor;
class UWorld;

/** Read-only facts about the World and its active Chaos physics configuration. */
struct UERLROBOT_API FUERLPhysicsSnapshot
{
	/** The solver backing the supplied World, or None when no solver is available. */
	FName SolverPath = NAME_None;
	/** Whether the supplied World currently owns a physics solver. */
	bool bHasSolver = false;
	bool bTickPhysicsAsync = false;
	bool bSubsteppingAsync = false;
	bool bSubstepping = false;
	double MaxSubstepDeltaTime = 0.0;
	int32 MaxSubsteps = 0;
	double MinPhysicsDeltaTime = 0.0;
	/** Kept for diagnostics; synchronous substep validation does not reject it. */
	double MaxPhysicsDeltaTime = 0.0;
	double WorldTimeDilation = 1.0;
	double OwnerTimeDilation = 1.0;
	/** False when the caller did not provide the deployment owner actor. */
	bool bOwnerTimeDilationKnown = false;
	/** Actual World gravity, retained for diagnostics only. */
	double GravityZ = 0.0;
};

/** One read of the completed solver clock at a safe synchronous boundary. */
struct UERLROBOT_API FUERLSolverClockSnapshot
{
	int32 Frame = 0;
	double SolverTime = 0.0;
	double LastDt = 0.0;
};

/** Read World settings and the solver selected by that same World without mutating them. */
UERLROBOT_API bool ReadUERLPhysicsSnapshot(
	const UWorld& World,
	const AActor* Owner,
	FUERLPhysicsSnapshot& OutSnapshot,
	FString& OutError);

/** Read the completed solver clock from the supplied World without inferring time from configuration. */
UERLROBOT_API bool ReadUERLSolverClock(
	const UWorld& World,
	FUERLSolverClockSnapshot& OutClock,
	FString& OutError);

/** Downward WorldStatic ground clearance in metres at Location along Up. */
UERLROBOT_API bool MeasureUERLWorldStaticClearanceMeters(
	UWorld& World,
	const FVector& Location,
	const FVector& Up,
	double& OutMeters,
	FString& OutError,
	const AActor* IgnoreActor = nullptr);
