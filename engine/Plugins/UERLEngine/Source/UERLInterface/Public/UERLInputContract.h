#pragma once

#include "UERLInterfaceTypes.h"

class FJsonObject;

/** Persist input meaning independently of concrete World objects. */
struct FUERLInputSpec
{
	FString Name;
	FString ProviderId;
	int32 ProviderVersion = 1;
	FString ParametersJson = TEXT("{}");
	FString Attachment;
	TArray<FString> SceneBindings;
	FString Sampling = TEXT("completed_window");
	FString MissingData = TEXT("fault");
};

struct FUERLResolvedInput
{
	FUERLInputSpec Spec;
	TArray<FUERLFieldDescriptor> Fields;
};

UERLINTERFACE_API bool ParseUERLInputContract(const TSharedPtr<FJsonObject>& Json,
	TArray<FUERLResolvedInput>& Out, bool bRequireFields, FString& OutError);
UERLINTERFACE_API TSharedRef<FJsonObject> WriteUERLInputContract(TConstArrayView<FUERLResolvedInput> Inputs);
UERLINTERFACE_API bool SameUERLInputField(const FUERLFieldDescriptor& A, const FUERLFieldDescriptor& B);
