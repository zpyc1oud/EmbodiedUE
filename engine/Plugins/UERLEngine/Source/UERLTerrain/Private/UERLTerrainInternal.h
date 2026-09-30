#pragma once

#include "UERLTerrainTypes.h"

namespace UERLTerrainInternal
{
	FName PrimitiveId(EUERLTerrainPrimitive Primitive);

	FVector PatchOriginMeters(
		const FUERLTerrainConfig& Config,
		int32 Level,
		const FVector& SlotOriginCm);

	bool MakeRequest(
		const FUERLTerrainConfig& Config,
		const FUERLTerrainTierConfig& Tier,
		int32 SlotId,
		const FVector& PatchOriginMetersValue,
		const TArray<FVector2D>& ResetCenters,
		FUERLSubTerrainRequest& OutRequest,
		FString& OutError);

	bool GeneratePlanePatch(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError);

	bool GenerateHeightfieldPatch(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError);

	bool GenerateBoxesPatch(
		const FUERLSubTerrainRequest& Request,
		FUERLTerrainPatch& OutPatch,
		FString& OutError);

	bool ValidatePlaneParams(const FJsonObject& Params, FString& OutError);
	bool ValidateHeightfieldParams(const FJsonObject& Params, FString& OutError);
	bool ValidateBoxesParams(const FJsonObject& Params, FString& OutError);

	void AppendPatchToPlan(const FUERLTerrainPatch& Patch, FUERLTerrainTierPlan& OutPlan);
}
