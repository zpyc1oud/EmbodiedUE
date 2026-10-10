#pragma once

#include "CoreMinimal.h"
#include "UERLBatchBinding.h"
#include "UERLProvider.h"
#include "UObject/StrongObjectPtr.h"

class UWorld;
class UPrimitiveComponent;
class UPhysicalMaterial;

/** Owns one Environment instance, one Robot instance, and all stable Slot identities. */
class UERLWORKER_API FUERLEnvironmentPool
{
public:
	~FUERLEnvironmentPool();

	/**
	 * Create one Environment and Robot set for the requested Slot count.
	 *
	 * The pool owns the created providers and consumes reset values supplied by Python.
	 *
	 * @param World Supply the World that owns provider objects.
	 * @param NumSlots Supply the stable Slot count for this run.
	 * @param EnvironmentFactory Create the selected Environment.
	 * @param EnvironmentConfig Supply validated Environment settings.
	 * @param RobotFactory Create the selected Robot.
	 * @param RobotConfig Supply validated Robot settings.
	 * @param SelectedStateFields Supply the fields selected after schema binding;
	 *        passed to the Robot before its Slots are spawned.
	 * @param TerrainConfig Supply the optional pre-generated terrain grid; empty when absent.
	 * @param OutError Receive a diagnostic when creation fails.
	 * @return true when all providers and Slots are ready.
	 */
	bool Create(
		UWorld& World,
		int32 NumSlots,
		const TSharedRef<IUERLEnvironmentFactory>& EnvironmentFactory,
		const FUERLProviderConfig& EnvironmentConfig,
		const TSharedRef<IUERLRobotFactory>& RobotFactory,
		const FUERLProviderConfig& RobotConfig,
		const TArray<FUERLFieldDescriptor>& SelectedStateFields,
		const FUERLTerrainConfig& TerrainConfig,
		FString& OutError);

	/** Destroy providers and clear all stable Slot and episode state. */
	void Destroy();
	/** Return whether the Environment and Robot providers are created. */
	bool IsCreated() const { return bCreated; }
	/** Return the number of stable Slots owned by the pool. */
	int32 Num() const { return Slots.Num(); }

	/** Apply one decoded physical command batch to all active Slots. */
	bool ApplyCommands(const FUERLNamedActionReader& Reader, FString& OutError);
	/** Place all Slots at their initial terrain levels without applying Robot reset overrides. */
	bool InitializeSlots(const TArray<uint16>& TerrainLevels, FString& OutError);
	/** Return the active Robot's physical command retention requirement. */
	bool RequiresCommandsEveryPhysicsFrame() const;
	/** Sample the active Robot's terminal support and latest solver-step force. */
	void SamplePhysicsContacts(double SolverStepSeconds);
	/** Apply one validated scene/physics event to selected Slots. */
	bool ApplyEvent(FUERLEventBatch& Event, FString& OutError);
	/** Reset selected Slots to requested terrain levels and apply Python-owned Robot overrides. */
	bool ResetSlots(
		const TArray<int32>& SlotIds,
		const TArray<uint16>& TerrainLevels,
		const TArray<float>& ResetValues,
		FString& OutError);
	/** Collect selected Environment and Robot State fields into the writer. */
	bool CollectState(
		const TArray<int32>& SlotIds,
		FUERLNamedStateWriter& Writer,
		FString& OutError) const;
	/** Validate selected Slots and return a full aligned recoverable fault batch. */
	void ValidateSlots(const TArray<int32>& SlotIds, TArray<EUERLSlotFaultCode>& OutFaults) const;

	/** Return the cached stable Slot identifiers in ascending order. */
	const TArray<int32>& AllSlotIds() const;
	/** Return the current per-Slot episode indices without transferring ownership. */
	const TArray<uint64>& EpisodeIndices() const { return Episodes; }

#if WITH_DEV_AUTOMATION_TESTS
	// Independent test capture uses the Environment-owned frame, never the
	// moving robot Actor transform as a substitute for the Slot origin.
	const FUERLSlotContext* PhysicalTestSlot(int32 SlotId) const
	{
		return Slots.IsValidIndex(SlotId) ? &Slots[SlotId] : nullptr;
	}
#endif

private:
	bool BuildResetBatch(
		const TArray<int32>& SlotIds,
		const TArray<uint16>& TerrainLevels,
		const TArray<float>& ResetValues,
		bool bApplyRobotOverrides,
		FUERLResetBatch& OutReset,
		FString& OutError) const;
	bool ApplyResetSlots(
		const TArray<int32>& SlotIds,
		const TArray<uint16>& TerrainLevels,
		const TArray<float>& ResetValues,
		bool bApplyRobotOverrides,
		bool bAdvanceEpisode,
		FString& OutError);

	// Keep original overrides alive while transient training materials replace them.
	TMap<TWeakObjectPtr<UPrimitiveComponent>, TStrongObjectPtr<UPhysicalMaterial>> OriginalGroundMaterials;
	TUniquePtr<IUERLEnvironment> Environment;
	TUniquePtr<IUERLRobot> Robot;
	TArray<FUERLSlotContext> Slots;
	TArray<int32> CachedAllSlotIds;
	TArray<uint64> Episodes;
	TArray<FUERLResetBinding> ResetBindings;
	FUERLTerrainConfig TerrainConfig;
	FUERLSlotCollisionPlan CollisionPlan;
	TArray<uint16> ActiveTerrainLevels;
	TArray<bool> TerrainLevelEventPending;
	bool bCreated = false;
};
