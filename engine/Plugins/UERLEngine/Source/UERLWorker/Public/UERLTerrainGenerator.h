#pragma once

#include "CoreMinimal.h"
#include "UERLProvider.h"
#include "UERLTerrainAtlas.h"
#include "UERLTerrainTypes.h"

class AActor;
class UWorld;

/** Generate and own all deterministic terrain geometry for one Worker session.
 *
 * Geometry planning lives in UERLTerrain (atlas + registered sub-generators).
 * This Worker type owns the world-spawned atlas actors and reset samples.
 */
class UERLWORKER_API FUERLTerrainGenerator
{
public:
	/**
	 * Build deterministic geometry plans without touching a World.
	 *
	 * Slot origins are supplied in UE world centimetres. The returned plans keep
	 * all geometry internal to UE; no plan data is ever sent through the Bridge.
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

	/** Generate all tier plans and register their Chaos collision geometry. */
	bool Generate(
		UWorld& World,
		const FUERLTerrainConfig& Config,
		TArray<FUERLSlotContext>& SlotContexts,
		FString& OutError);
	/** Generate one shared terrain atlas whose collision belongs to the World. */
	bool GenerateShared(
		UWorld& World,
		const FUERLTerrainConfig& Config,
		TArray<FUERLSlotContext>& SlotContexts,
		FString& OutError);

	/** Destroy all generated terrain atlas actors and clear deterministic plans. */
	void Destroy();

	/** Return the pre-generated sample for one terrain level and Slot. */
	bool TryGetSample(int32 Level, int32 SlotId, FUERLTerrainSpawnSample& OutSample) const;

	/** Return generated tier plans for reset-time lookup and diagnostics.
	 *
	 * Runtime Generate compacts geometry after registration; reset samples remain
	 * available while the transient build meshes are released.
	 */
	const TArray<FUERLTerrainTierPlan>& TierPlans() const { return Plans; }

private:
	bool SpawnPlans(
		UWorld& World,
		const TArray<FUERLTerrainTierPlan>& InPlans,
		TArray<FUERLSlotContext>& SlotContexts,
		FString& OutError);
	bool SpawnSharedPlans(
		UWorld& World,
		const TArray<FUERLTerrainTierPlan>& InPlans,
		TArray<FUERLSlotContext>& SlotContexts,
		FString& OutError);

	TArray<TWeakObjectPtr<AActor>> TerrainActors;
	TArray<FUERLTerrainTierPlan> Plans;
	/** Copied from the active Generate/GenerateShared config for spawn-time collision. */
	bool bPhysicsCollision = true;
};
