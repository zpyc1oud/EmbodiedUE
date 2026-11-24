#pragma once

#include "CoreMinimal.h"
#include "UERLBatchLayout.h"
#include "UERLBatchStaging.h"
#include "UERLBridgeTypes.h"

class FJsonObject;

namespace UERLSocketBridgeCodec
{
	bool CheckKeys(
		const TSharedPtr<FJsonObject>& Object,
		const TArray<FString>& Required,
		const TArray<FString>& Optional,
		FString& OutError);

	bool ReadInt(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Key,
		int32 Min,
		int32 Max,
		int32& OutValue,
		FString& OutError);

	bool IsHex32(const FString& Text);
	bool IsUuidV4(const FGuid& Session);

	bool ParseEvent(
		const TSharedPtr<FJsonObject>& Object,
		int32 NumSlots,
		FUERLEventBatch& Out,
		FString& OutError);

	bool ParseWorkerConfig(
		const TSharedPtr<FJsonObject>& Object,
		FUERLWorkerProjection& Out,
		FString& OutError);

	bool WorkerIdentityHash(
		const FUERLWorkerProjection& Projection,
		FString& OutHash,
		FString& OutError);

	bool ParseFields(
		const TArray<TSharedPtr<FJsonValue>>& Values,
		TArray<FUERLFieldDescriptor>& Out,
		bool bAction,
		FString& OutError);

	bool PackRequestedFields(
		const TArray<FUERLFieldDescriptor>& Requested,
		TArray<FUERLBatchFieldBinding>& Out,
		int32& OutWidth,
		FString& OutError);

	TSharedRef<FJsonValue> DescriptorArray(const TArray<FUERLFieldDescriptor>& Fields);
	TSharedRef<FJsonObject> ProtocolVersionJson();
	TSharedRef<FJsonObject> BuildIdentityJson();
	TSharedRef<FJsonObject> TopologyJson(const FUERLRobotTopology& Topology);
	TSharedRef<FJsonObject> WorkerConfigJson(const FUERLWorkerProjection& Projection);
	TArray<uint8> SerializeJson(const TSharedRef<FJsonObject>& Object);
	FString WorkerErrorCode(int32 Code);
}
