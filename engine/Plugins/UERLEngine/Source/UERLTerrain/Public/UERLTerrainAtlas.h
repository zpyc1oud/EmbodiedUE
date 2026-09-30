#pragma once

#include "CoreMinimal.h"
#include "UERLTerrainTypes.h"

/**
 * Difficulty interpolation + row/column grid layout, independent of primitives.
 *
 * Difficulty(Level) uses the Isaac Lab row formula Level / max(1, NumLevels-1).
 *
 * Geometry authority for existing configs remains the authored per-tier params
 * (including boxes params.difficulty and heightfield noise_range ladders). The
 * PhantomX discrete curriculum uses a non-linear difficulty ladder that is not
 * Level/(NumLevels-1); BuildPlan preserves that authored value for bit parity.
 */
class UERLTERRAIN_API FUERLTerrainAtlas
{
public:
	/** Resolve structural difficulty in [0, 1] for a contiguous level. */
	static double Difficulty(int32 Level, int32 NumLevels);

	/** World-space origin in metres for one (level, column) cell on the atlas grid. */
	static bool ResolveOrigin(
		const FUERLTerrainConfig& Config,
		int32 Level,
		int32 Column,
		FVector& OutOriginMeters,
		FString& OutError);

	/**
	 * Build deterministic geometry plans without touching a World.
	 * Slot origins are UE world centimetres.
	 */
	static bool BuildPlan(
		const FUERLTerrainConfig& Config,
		const TArray<FVector>& SlotOrigins,
		TArray<FUERLTerrainTierPlan>& OutPlans,
		FString& OutError);

	/** Build one shared geometry region per tier with one spawn sample per Slot. */
	static bool BuildSharedPlan(
		const FUERLTerrainConfig& Config,
		const TArray<FVector>& SlotOrigins,
		TArray<FUERLTerrainTierPlan>& OutPlans,
		FString& OutError);
};
