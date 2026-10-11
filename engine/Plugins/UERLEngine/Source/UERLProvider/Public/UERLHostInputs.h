#pragma once

#include "UERLInputProvider.h"

/** Resolve effective specifications and check persisted numeric meaning before creating instances. */
UERLPROVIDER_API bool ResolveUERLHostInputs(TArray<FUERLResolvedInput>& Inputs,
	const TArray<FUERLFieldDescriptor>& HostFields, FString& OutError);

/** Share selected-field routing and completed-window publication between Worker and game hosts. */
class UERLPROVIDER_API FUERLHostInputs
{
public:
	bool Configure(const TArray<FUERLResolvedInput>& Inputs, const TArray<FUERLFieldDescriptor>& HostFields,
		const TArray<FUERLFieldDescriptor>& SelectedFields, FString& OutError);
	bool Bind(TArray<FUERLInputSlotBinding> Bindings, FString& OutError);
	bool Publish(int32 SlotId, int64 Sequence, double SolverTime, TConstArrayView<float> HostValues,
		TArray<float>& OutValues, FString& OutError);
	bool Reset(TConstArrayView<int32> SlotIds, FString& OutError) { return Set.Reset(SlotIds, OutError); }
	void Release();
	const TArray<FUERLFieldDescriptor>& SourceFields() const { return Sources; }
	bool IsActive() const { return !Requirements.IsEmpty(); }
private:
	struct FSnapshot { FUERLInputSampleStamp Stamp; TArray<float> Values; };
	struct FRoute { bool bInput = false; int32 Offset = 0; int32 Width = 0; };
	TArray<FUERLResolvedInput> Requirements;
	TArray<FUERLFieldDescriptor> Sources;
	TArray<FRoute> Routes;
	TMap<int32, FSnapshot> Snapshots;
	FUERLCompiledInputSet Set;
};
