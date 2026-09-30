#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "UERLInterfaceTypes.h"

/** Store one deterministic terrain spawn sample in UE world units. */
struct UERLTERRAIN_API FUERLTerrainSpawnSample
{
	/** Store the world-space spawn origin, including a safe terrain reset base. */
	FVector Origin = FVector::ZeroVector;
	/** Store the world-space terrain reset base height in centimetres. */
	double GroundHeight = 0.0;
	/** Store the upward surface normal at the spawn origin. */
	FVector GroundNormal = FVector::UpVector;
};

/** Describe one generated static box in metres before UE unit conversion. */
struct UERLTERRAIN_API FUERLTerrainBoxSpec
{
	/** Identify the Slot that owns this collision geometry. */
	int32 SlotId = INDEX_NONE;
	/** Store the world-space box centre in metres. */
	FVector CenterMeters = FVector::ZeroVector;
	/** Store positive half-extents in metres. */
	FVector ExtentMeters = FVector::ZeroVector;
	/** Store the world-space yaw in radians. */
	double YawRadians = 0.0;

	bool operator==(const FUERLTerrainBoxSpec& Other) const
	{
		return SlotId == Other.SlotId
			&& CenterMeters == Other.CenterMeters
			&& ExtentMeters == Other.ExtentMeters
			&& YawRadians == Other.YawRadians;
	}

	bool operator!=(const FUERLTerrainBoxSpec& Other) const
	{
		return !(*this == Other);
	}
};

/** Describe one generated triangulated heightfield patch in metres. */
struct UERLTERRAIN_API FUERLTerrainMeshSpec
{
	/** Identify the Slot that owns this collision geometry. */
	int32 SlotId = INDEX_NONE;
	/** Store world-space vertex positions in metres. */
	TArray<FVector> VerticesMeters;
	/** Store one upward-oriented normal per vertex. */
	TArray<FVector> Normals;
	/** Store the triangle index buffer. */
	TArray<int32> Triangles;
};

/** Keep deterministic geometry and spawn samples for one terrain tier. */
struct UERLTERRAIN_API FUERLTerrainTierPlan
{
	/** Store the contiguous difficulty level represented by this plan. */
	int32 Level = 0;
	/** Store one spawn sample for each stable environment Slot. */
	TArray<FUERLTerrainSpawnSample> Samples;
	/** Store plane or boxes geometry; heightfields use Meshes instead. */
	TArray<FUERLTerrainBoxSpec> Boxes;
	/** Store heightfield patches; each Slot contributes one patch. */
	TArray<FUERLTerrainMeshSpec> Meshes;
	/** Store merged static collision patches for plane and boxes tiers. */
	TArray<FUERLTerrainMeshSpec> CollisionMeshes;
};

/** Carry one sub-terrain generation request after atlas difficulty resolution. */
struct UERLTERRAIN_API FUERLSubTerrainRequest
{
	/** Difficulty in [0, 1] supplied by the atlas (or authored legacy params). */
	double Difficulty = 0.0;
	double CellSize[2] = { 0.0, 0.0 };
	double BorderWidth = 0.0;
	double PlatformWidth = 0.0;
	uint64 Seed = 0;
	int32 SlotId = INDEX_NONE;
	FVector PatchOriginMeters = FVector::ZeroVector;
	TArray<FVector2D> ResetCenters;
	TSharedPtr<FJsonObject> Params;
};

/** Hold geometry produced by one sub-terrain generator invocation. */
struct UERLTERRAIN_API FUERLTerrainPatch
{
	TArray<FUERLTerrainBoxSpec> Boxes;
	TArray<FUERLTerrainMeshSpec> Meshes;
	TArray<FUERLTerrainMeshSpec> CollisionMeshes;
	TArray<double> ResetHeights;
};
