#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"
#include "Subsystems/WorldSubsystem.h"
#include "UERLViewportObserver.h"
#include "UERLStepPipeline.generated.h"

class UUERLStepPipeline;
class FUERLWorkerSolverScheduling;

struct FUERLPrePhysicsTickFunction final : public FTickFunction
{
	UUERLStepPipeline* Owner = nullptr;
	virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread,
		const FGraphEventRef& MyCompletionGraphEvent) override;
	virtual FString DiagnosticMessage() override;
};

struct FUERLPostPhysicsTickFunction final : public FTickFunction
{
	UUERLStepPipeline* Owner = nullptr;
	virtual void ExecuteTick(float DeltaTime, ELevelTick TickType, ENamedThreads::Type CurrentThread,
		const FGraphEventRef& MyCompletionGraphEvent) override;
	virtual FString DiagnosticMessage() override;
};

/**
 * Own only physics Tick integration and fixed-step diagnostics for one World.
 *
 * The pipeline does not advance training time by itself; the Worker frame gate
 * and runtime request pump decide when an accepted Step may consume frames.
 */
UCLASS()
class UERLWORKER_API UUERLStepPipeline final : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	UUERLStepPipeline();
	~UUERLStepPipeline();

	/** Report whether this World is eligible for Worker pipeline creation. */
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	/** Bind the pipeline to the World and run startup diagnostics. */
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	/** Unregister physics ticks and release the bound World. */
	virtual void Deinitialize() override;

	/** Report whether this pipeline supports the supplied World type. */
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;
	/** Register physics hooks for the active Worker World. */
	bool ActivateForWorker();
	/** Remove physics hooks from the Worker World. */
	void DeactivateForWorker();

	/** Run the pre-physics request pump for the current frame. */
	void OnPrePhysics();
	/** Run the post-physics completion and diagnostics hook. */
	void OnPostPhysics();
	/** Return whether the pipeline has a valid bound World. */
	bool IsReady() const { return bReady; }
	/** Return whether startup fixed-step checks passed. */
	bool DidStartupGatePass() const { return bGatePassed; }
	/** Return the startup diagnostic when the gate failed. */
	const FString& GetGateFailure() const { return GateFailure; }
	/** Return the World currently bound to this pipeline. */
	UWorld* GetBoundWorld() const { return BoundWorld.Get(); }
	/** Validate that the requested fixed-step delta matches the World. */
	bool ValidateRequestedPhysics(double PhysicsDt, FString& OutError) const;
	/** Capture the solver baseline used by fixed-step diagnostics. */
	bool CaptureSolverBaseline(FString& OutError);

private:
	void RunStartupGateChecks();
	void RegisterPhysicsTicks();
	void UnregisterPhysicsTicks();
	bool ReadSolverFrameTime(int32& OutFrame, double& OutTime) const;
	bool BeginStepMeasurement(FString& OutError);
	bool EndStepMeasurement(FString& OutError);

	UPROPERTY(Transient)
	TObjectPtr<UWorld> BoundWorld = nullptr;

	FUERLPrePhysicsTickFunction PrePhysicsTick;
	FUERLPostPhysicsTickFunction PostPhysicsTick;
	bool bReady = false;
	bool bGatePassed = false;
	bool bTicksRegistered = false;
	FString GateFailure;

	int64 PhysicsFrameCount = 0;
	int64 StepStartPhysicsFrame = 0;
	int32 StepStartSolverFrame = 0;
	double StepStartSolverTime = 0.0;
	int32 LastAckSolverFrame = 0;
	double LastAckSolverTime = 0.0;
	int64 CompletedSteps = 0;

	/** Scoped outer dispatch for a headless Worker; restored at deactivation. */
	TUniquePtr<FUERLWorkerSolverScheduling> SolverScheduling;

	/** Viewport-only observation layer; never constructed in headless sessions. */
	TUniquePtr<FUERLViewportObserver> ViewportObserver;
};
