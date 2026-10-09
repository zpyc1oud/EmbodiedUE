#pragma once

#include "CoreMinimal.h"
#include "Chaos/Framework/PhysicsSolverBase.h"

class UWorld;

/** Own a headless Worker's outer solver scheduling mode, not solver parameters. */
class FUERLWorkerSolverScheduling final
{
public:
	FUERLWorkerSolverScheduling() = default;
	FUERLWorkerSolverScheduling(const FUERLWorkerSolverScheduling&) = delete;
	FUERLWorkerSolverScheduling& operator=(const FUERLWorkerSolverScheduling&) = delete;
	~FUERLWorkerSolverScheduling();

	/** Acquire on the GameThread before the Worker releases its first physics frame. */
	bool Acquire(UWorld& InWorld, FString& OutError);
	/** Restore the same surviving solver; never modify another World's solver. */
	void Reset();

private:
	TWeakObjectPtr<UWorld> World;
	Chaos::FPhysicsSolverBase* Solver = nullptr;
	Chaos::EThreadingModeTemp PreviousMode = Chaos::EThreadingModeTemp::SingleThread;
};
