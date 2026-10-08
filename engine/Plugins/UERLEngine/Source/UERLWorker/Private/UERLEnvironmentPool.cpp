#include "UERLEnvironmentPool.h"

#include "Components/PrimitiveComponent.h"
#include "Engine/World.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	/**
	 * Make a ground surface pass the Robot's friction through unchanged: Chaos
	 * combines two materials with the larger combine mode, so Multiply against
	 * 1.0 yields exactly the Robot value. Several Slots may share this surface.
	 */
	void NeutralizeGroundFriction(UPrimitiveComponent& Ground)
	{
		UPhysicalMaterial* Current = Ground.GetBodyInstance()->GetSimplePhysicalMaterial();
		if (Current && Current->Friction == 1.0f && Current->StaticFriction == 1.0f
			&& Current->FrictionCombineMode == EFrictionCombineMode::Multiply)
		{
			return;
		}
		UPhysicalMaterial* Neutral = NewObject<UPhysicalMaterial>(&Ground, NAME_None, RF_Transient, Current);
		Neutral->Friction = 1.0f;
		Neutral->StaticFriction = 1.0f;
		Neutral->FrictionCombineMode = EFrictionCombineMode::Multiply;
		Neutral->bOverrideFrictionCombineMode = true;
		Ground.SetPhysMaterialOverride(Neutral);
	}
}

FUERLEnvironmentPool::~FUERLEnvironmentPool()
{
	Destroy();
}

bool FUERLEnvironmentPool::Create(
	UWorld& World,
	int32 NumSlots,
	const TSharedRef<IUERLEnvironmentFactory>& EnvironmentFactory,
	const FUERLProviderConfig& EnvironmentConfig,
	const TSharedRef<IUERLRobotFactory>& RobotFactory,
	const FUERLProviderConfig& RobotConfig,
	const TArray<FUERLFieldDescriptor>& SelectedStateFields,
	const FUERLTerrainConfig& InTerrainConfig,
	FString& OutError)
{
	Destroy();
	if (NumSlots <= 0)
	{
		OutError = TEXT("environment pool requires at least one Slot");
		return false;
	}
	const EUERLEnvironmentCollisionScope CollisionScope = EnvironmentFactory->Describe().CollisionScope;
	if (!CollisionPlan.Compile(NumSlots, CollisionScope, OutError))
	{
		return false;
	}
	TerrainConfig = InTerrainConfig;

	ResetBindings = RobotConfig.ResetBindings;

	Environment = EnvironmentFactory->Create(EnvironmentConfig);
	Robot = RobotFactory->Create(RobotConfig);
	if (!Environment || !Robot)
	{
		OutError = TEXT("provider factory returned a null instance");
		Destroy();
		return false;
	}
	if (!Robot->PrepareState(SelectedStateFields, OutError))
	{
		Destroy();
		return false;
	}
	if (!Environment->CreateSlots(
		World, NumSlots, CollisionPlan, TerrainConfig, Slots, OutError))
	{
		Destroy();
		return false;
	}
	if (Slots.Num() != NumSlots)
	{
		OutError = TEXT("Environment did not create the requested number of stable Slots");
		Destroy();
		return false;
	}
	CachedAllSlotIds.SetNumUninitialized(NumSlots);
	for (int32 Index = 0; Index < Slots.Num(); ++Index)
	{
		CachedAllSlotIds[Index] = Index;
		if (Slots[Index].SlotId != Index)
		{
			OutError = TEXT("Environment returned non-contiguous Slot identities");
			Destroy();
			return false;
		}
		if (!Slots[Index].CollisionProfile.IsValid()
			|| Slots[Index].CollisionProfile.Scope() != CollisionScope)
		{
			OutError = TEXT("Environment returned a Slot without its compiled collision profile");
			Destroy();
			return false;
		}
	}
	if (TerrainConfig.NumLevels > 0)
	{
		ActiveTerrainLevels.Init(0, NumSlots);
		TerrainLevelEventPending.Init(false, NumSlots);
	}
	if (!Robot->SpawnIntoSlots(World, Slots, OutError))
	{
		Destroy();
		return false;
	}

	if (!Environment->BindRobot(*Robot, OutError))
	{
		Destroy();
		return false;
	}
	Episodes.SetNumZeroed(NumSlots);
	bCreated = true;
	return true;
}

