#include "UERLIsolatedGridEnvironment.h"

#include "UERLSlotCollisionPlan.h"
#include "UERLTerrainGenerator.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace UERLIsolatedGrid
{
	const FName EnvironmentId(TEXT("uerl.environment.slot_isolated"));
	const FName SpacingX(TEXT("environment.spacing_x_m"));
	const FName SpacingY(TEXT("environment.spacing_y_m"));
	const FName Columns(TEXT("environment.columns"));
	const FName OriginX(TEXT("environment.origin_x_m"));
	const FName OriginY(TEXT("environment.origin_y_m"));
}

namespace
{
	constexpr double MetresToCentimetres = 100.0;

	class FIsolatedGridEnvironment final : public IUERLEnvironment
	{
	public:
		explicit FIsolatedGridEnvironment(const FUERLProviderConfig& Config)
			: SpacingXCentimetres(Config.Scalars[UERLIsolatedGrid::SpacingX] * MetresToCentimetres)
			, SpacingYCentimetres(Config.Scalars[UERLIsolatedGrid::SpacingY] * MetresToCentimetres)
			, NumColumns(static_cast<int32>(Config.Scalars[UERLIsolatedGrid::Columns]))
			, OriginXCentimetres(Config.Scalars[UERLIsolatedGrid::OriginX] * MetresToCentimetres)
			, OriginYCentimetres(Config.Scalars[UERLIsolatedGrid::OriginY] * MetresToCentimetres)
		{
		}

		bool CreateSlots(
			UWorld& World,
			int32 NumSlots,
			const FUERLSlotCollisionPlan& CollisionPlan,
			const FUERLTerrainConfig& TerrainConfig,
			TArray<FUERLSlotContext>& OutSlots,
			FString& OutError) override
		{
			DestroySlots();
			OutSlots.Reset();
			if (TerrainConfig.NumLevels == 0)
			{
				OutError = TEXT("slot-isolated Environment requires a procedural terrain configuration");
				return false;
			}
			OutSlots.Reserve(NumSlots);
			for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
			{
				AActor* Anchor = World.SpawnActor<AActor>(
					AActor::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator);
				if (!Anchor)
				{
					OutError = FString::Printf(TEXT("failed to spawn isolated-grid anchor for Slot %d"), SlotId);
					DestroySlots();
					OutSlots.Reset();
					return false;
				}
				Anchors.Add(Anchor);
				const int32 Column = SlotId % NumColumns;
				const int32 Row = SlotId / NumColumns;
				FUERLSlotContext& Slot = OutSlots.AddDefaulted_GetRef();
				Slot.SlotId = SlotId;
				Slot.CollisionProfile = CollisionPlan.Profile(SlotId);
				Slot.Origin = FVector(
					OriginXCentimetres + Column * SpacingXCentimetres,
					OriginYCentimetres + Row * SpacingYCentimetres,
					0.0);
				Slot.GroundHeight = 0.0;
				Slot.GroundNormal = FVector::UpVector;
				Slot.EnvironmentActors.Add(Anchor);
			}
			if (!TerrainGenerator.Generate(World, TerrainConfig, OutSlots, OutError))
			{
				DestroySlots();
				OutSlots.Reset();
				return false;
			}
			for (FUERLSlotContext& Slot : OutSlots)
			{
				FUERLTerrainSpawnSample Sample;
				if (!TerrainGenerator.TryGetSample(0, Slot.SlotId, Sample))
				{
					OutError = FString::Printf(
						TEXT("terrain tier 0 has no spawn sample for Slot %d"), Slot.SlotId);
					DestroySlots();
					OutSlots.Reset();
					return false;
				}
				Slot.Origin = Sample.Origin;
				Slot.GroundHeight = Sample.GroundHeight;
				Slot.GroundNormal = Sample.GroundNormal;
			}
			return true;
		}

		bool ResolveGroundFrame(
			int32 SlotId,
			uint16 TerrainLevel,
			FVector& OutOrigin,
			double& OutGroundHeight,
			FVector& OutGroundNormal,
			FString& OutError) const override
		{
			FUERLTerrainSpawnSample Sample;
			if (!TerrainGenerator.TryGetSample(TerrainLevel, SlotId, Sample))
			{
				OutError = FString::Printf(
					TEXT("terrain level %d has no spawn sample for Slot %d"), TerrainLevel, SlotId);
				return false;
			}
			OutOrigin = Sample.Origin;
			OutGroundHeight = Sample.GroundHeight;
			OutGroundNormal = Sample.GroundNormal;
			return true;
		}

