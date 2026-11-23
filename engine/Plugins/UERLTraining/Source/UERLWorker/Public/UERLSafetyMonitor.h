#pragma once

#include "CoreMinimal.h"
#include "UERLBridgeTypes.h"
#include "UERLProvider.h"

/**
 * Validate State batches and retain one finite fallback per Slot.
 *
 * Transition faults restore the last valid row when possible; an initial or
 * unrecoverable non-finite row fails the Worker transaction.
 */
class UERLWORKER_API FUERLSafetyMonitor
{
public:
	/** Allocate fallback storage for the configured Slot and State dimensions. */
	void Initialize(int32 NumSlots, int32 StateWidth);
	/** Clear fallback storage and return to the uninitialized state. */
	void Reset();

	/**
	 * Validate the first State batch and establish every Slot fallback.
	 *
	 * @return false when a provider fault or non-finite initial row prevents initialization.
	 */
	bool AcceptInitial(
		FUERLMutableBatchView States,
		const TArray<EUERLSlotFaultCode>& ProviderFaults,
		FUERLFaultBatchView Faults,
		FString& OutError);

	/**
	 * Validate a transition and restore the last finite row for recoverable faults.
	 *
	 * @return false when a faulted Slot has no finite fallback.
	 */
	bool SanitizeTransition(
		FUERLMutableBatchView States,
		const TArray<EUERLSlotFaultCode>& ProviderFaults,
		FUERLFaultBatchView Faults,
		FString& OutError);

	/**
	 * Validate reset output and replace the fallback for each reset Slot.
	 *
	 * @return false when a selected Slot remains faulted or non-finite after reset.
	 */
	bool AcceptReset(
		FUERLMutableBatchView States,
		const TArray<int32>& ResetSlots,
		const TArray<EUERLSlotFaultCode>& ProviderFaults,
		FUERLFaultBatchView Faults,
		FString& OutError);

private:
	bool IsFiniteRow(FUERLMutableBatchView States, int32 Row) const;
	void SaveRow(FUERLMutableBatchView States, int32 Row);
	void RestoreRow(FUERLMutableBatchView States, int32 Row) const;

	int32 NumSlots = 0;
	int32 StateWidth = 0;
	TArray<float> LastValidStates;
	TArray<uint8> HasLastValid;
};