void FUERLEnvironmentPool::Destroy()
{
	EndControlWindow();
	for (const auto& Entry : OriginalGroundMaterials)
	{
		if (UPrimitiveComponent* Ground = Entry.Key.Get())
		{
			Ground->SetPhysMaterialOverride(Entry.Value.Get());
		}
	}
	OriginalGroundMaterials.Reset();
	if (Robot)
	{
		Robot->DestroySlots();
		Robot.Reset();
	}
	if (Environment)
	{
		Environment->DestroySlots();
		Environment.Reset();
	}
	Slots.Reset();
	CachedAllSlotIds.Reset();
	Episodes.Reset();
	ResetBindings.Reset();
	TerrainConfig = FUERLTerrainConfig();
	ActiveTerrainLevels.Reset();
	TerrainLevelEventPending.Reset();
	CollisionPlan.Reset();
	bCreated = false;
}

bool FUERLEnvironmentPool::ApplyCommands(const FUERLNamedActionReader& Reader, FString& OutError)
{
	if (!bCreated || !Robot || Reader.NumRows() != Slots.Num())
	{
		OutError = TEXT("cannot apply commands before the Environment Pool is ready");
		return false;
	}

	return Robot->ApplyCommands(Reader, OutError);
}

bool FUERLEnvironmentPool::RequiresCommandsEveryPhysicsFrame() const
{
	return Robot->RequiresCommandsEveryPhysicsFrame();
}

void FUERLEnvironmentPool::BeginControlWindow()
{
	if (bCreated && Environment) { Environment->BeginControlWindow(); }
}

void FUERLEnvironmentPool::AdvancePhysicsFrame(double PhysicsDt)
{
	if (bCreated && Environment) { Environment->AdvancePhysicsFrame(PhysicsDt); }
}

void FUERLEnvironmentPool::EndControlWindow()
{
	if (Environment) { Environment->EndControlWindow(); }
}

void FUERLEnvironmentPool::SamplePhysicsContacts(double SolverStepSeconds)
{
	Robot->SamplePhysicsContacts(SolverStepSeconds);
}

bool FUERLEnvironmentPool::ApplyEvent(FUERLEventBatch& Event, FString& OutError)
{
	if (!bCreated || !Environment || !Robot)
	{
		OutError = TEXT("cannot apply an event before the Environment Pool is ready");
		return false;
	}
	if (Event.SlotIds.Num() != Event.Values.Num() || Event.SlotIds.IsEmpty())
	{
		OutError = TEXT("event Slot/value rows must be non-empty and aligned");
		return false;
	}
	TSet<int32> Seen;
	for (const int32 SlotId : Event.SlotIds)
	{
		if (!Slots.IsValidIndex(SlotId) || Seen.Contains(SlotId))
		{
			OutError = FString::Printf(TEXT("invalid or duplicate event Slot %d"), SlotId);
			return false;
		}
		Seen.Add(SlotId);
	}
	for (const FVector& Value : Event.Values)
	{
		if (Value.ContainsNaN())
		{
			OutError = TEXT("event values must be finite");
			return false;
		}
	}

	switch (Event.Kind)
	{
	case EUERLEventKind::GroundFriction:
	{
		// SharedWorld Slots share one ground component, so the per-Slot value
		// lives on each Slot's Robot bodies and the ground only passes it through.
		TArray<UPrimitiveComponent*> Grounds;
		for (int32 Row = 0; Row < Event.SlotIds.Num(); ++Row)
		{
			const FVector& Value = Event.Values[Row];
			if (Value.X < 0.0 || Value.Y < 0.0 || Value.Z != 0.0)
			{
				OutError = TEXT("ground friction requires non-negative static/dynamic values");
				return false;
			}
			bool bHasGround = false;
			for (const TWeakObjectPtr<UPrimitiveComponent>& Ground : Slots[Event.SlotIds[Row]].GroundComponents)
			{
				UPrimitiveComponent* Component = Ground.Get();
				if (Component && Component->IsRegistered())
				{
					Grounds.AddUnique(Component);
					bHasGround = true;
				}
			}
			if (!bHasGround)
			{
				OutError = FString::Printf(
					TEXT("ground friction Slot %d has no registered ground component"), Event.SlotIds[Row]);
				return false;
			}
		}
		if (!Robot->ApplyEvent(Event, OutError))
		{
			return false;
		}
		for (UPrimitiveComponent* Ground : Grounds)
		{
			const TWeakObjectPtr<UPrimitiveComponent> GroundKey(Ground);
			if (!OriginalGroundMaterials.Contains(GroundKey))
			{
				OriginalGroundMaterials.Add(
					GroundKey, TStrongObjectPtr<UPhysicalMaterial>(Ground->GetPhysicsMaterialOverride()));
			}
			NeutralizeGroundFriction(*Ground);
		}
		return true;
	}
	case EUERLEventKind::TerrainTierParams:
	{
		if (TerrainConfig.NumLevels < 1)
		{
			OutError = TEXT("terrain tier parameters require procedural terrain");
			return false;
		}
		TArray<uint16> AppliedLevels;
		AppliedLevels.Reserve(Event.SlotIds.Num());
		for (int32 Row = 0; Row < Event.SlotIds.Num(); ++Row)
		{
			const float RoughnessScale = static_cast<float>(Event.Values[Row].X);
			if (RoughnessScale < 0.0f || RoughnessScale > 1.0f
				|| Event.Values[Row].Y != 0.0 || Event.Values[Row].Z != 0.0)
			{
				OutError = TEXT("terrain roughness scale must be normalized to [0, 1]");
				return false;
			}
			const int32 Level = FMath::Clamp(
				FMath::RoundToInt(RoughnessScale * static_cast<float>(TerrainConfig.NumLevels - 1)),
				0, TerrainConfig.NumLevels - 1);
			ActiveTerrainLevels[Event.SlotIds[Row]] = static_cast<uint16>(Level);
			TerrainLevelEventPending[Event.SlotIds[Row]] = true;
			AppliedLevels.Add(static_cast<uint16>(Level));
		}
		Event.AppliedTerrainLevels = MoveTemp(AppliedLevels);
		return true;
	}
	case EUERLEventKind::RootPush:
		return Robot->ApplyEvent(Event, OutError);
	default:
		OutError = TEXT("unknown event kind");
		return false;
	}
}

