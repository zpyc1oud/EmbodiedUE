#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UERLPolicyComponent.h"
#include "UERLPolicyComponentTestEvents.generated.h"

/** Records dynamic component events for the policy component integration tests. */
UCLASS()
class UUERLPolicyComponentTestEventRecorder : public UObject
{
	GENERATED_BODY()

public:
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

	int32 FaultCount = 0;

	UFUNCTION()
	void OnFault(const FString& Reason)
	{
		++FaultCount;
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
