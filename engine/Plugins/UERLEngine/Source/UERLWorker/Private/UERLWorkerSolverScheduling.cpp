#include "UERLWorkerSolverScheduling.h"

#include "Engine/World.h"
#include "PBDRigidsSolver.h"
#include "Physics/Experimental/PhysScene_Chaos.h"

FUERLWorkerSolverScheduling::~FUERLWorkerSolverScheduling()
{
	Reset();
}

bool FUERLWorkerSolverScheduling::Acquire(UWorld& InWorld, FString& OutError)
{
	check(IsInGameThread());
	Reset();
	FPhysScene* Scene = InWorld.GetPhysicsScene();
	Chaos::FPhysicsSolver* CurrentSolver = Scene ? Scene->GetSolver() : nullptr;
	if (!CurrentSolver)
	{
		OutError = TEXT("headless Worker scheduling requires a World Chaos solver");
		return false;
	}
	World = &InWorld;
	Solver = CurrentSolver;
	PreviousMode = Solver->GetThreadingMode();
	// This changes only outer dispatch. It retains buffering, fixed steps,
	// internal solver work and the normal EndFrame result synchronization.
	Solver->SetThreadingMode_External(Chaos::EThreadingModeTemp::SingleThread);
	OutError.Reset();
	return true;
}

void FUERLWorkerSolverScheduling::Reset()
{
	if (Solver)
	{
		check(IsInGameThread());
		UWorld* BoundWorld = World.Get();
		FPhysScene* Scene = BoundWorld ? BoundWorld->GetPhysicsScene() : nullptr;
		if (Scene && Scene->GetSolver() == Solver
			&& Solver->GetThreadingMode() == Chaos::EThreadingModeTemp::SingleThread)
		{
			Solver->SetThreadingMode_External(PreviousMode);
		}
	}
	Solver = nullptr;
	World.Reset();
}