bool FUERLEnvironmentPool::BuildResetBatch(
	const TArray<int32>& SlotIds,
	const TArray<uint16>& TerrainLevels,
	const TArray<float>& ResetValues,
	bool bApplyRobotOverrides,
	FUERLResetBatch& OutReset,
	FString& OutError) const
{
	if (TerrainConfig.NumLevels > 0 && !TerrainLevels.IsEmpty() && TerrainLevels.Num() != Slots.Num())
	{
		OutError = TEXT("terrain level batch must contain one value per Slot");
		return false;
	}
	if (bApplyRobotOverrides && ResetValues.Num() != Slots.Num() * ResetBindings.Num())
	{
		OutError = TEXT("Robot reset value batch must contain one fixed vector per Slot");
		return false;
	}
	if (!bApplyRobotOverrides && !ResetValues.IsEmpty())
	{
		OutError = TEXT("initial Slot placement does not accept Robot reset overrides");
		return false;
	}
	OutReset.Rows.Reset();
	TSet<int32> Seen;
	for (int32 SlotId : SlotIds)
	{
		if (!Slots.IsValidIndex(SlotId) || Seen.Contains(SlotId))
		{
			OutError = FString::Printf(TEXT("invalid or duplicate reset Slot %d"), SlotId);
			return false;
		}
		Seen.Add(SlotId);
		FUERLResetRow& Row = OutReset.Rows.AddDefaulted_GetRef();
		Row.SlotId = SlotId;
		if (bApplyRobotOverrides)
		{
			Row.Values.SetNumUninitialized(ResetBindings.Num());
		}
		if (TerrainConfig.NumLevels > 0)
		{
			Row.TerrainLevel = TerrainLevelEventPending.IsValidIndex(SlotId)
				&& TerrainLevelEventPending[SlotId]
				? ActiveTerrainLevels[SlotId]
				: TerrainLevels.IsEmpty()
				? ActiveTerrainLevels[SlotId]
				: TerrainLevels[SlotId];
			if (Row.TerrainLevel >= TerrainConfig.NumLevels)
			{
				OutError = FString::Printf(TEXT("terrain level %d is out of range for Slot %d"), Row.TerrainLevel, SlotId);
				return false;
			}
			if (!Environment->ResolveGroundFrame(
				SlotId,
				Row.TerrainLevel,
				Row.Origin,
				Row.GroundHeight,
				Row.GroundNormal,
				OutError))
			{
				return false;
			}
		}
		else
		{
			Row.Origin = Slots[SlotId].Origin;
			Row.GroundHeight = Slots[SlotId].GroundHeight;
			Row.GroundNormal = Slots[SlotId].GroundNormal;
		}
		for (int32 BindingIndex = 0; bApplyRobotOverrides && BindingIndex < ResetBindings.Num(); ++BindingIndex)
		{
			const float Value = ResetValues[SlotId * ResetBindings.Num() + BindingIndex];
			if (!FMath::IsFinite(Value))
			{
				OutError = FString::Printf(TEXT("Robot reset value '%s' must be finite"), *ResetBindings[BindingIndex].Name.ToString());
				return false;
			}
			Row.Values[BindingIndex] = Value;
		}
	}
	return true;
}

