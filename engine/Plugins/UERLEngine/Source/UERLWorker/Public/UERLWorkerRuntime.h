#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"
#include "Templates/UniquePtr.h"
#include "UObject/WeakObjectPtr.h"
#include "UERLBatchBinding.h"
#include "UERLBridgeServer.h"
#include "UERLEnvironmentPool.h"
#include "UERLSafetyMonitor.h"
#include "UERLSessionConfig.h"

class FEvent;
class UUERLFrameGate;
class UUERLStepPipeline;

/** Define the fixed-step defaults used before a Worker Config is negotiated. */
namespace UERLWorkerDefaults
{
	constexpr double PhysicsDt = 1.0 / 120.0;
	constexpr int32 Decimation = 2;
}

/** Identify the Worker-domain lifecycle phase visible to the runtime owner. */
enum class EUERLWorkerPhase : uint8
{
	Booting,
	WaitingInitialize,
	Prepared,
	StabilizingInitialize,
	WaitingReady,
	Idle,
	ActiveStep,
	ShuttingDown,
	Failed,
};

/** Process-level owner of Worker-domain lifecycle; implements the Transport seam. */
class UERLWORKER_API FUERLWorkerRuntime final : public IBridgeRequestHandler
{
public:
	using FSessionEnded = TFunction<void(EUERLSessionEndReason)>;

	/** Return the process-wide Worker runtime instance. */
	static FUERLWorkerRuntime& Get();

	FUERLWorkerRuntime(const FUERLWorkerRuntime&) = delete;
	FUERLWorkerRuntime& operator=(const FUERLWorkerRuntime&) = delete;

	/**
	 * Activate the Bridge listener and prepare the Worker lifecycle.
	 *
	 * Repeated activation is idempotent while the runtime is already active.
	 * The callback runs once when the session ends.
	 *
	 * @param Config Supply process/session ownership and Bridge settings.
	 * @param OnSessionEnded Receive the terminal session reason.
	 * @return true when the runtime is active or was already active.
	 */
	bool Activate(const FUERLWorkerLaunchConfig& Config, FSessionEnded OnSessionEnded);
	/** Stop the Bridge, close the frame gate, and release Worker resources. */
	void Deactivate();
	/** Return whether the Worker runtime owns an active session. */
	bool IsActive() const { return bActive; }
	/** Return the launch configuration used by the active runtime. */
	const FUERLWorkerLaunchConfig& GetConfig() const { return LaunchConfig; }

	/** Return the current Worker lifecycle phase. */
	EUERLWorkerPhase GetPhase() const { return Phase.Load(); }
	/** Return the negotiated fixed physics delta time. */
	double GetPhysicsDt() const { return ActiveProjection.PhysicsDt; }
	/** Return the negotiated minimum physics frames per accepted Step. */
	int32 GetDecimationMin() const { return ActiveProjection.DecimationMin; }
	/** Return the negotiated maximum physics frames per accepted Step. */
	int32 GetDecimationMax() const { return ActiveProjection.DecimationMax; }
	/** Return the active physics frames for the current Step. */
	int32 GetActiveStepDecimation() const { return ActiveStepDecimation; }
	/** Return the number of physics frames remaining in the active Step. */
	int32 GetRemainingFrames() const { return RemainingFrames; }

	/** Bind the one Worker World step pipeline, rejecting a second World. */
	bool BindPipeline(UUERLStepPipeline* Pipeline);
	/** Unbind the Worker World step pipeline when it is destroyed. */
	void UnbindPipeline(UUERLStepPipeline* Pipeline);
	/** Bind the one Worker frame gate used to control training time. */
	bool BindFrameGate(UUERLFrameGate* InFrameGate);
	/** Unbind the Worker frame gate when it is destroyed. */
	void UnbindFrameGate(UUERLFrameGate* InFrameGate);

	/** Implement the Transport request seam; call only from the Transport I/O thread. */
	virtual void SubmitAndWait(FUERLBridgeRequest& InOutRequest) override;
	/** Mark the session disconnected and wake any pending request. */
	virtual void NotifyBridgeDisconnected() override;
	/** Complete the graceful session callback after Shutdown is acknowledged. */
	virtual void NotifyBridgeShutdownComplete() override;

