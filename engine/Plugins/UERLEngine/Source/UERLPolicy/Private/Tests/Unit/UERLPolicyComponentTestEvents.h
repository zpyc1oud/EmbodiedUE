#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UERLPolicyComponent.h"
#include "UERLPolicyTraceRecorder.h"
#include "UERLPolicyComponentTestEvents.generated.h"

enum class EUERLPolicyTestFrameCallbackMode : uint8
{
	Stop,
	SoftReset,
	SoftResetAndMarkBoundary,
	RecordOnly,
};

/** Records dynamic component events for the policy component integration tests. */
UCLASS()
class UUERLPolicyComponentTestEventRecorder : public UObject
{
	GENERATED_BODY()

public:
	int32 ControlFrameCount = 0;
	FUERLPolicyControlFrameSnapshot LastControlFrame;
	TArray<FUERLPolicyControlFrameSnapshot> ControlFrames;
	EUERLPolicyTestFrameCallbackMode ControlFrameCallbackMode = EUERLPolicyTestFrameCallbackMode::RecordOnly;
	TWeakObjectPtr<UUERLPolicyComponent> ControlFrameCallbackComponent;
	TWeakObjectPtr<UUERLPolicyTraceRecorder> ControlFrameTraceRecorder;
	int32 ControlFrameEpisodeIndex = 1;
	FString ControlFrameBoundaryReason = TEXT("callback_soft_reset");
	bool bControlFrameCallbackSucceeded = false;

	UFUNCTION()
	void OnControlFrameCompleted(const FUERLPolicyControlFrameSnapshot& Frame)
	{
		++ControlFrameCount;
		LastControlFrame = Frame;
		ControlFrames.Add(Frame);
		if (UUERLPolicyComponent* Component = ControlFrameCallbackComponent.Get())
		{
			if (ControlFrameCallbackMode == EUERLPolicyTestFrameCallbackMode::Stop)
			{
				Component->StopPolicy();
				bControlFrameCallbackSucceeded = !Component->IsRunning();
			}
			else if (ControlFrameCallbackMode == EUERLPolicyTestFrameCallbackMode::SoftReset)
			{
				bControlFrameCallbackSucceeded = Component->SoftReset();
			}
			else if (ControlFrameCallbackMode == EUERLPolicyTestFrameCallbackMode::SoftResetAndMarkBoundary)
			{
				bControlFrameCallbackSucceeded = Component->SoftReset();
				if (bControlFrameCallbackSucceeded)
				{
					if (UUERLPolicyTraceRecorder* Trace = ControlFrameTraceRecorder.Get())
					{
						bControlFrameCallbackSucceeded = Trace->MarkEpisodeBoundary(
							ControlFrameEpisodeIndex, ControlFrameBoundaryReason);
					}
					else
					{
						bControlFrameCallbackSucceeded = false;
					}
				}
				ControlFrameCallbackMode = EUERLPolicyTestFrameCallbackMode::RecordOnly;
			}
		}
	}

	int32 OverrunCount = 0;
	float LastGameSeconds = 0.0f;
	float LastPhysicsSeconds = 0.0f;
	float LastObservationSeconds = 0.0f;

	UFUNCTION()
	void OnOverrun(float GameSeconds, float PhysicsSeconds, float ObservationSeconds)
	{
		++OverrunCount;
		LastGameSeconds = GameSeconds;
		LastPhysicsSeconds = PhysicsSeconds;
		LastObservationSeconds = ObservationSeconds;
	}

	int32 StaleCount = 0;
	FName LastChannel = NAME_None;
	float LastStaleSeconds = 0.0f;

	UFUNCTION()
	void OnStale(FName Channel, float StaleSeconds)
	{
		++StaleCount;
		LastChannel = Channel;
		LastStaleSeconds = StaleSeconds;
	}

	TWeakObjectPtr<UUERLPolicyComponent> StaleFallbackComponent;

	UFUNCTION()
	void OnStaleStopPolicy(FName Channel, float StaleSeconds)
	{
		OnStale(Channel, StaleSeconds);
		if (UUERLPolicyComponent* Component = StaleFallbackComponent.Get())
		{
			Component->StopPolicy();
		}
	}

	int32 FaultCount = 0;
	FString LastFaultReason;

	UFUNCTION()
	void OnFault(const FString& Reason)
	{
		++FaultCount;
		LastFaultReason = Reason;
	}

	TWeakObjectPtr<UUERLPolicyComponent> FaultFallbackComponent;

	UFUNCTION()
	void OnFaultStopPolicy(const FString& Reason)
	{
		OnFault(Reason);
		if (UUERLPolicyComponent* Component = FaultFallbackComponent.Get())
		{
			Component->StopPolicy();
		}
	}

	int32 MismatchCount = 0;
	FString MismatchReport;

	UFUNCTION()
	void OnMismatch(const FString& Report)
	{
		++MismatchCount;
		MismatchReport = Report;
	}

	/** 0 stops the policy inside the callback; 1 applies a SoftReset. */
	int32 CallbackMode = 0;
	TWeakObjectPtr<UUERLPolicyComponent> CallbackComponent;

	UFUNCTION()
	void OnOverrunStopOrReset(float GameSeconds, float PhysicsSeconds, float ObservationSeconds)
	{
		++OverrunCount;
		if (UUERLPolicyComponent* Component = CallbackComponent.Get())
		{
			if (CallbackMode == 0)
			{
				Component->StopPolicy();
			}
			else
			{
				Component->SoftReset();
			}
		}
	}
};
