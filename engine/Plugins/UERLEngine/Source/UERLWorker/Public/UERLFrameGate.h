#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineCustomTimeStep.h"
#include "UERLFrameGate.generated.h"

/** Request-driven CustomTimeStep that keeps training time closed between Steps. */
UCLASS()
class UERLWORKER_API UUERLFrameGate final : public UEngineCustomTimeStep
{
	GENERATED_BODY()

public:
	/** Install the request-driven custom time step on the supplied engine. */
	virtual bool Initialize(UEngine* InEngine) override;
	/** Remove the custom time step and close further training frames. */
	virtual void Shutdown(UEngine* InEngine) override;
	/** Advance application time only while the Worker has an open frame gate. */
	virtual bool UpdateTimeStep(UEngine* InEngine) override;
	/** Return the engine synchronization state exposed to UE. */
	virtual ECustomTimeStepSynchronizationState GetSynchronizationState() const override;
	/** Return the display name used by UE diagnostics. */
	virtual FString GetDisplayName() const override;
	/** Close the gate so idle engine ticks cannot advance training time. */
	void Close();
	/** Return whether the gate is closed to training time. */
	bool IsClosed() const { return bClosed; }

private:
	void AdvanceAppTimeFixed(double PhysicsDt);
	ECustomTimeStepSynchronizationState SyncState = ECustomTimeStepSynchronizationState::Closed;
	bool bClosed = true;
};