	/** Take the pending request from the Transport thread on the Game Thread. */
	bool TryTakeRequest(FUERLBridgeRequest& OutRequest);
	/** Wait for a pending request or the supplied timeout. */
	void WaitForRequest(uint32 TimeoutMs);
	/** Execute one validated request on the Game Thread and report gate actions. */
	void ProcessRequestGameThread(FUERLBridgeRequest& Request, bool& bOutReleaseFrame, bool& bOutExit);
	/** Fail a pending request before it reaches the domain pipeline. */
	void FailPendingRequest(int32 ErrorCode, const FString& Message);
	/** Stop accepting work after the Bridge disconnects. */
	void StopForBridgeDisconnect();
	/** Stop the Worker after an unrecoverable domain failure. */
	bool StopForWorkerFailure();
	/** Return whether the Bridge has disconnected from the Worker. */
	bool IsBridgeDisconnected() const { return bBridgeDisconnected.Load(); }

	/** Apply the active command batch before the next physics frame. */
	bool ApplyActiveCommands();
	bool ShouldApplyActiveCommands() const;
	/** Advance Environment-owned targets once before the next solver frame. */
	void AdvanceEnvironmentPhysicsFrame();
	/** Start timing the Engine/Chaos portion of the current physics frame. */
	void BeginPhysicsFrameTiming();
	/** Finish timing the Engine/Chaos portion of the current physics frame. */
	void EndPhysicsFrameTiming();
	/** Complete one fixed physics frame and return frames still required. */
	int32 CompletePhysicsFrame();
	/** Finish the active Step and publish its result to the Transport thread. */
	void FinishActiveStep();
	/** Fail the active Step and publish a Worker error to the Transport thread. */
	void FailActiveStep(const FString& Message);
	/** Publish Initialize after one post-spawn Chaos stabilization frame. */
	void FinishInitializeStabilization();

private:
	FUERLWorkerRuntime() = default;
	~FUERLWorkerRuntime();

	void CompleteRequest(const FUERLBridgeRequest& Response);
	void PrepareInitialize(FUERLBridgeRequest& Request);
	void CommitInitialize(FUERLBridgeRequest& Request);
	void AbortInitialize(FUERLBridgeRequest& Request);
	void ExecuteReady(FUERLBridgeRequest& Request);
	void ExecuteEvent(FUERLBridgeRequest& Request);
	void ExecuteReset(FUERLBridgeRequest& Request);
	void ExecuteShutdown(FUERLBridgeRequest& Request);
	void BeginStep(FUERLBridgeRequest& Request);
	bool RequirePhase(EUERLWorkerPhase Expected, const TCHAR* Operation, FUERLBridgeRequest& Request);
	bool ValidateStateBatch(
		const FUERLBridgeRequest& Request,
		const FUERLWorkerProjection& Projection,
		FString& OutError) const;
	void FailWorker(
		const FString& Message,
		FUERLBridgeRequest* Request = nullptr,
		int32 ErrorCode = UERLBridgeError::WorkerFatal);
	void ClearPreparedState();
	void CloseFrameGate();
	void DestroyTrainingResources();
	void NotifySessionEnded(EUERLSessionEndReason Reason);

	FUERLWorkerLaunchConfig LaunchConfig;
	FUERLWorkerProjection ActiveProjection;
	bool bActive = false;
	TAtomic<EUERLWorkerPhase> Phase{ EUERLWorkerPhase::Booting };
	TAtomic<bool> bBridgeDisconnected{ false };
	TAtomic<bool> bFatalResponsePending{ false };
	FCriticalSection SessionEndMutex;
	FSessionEnded SessionEndedCallback;
	bool bSessionEndNotified = false;

	FUERLBridgeServer BridgeServer;
	TWeakObjectPtr<UUERLFrameGate> FrameGate;
	TWeakObjectPtr<UUERLStepPipeline> StepPipeline;
	TUniquePtr<FUERLEnvironmentPool> Pool;
	FUERLBatchBinding BatchBinding;
	FUERLSafetyMonitor Safety;
	TArray<EUERLSlotFaultCode> ProviderFaults;

	TSharedPtr<IUERLEnvironmentFactory> PreparedEnvironmentFactory;
	TSharedPtr<IUERLRobotFactory> PreparedRobotFactory;
	FUERLProviderConfig EffectiveEnvironmentConfig;
	FUERLProviderConfig EffectiveRobotConfig;
	TArray<FUERLFieldDescriptor> AvailableActionFields;
	TArray<FUERLFieldDescriptor> AvailableStateFields;

	int32 RemainingFrames = 0;
	int32 ActiveStepDecimation = 1;
	double ActivePhysicsFrameStartSeconds = 0.0;
	FUERLBridgeRequest ActiveInitializeRequest;
	FUERLBridgeRequest ActiveStepRequest;

	FCriticalSection RequestMutex;
	FUERLBridgeRequest PendingRequest;
	TAtomic<bool> bRequestPending{ false };
	FEvent* RequestEvent = nullptr;
	FEvent* DoneEvent = nullptr;
};