		bool ResetSlots(const FUERLResetBatch&, FString&) override
		{
			return true;
		}

		void CollectState(const TArray<int32>&, FUERLNamedStateWriter&) const override
		{
		}

		EUERLSlotFaultCode ValidateSlot(int32 SlotId, FString&) const override
		{
			return Anchors.IsValidIndex(SlotId) && Anchors[SlotId].IsValid()
				? EUERLSlotFaultCode::None
				: EUERLSlotFaultCode::MissingObject;
		}

		void DestroySlots() override
		{
			TerrainGenerator.Destroy();
			for (const TWeakObjectPtr<AActor>& Anchor : Anchors)
			{
				if (Anchor.IsValid())
				{
					Anchor->Destroy();
				}
			}
			Anchors.Reset();
		}

	private:
		double SpacingXCentimetres;
		double SpacingYCentimetres;
		int32 NumColumns;
		double OriginXCentimetres;
		double OriginYCentimetres;
		TArray<TWeakObjectPtr<AActor>> Anchors;
		FUERLTerrainGenerator TerrainGenerator;
	};

	class FIsolatedGridEnvironmentFactory final : public IUERLEnvironmentFactory
	{
	public:
		FIsolatedGridEnvironmentFactory()
		{
			Descriptor.Id = UERLIsolatedGrid::EnvironmentId;
			Descriptor.Version = 1;
			Descriptor.CollisionScope = EUERLEnvironmentCollisionScope::SlotIsolated;
		}

		const FUERLEnvironmentDescriptor& Describe() const override
		{
			return Descriptor;
		}

		bool ValidateConfig(
			const FUERLProviderConfig& Input,
			FUERLProviderConfig& OutEffective,
			FString& OutError) const override
		{
			const TSet<FName> Allowed = {
				UERLIsolatedGrid::SpacingX, UERLIsolatedGrid::SpacingY, UERLIsolatedGrid::Columns,
				UERLIsolatedGrid::OriginX, UERLIsolatedGrid::OriginY,
			};
			for (const TPair<FName, double>& Pair : Input.Scalars)
			{
				if (!Allowed.Contains(Pair.Key) || !FMath::IsFinite(Pair.Value))
				{
					OutError = FString::Printf(
						TEXT("unknown or non-finite slot-isolated scalar '%s'"), *Pair.Key.ToString());
					return false;
				}
			}
			if (!Input.ResetDistributions.IsEmpty())
			{
				OutError = TEXT("slot-isolated Environment has no reset parameters");
				return false;
			}

			OutEffective = Input;
			OutEffective.Scalars.FindOrAdd(UERLIsolatedGrid::SpacingX, 1.5);
			OutEffective.Scalars.FindOrAdd(UERLIsolatedGrid::SpacingY, 1.5);
			OutEffective.Scalars.FindOrAdd(UERLIsolatedGrid::Columns, 8.0);
			OutEffective.Scalars.FindOrAdd(UERLIsolatedGrid::OriginX, 0.0);
			OutEffective.Scalars.FindOrAdd(UERLIsolatedGrid::OriginY, 0.0);

			const double ColumnValue = OutEffective.Scalars[UERLIsolatedGrid::Columns];
			if (OutEffective.Scalars[UERLIsolatedGrid::SpacingX] <= 0.0
				|| OutEffective.Scalars[UERLIsolatedGrid::SpacingY] <= 0.0
				|| ColumnValue < 1.0
				|| ColumnValue > FUERLSlotCollisionPlan::MaxSlots
				|| ColumnValue != FMath::RoundToDouble(ColumnValue))
			{
				OutError = TEXT("slot-isolated spacing must be positive; columns must be an integer in 1..64");
				return false;
			}
			return true;
		}

		TUniquePtr<IUERLEnvironment> Create(const FUERLProviderConfig& EffectiveConfig) const override
		{
			return MakeUnique<FIsolatedGridEnvironment>(EffectiveConfig);
		}

	private:
		FUERLEnvironmentDescriptor Descriptor;
	};
}

TSharedRef<IUERLEnvironmentFactory> UERLIsolatedGrid::MakeFactory()
{
	return MakeShared<FIsolatedGridEnvironmentFactory>();
}
