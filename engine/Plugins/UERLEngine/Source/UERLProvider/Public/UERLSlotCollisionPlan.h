#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"

class UPrimitiveComponent;
struct FBodyInstance;

/** Select whether Environment collision geometry belongs to one Slot or the whole World. */
enum class EUERLEnvironmentCollisionScope : uint8
{
	SlotIsolated,
	SharedWorld,
};

/** Apply one compiled Slot's simulation/query collision profile. */
struct UERLPROVIDER_API FUERLSlotCollisionProfile
{
public:
	FUERLSlotCollisionProfile() = default;

	/** Return whether this profile was compiled for a valid Slot. */
	bool IsValid() const;
	/** Return the Environment collision scope compiled for this Slot. */
	EUERLEnvironmentCollisionScope Scope() const;
	/** Return the unique collision/query channel assigned to an isolated Slot. */
	ECollisionChannel Channel() const;
	/** Return the Chaos collision group assigned to a shared-world Robot. */
	int32 CollisionGroup() const;
	/** Configure a Robot primitive for this Slot. */
	void ApplyRobot(UPrimitiveComponent& Component) const;
	/** Configure an Environment-owned primitive for this isolated Slot. */
	void ApplySlotEnvironment(UPrimitiveComponent& Component) const;
	/** Apply the shared-world group to one initialized Robot physics body. */
	bool ApplyRobotBody(FBodyInstance& Body) const;

private:
	FUERLSlotCollisionProfile(
		EUERLEnvironmentCollisionScope InScope,
		uint8 InChannelIndex,
		int32 InCollisionGroup)
		: CollisionScope(InScope)
		, ChannelIndex(InChannelIndex)
		, CollisionGroupValue(InCollisionGroup)
	{
	}

	EUERLEnvironmentCollisionScope CollisionScope = EUERLEnvironmentCollisionScope::SlotIsolated;
	uint8 ChannelIndex = MAX_uint8;
	int32 CollisionGroupValue = INDEX_NONE;

	friend class FUERLSlotCollisionPlan;
};

/** Compile the fixed one-channel-per-Slot collision plan for one Session. */
class UERLPROVIDER_API FUERLSlotCollisionPlan
{
public:
	/** UE 5.8 exposes exactly 64 serialized collision channels. */
	static constexpr int32 MaxSlots = static_cast<int32>(ECC_GameTraceChannel50) + 1;
	/** The wire protocol carries at most 65,536 stable Slot rows. */
	static constexpr int32 MaxSharedWorldSlots = 65536;

	/** Replace the current plan with NumSlots profiles for one Environment collision scope. */
	bool Compile(int32 NumSlots, EUERLEnvironmentCollisionScope Scope, FString& OutError);
	/** Clear all compiled profiles. */
	void Reset();
	/** Return whether at least one Slot profile is compiled. */
	bool IsCompiled() const { return !Profiles.IsEmpty(); }
	/** Return one compiled Slot profile. */
	const FUERLSlotCollisionProfile& Profile(int32 SlotId) const;

private:
	TArray<FUERLSlotCollisionProfile> Profiles;
};
