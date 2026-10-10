#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS
class UWorld;
class FUERLBatchBinding;
struct FUERLBridgeRequest;
struct FUERLSlotContext;
// A bounded one-Slot physical oracle. Called after collection and before the
// same request is released to Transport. This is absent from shipping builds.
bool CaptureUERLPhysicalFeedback(UWorld* World, const FUERLBridgeRequest& Request,
    const FUERLBatchBinding& Binding, const FUERLSlotContext* Slot, FString& OutError);
#endif
