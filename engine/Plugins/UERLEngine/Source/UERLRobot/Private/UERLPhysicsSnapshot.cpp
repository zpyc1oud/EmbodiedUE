#include "UERLPhysicsSnapshot.h"

#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "PBDRigidsSolver.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsEngine/PhysicsSettings.h"

namespace
{
	bool PhysicsSnapshotFail(FString& OutError, const TCHAR* Message)
	{
		OutError = Message;
		return false;
	}
}

bool ReadUERLPhysicsSnapshot(
	const UWorld& World,
	const AActor* Owner,
	FUERLPhysicsSnapshot& OutSnapshot,
	FString& OutError)
{
	OutSnapshot = FUERLPhysicsSnapshot();
	OutError.Reset();

	const UPhysicsSettings* Settings = UPhysicsSettings::Get();
	if (!Settings)
	{
		return PhysicsSnapshotFail(OutError, TEXT("UPhysicsSettings is unavailable"));
	}
	const AWorldSettings* WorldSettings = World.GetWorldSettings();
	if (!WorldSettings)
	{
		return PhysicsSnapshotFail(OutError, TEXT("WorldSettings is unavailable"));
	}

	OutSnapshot.bTickPhysicsAsync = Settings->bTickPhysicsAsync;
	OutSnapshot.bSubsteppingAsync = Settings->bSubsteppingAsync;
	OutSnapshot.bSubstepping = Settings->bSubstepping;
	OutSnapshot.MaxSubstepDeltaTime = static_cast<double>(Settings->MaxSubstepDeltaTime);
	OutSnapshot.MaxSubsteps = Settings->MaxSubsteps;
	OutSnapshot.MinPhysicsDeltaTime = static_cast<double>(Settings->MinPhysicsDeltaTime);
	OutSnapshot.MaxPhysicsDeltaTime = static_cast<double>(Settings->MaxPhysicsDeltaTime);
	OutSnapshot.WorldTimeDilation = static_cast<double>(WorldSettings->GetEffectiveTimeDilation());
	OutSnapshot.GravityZ = static_cast<double>(WorldSettings->GetGravityZ());
	if (Owner)
	{
		OutSnapshot.OwnerTimeDilation = static_cast<double>(Owner->CustomTimeDilation);
		OutSnapshot.bOwnerTimeDilationKnown = true;
	}

	if (World.GetPhysicsScene())
	{
		if (Chaos::FPhysicsSolver* Solver = World.GetPhysicsScene()->GetSolver())
		{
			OutSnapshot.SolverPath = FName(TEXT("Chaos"));
			OutSnapshot.bHasSolver = true;
		}
	}
	return true;
}

bool ReadUERLSolverClock(
	const UWorld& World,
	FUERLSolverClockSnapshot& OutClock,
	FString& OutError)
{
	OutClock = FUERLSolverClockSnapshot();
	OutError.Reset();
	if (!World.GetPhysicsScene())
	{
		return PhysicsSnapshotFail(OutError, TEXT("World physics scene is unavailable"));
	}
	Chaos::FPhysicsSolver* Solver = World.GetPhysicsScene()->GetSolver();
	if (!Solver)
	{
		return PhysicsSnapshotFail(OutError, TEXT("World Chaos solver is unavailable"));
	}

	OutClock.Frame = Solver->GetCurrentFrame();
	OutClock.SolverTime = static_cast<double>(Solver->GetSolverTime());
	OutClock.LastDt = static_cast<double>(Solver->GetLastDt());
	if (!FMath::IsFinite(OutClock.SolverTime) || !FMath::IsFinite(OutClock.LastDt))
	{
		OutClock = FUERLSolverClockSnapshot();
		return PhysicsSnapshotFail(OutError, TEXT("World Chaos solver clock is not finite"));
	}
	return true;
}

bool MeasureUERLWorldStaticClearanceMeters(
	UWorld& World,
	const FVector& Location,
	const FVector& Up,
	double& OutMeters,
	FString& OutError,
	const AActor* IgnoreActor)
{
	OutMeters = 0.0;
	OutError.Reset();
	if (Location.ContainsNaN() || Up.ContainsNaN() || !Up.IsNormalized())
	{
		return PhysicsSnapshotFail(OutError, TEXT("clearance probe location is invalid"));
	}
	FCollisionQueryParams QueryParams(FCollisionQueryParams::DefaultQueryParam);
	QueryParams.bTraceComplex = false;
	if (IgnoreActor)
	{
		QueryParams.AddIgnoredActor(IgnoreActor);
	}
	FHitResult Hit;
	const FVector TraceStart = Location + Up * 10.0;
	const FVector TraceEnd = Location - Up * 10000.0;
	if (!World.LineTraceSingleByObjectType(
			Hit,
			TraceStart,
			TraceEnd,
			FCollisionObjectQueryParams(ECC_WorldStatic),
			QueryParams)
		|| !Hit.bBlockingHit)
	{
		return PhysicsSnapshotFail(OutError, TEXT("no WorldStatic ground under the robot to measure start clearance"));
	}
	OutMeters = FVector::DotProduct(Location - Hit.ImpactPoint, Up) / 100.0;
	if (!FMath::IsFinite(OutMeters) || OutMeters <= 0.0)
	{
		OutMeters = 0.0;
		return PhysicsSnapshotFail(OutError, TEXT("measured start clearance is not a positive finite height"));
	}
	return true;
}
