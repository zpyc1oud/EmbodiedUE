#pragma once

#include "CoreMinimal.h"
#include "UERLBridgeTypes.h"

class FJsonObject;
class FJsonValue;

namespace UERLBatchLayout
{
	enum class EKind : uint8
	{
		InitialState,
		StepAction,
		StepResult,
		ResetRequest,
		ResetResult,
	};

	struct FSegment
	{
		FString Name;
		FString DType;
		TArray<int32> Shape;
		uint32 Offset = 0;
		uint32 ByteLength = 0;
		int32 BatchColumn = INDEX_NONE;
	};

	struct FLayout
	{
		EKind Kind = EKind::InitialState;
		int32 BatchSize = 0;
		TArray<FSegment> Segments;
		uint32 PayloadLength = 0;
		uint64 LayoutId = 0;
		FString LayoutHash;

		const FSegment* Find(const FString& Name) const;
		TSharedRef<FJsonObject> ToJson() const;
	};

	struct FSet
	{
		FLayout InitialState;
		FLayout StepAction;
		FLayout StepResult;
		FLayout ResetRequest;
		FLayout ResetResult;

		const FLayout* Find(EKind Kind) const;
	};

	bool Compile(int32 NumSlots, const FUERLBatchSchema& Schema, FSet& OutLayouts, FString& OutError);
	TSharedRef<FJsonValue> FieldArray(const TArray<FUERLBatchFieldBinding>& Fields);

	bool DecodeActions(const FLayout& Layout, const TArray<uint8>& Payload, FUERLMutableBatchView Actions, FString& OutError);
	bool DecodeStepDecimation(const FLayout& Layout, const TArray<uint8>& Payload, int32& OutDecimation, FString& OutError);
	bool DecodeResetMask(const FLayout& Layout, const TArray<uint8>& Payload, int32 NumSlots,
		TArray<int32>& OutSlots, FString& OutError);
	bool DecodeTerrainLevels(const FLayout& Layout, const TArray<uint8>& Payload, int32 NumSlots,
		TArray<uint16>& OutTerrainLevels, FString& OutError);
	bool DecodeResetValues(const FLayout& Layout, const TArray<uint8>& Payload, int32 NumSlots,
		TArray<float>& OutValues, FString& OutError);
	bool EncodeState(const FLayout& Layout, FUERLMutableBatchView States, FUERLFaultBatchView Faults,
		const TArray<uint64>& Episodes, const TArray<int32>* ResetSlots, TArray<uint8>& OutPayload, FString& OutError);
}