bool FUERLEnvironmentPool::InitializeSlots(const TArray<uint16>& TerrainLevels, FString& OutError)
{
	return ApplyResetSlots(AllSlotIds(), TerrainLevels, {}, false, false, OutError);
}

bool FUERLEnvironmentPool::ResetSlots(
	const TArray<int32>& SlotIds,
	const TArray<uint16>& TerrainLevels,
	const TArray<float>& ResetValues,
	FString& OutError)
{
	return ApplyResetSlots(SlotIds, TerrainLevels, ResetValues, true, true, OutError);
}

bool FUERLEnvironmentPool::ApplyResetSlots(
	const TArray<int32>& SlotIds,
	const TArray<uint16>& TerrainLevels,
	const TArray<float>& ResetValues,
	bool bApplyRobotOverrides,
	bool bAdvanceEpisode,
	FString& OutError)
{
	if (!bCreated || !Environment || !Robot)
	{
		OutError = TEXT("cannot Reset before the Environment Pool is ready");
		return false;
	}
	for (int32 SlotId : SlotIds)
	{
		if (!Episodes.IsValidIndex(SlotId))
		{
			OutError = FString::Printf(TEXT("invalid reset Slot %d"), SlotId);
			return false;
		}
	}
	if (TerrainConfig.NumLevels > 0 && !TerrainLevels.IsEmpty())
	{
		if (TerrainLevels.Num() != Slots.Num())
		{
			OutError = TEXT("terrain level batch must contain one value per Slot");
			return false;
		}
		for (uint16 TerrainLevel : TerrainLevels)
		{
			if (TerrainLevel >= TerrainConfig.NumLevels)
			{
				OutError = FString::Printf(TEXT("terrain level %d is out of range"), TerrainLevel);
				return false;
			}
		}
	}
	FUERLResetBatch Reset;
	if (!BuildResetBatch(
		SlotIds, TerrainLevels, ResetValues, bApplyRobotOverrides, Reset, OutError))
	{
		return false;
	}
	if (!Environment->ResetSlots(Reset, OutError))
	{
		return false;
	}
	if (!Robot->ResetSlots(Reset, OutError))
	{
		return false;
	}
	// A reset is a single lifecycle transaction.  Keep episode indices and the
	// cached ground frame unchanged when either provider rejects the batch; only
	// commit them after both Environment and Robot have accepted the same rows.
	if (bAdvanceEpisode)
	{
		for (int32 SlotId : SlotIds)
		{
			++Episodes[SlotId];
		}
	}
	for (const FUERLResetRow& Row : Reset.Rows)
	{
		Slots[Row.SlotId].Origin = Row.Origin;
		Slots[Row.SlotId].GroundHeight = Row.GroundHeight;
		Slots[Row.SlotId].GroundNormal = Row.GroundNormal;
		if (TerrainConfig.NumLevels > 0)
		{
			ActiveTerrainLevels[Row.SlotId] = Row.TerrainLevel;
			TerrainLevelEventPending[Row.SlotId] = false;
		}
	}
	return true;
}

bool FUERLEnvironmentPool::CollectState(
	const TArray<int32>& SlotIds,
	FUERLNamedStateWriter& Writer,
	FString& OutError) const
{
	if (Environment) { Environment->CollectState(SlotIds, Writer); }
	if (Robot && !Robot->TryCollectState(SlotIds, Writer, OutError))
	{
		return false;
	}
	OutError.Reset();
	return true;
}

void FUERLEnvironmentPool::ValidateSlots(
	const TArray<int32>& SlotIds,
	TArray<EUERLSlotFaultCode>& OutFaults) const
{
	OutFaults.Init(EUERLSlotFaultCode::None, Slots.Num());
	for (int32 SlotId : SlotIds)
	{
		FString Reason;
		EUERLSlotFaultCode Code = Environment
			? Environment->ValidateSlot(SlotId, Reason)
			: EUERLSlotFaultCode::MissingObject;
		if (Code == EUERLSlotFaultCode::None)
		{
			Code = Robot ? Robot->ValidateSlot(SlotId, Reason) : EUERLSlotFaultCode::MissingObject;
		}
		OutFaults[SlotId] = Code;
	}
}

const TArray<int32>& FUERLEnvironmentPool::AllSlotIds() const
{
	return CachedAllSlotIds;
}
