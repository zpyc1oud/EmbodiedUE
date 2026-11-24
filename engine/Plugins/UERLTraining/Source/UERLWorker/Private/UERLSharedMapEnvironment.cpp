#include "UERLSharedMapEnvironment.h"

#include "Components/PrimitiveComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "PhysicsEngine/BodyInstance.h"
#include "UERLTerrainGenerator.h"

namespace UERLSharedMap
{
	const FName EnvironmentId(TEXT("uerl.environment.shared_world"));
	const FName SpacingX(TEXT("environment.spacing_x_m"));
	const FName SpacingY(TEXT("environment.spacing_y_m"));
	const FName Columns(TEXT("environment.columns"));
	const FName OriginX(TEXT("environment.origin_x_m"));
	const FName OriginY(TEXT("environment.origin_y_m"));
	const FName TraceStartZ(TEXT("environment.trace_start_z_m"));
	const FName TraceDepth(TEXT("environment.trace_depth_m"));
}

namespace
{
	constexpr double SharedMapMetersToCentimeters = 100.0;

	bool FindAuthoredWorldStaticCollisionInAtlas(
		UWorld& World,
		const FUERLTerrainConfig& TerrainConfig,
		FString& OutOwner)
	{
		const double PatchSizeX = (TerrainConfig.CellSize[0] + 2.0 * TerrainConfig.BorderWidth)
			* SharedMapMetersToCentimeters;
		const double PatchSizeY = (TerrainConfig.CellSize[1] + 2.0 * TerrainConfig.BorderWidth)
			* SharedMapMetersToCentimeters;
		const double MinX = -PatchSizeX * 0.5;
		const double MaxX = (TerrainConfig.NumLevels - 0.5) * PatchSizeX;
		const double MinY = -PatchSizeY * 0.5;
		const double MaxY = PatchSizeY * 0.5;
		for (TActorIterator<AActor> ActorIt(&World); ActorIt; ++ActorIt)
		{
			TInlineComponentArray<UPrimitiveComponent*> Components;
			ActorIt->GetComponents(Components);
			for (const UPrimitiveComponent* Component : Components)
			{
				const FBodyInstance* BodyInstance = Component->GetBodyInstance();
				const FBoxSphereBounds& Bounds = Component->Bounds;
				const FVector ComponentMin = Bounds.Origin - Bounds.BoxExtent;
				const FVector ComponentMax = Bounds.Origin + Bounds.BoxExtent;
				if (Component->IsRegistered()
					&& BodyInstance && BodyInstance->IsValidBodyInstance()
					&& Component->GetCollisionEnabled() != ECollisionEnabled::NoCollision
					&& Component->GetCollisionObjectType() == ECC_WorldStatic)
				{
					if (Bounds.SphereRadius > 0.0
						&& ComponentMax.X >= MinX && ComponentMin.X <= MaxX
						&& ComponentMax.Y >= MinY && ComponentMin.Y <= MaxY)
					{
						OutOwner = FString::Printf(TEXT("%s.%s"),
							*ActorIt->GetName(), *Component->GetName());
						return true;
					}
				}
			}
		}
		return false;
	}

	class FSharedMapEnvironment final : public IUERLEnvironment
	{
	public:
		explicit FSharedMapEnvironment(const FUERLProviderConfig& Config)
			: SpacingXCentimeters(Config.Scalars[UERLSharedMap::SpacingX] * SharedMapMetersToCentimeters)
			, SpacingYCentimeters(Config.Scalars[UERLSharedMap::SpacingY] * SharedMapMetersToCentimeters)
			, NumColumns(static_cast<int32>(Config.Scalars[UERLSharedMap::Columns]))
			, OriginXCentimeters(Config.Scalars[UERLSharedMap::OriginX] * SharedMapMetersToCentimeters)
			, OriginYCentimeters(Config.Scalars[UERLSharedMap::OriginY] * SharedMapMetersToCentimeters)
			, TraceStartZCentimeters(Config.Scalars[UERLSharedMap::TraceStartZ] * SharedMapMetersToCentimeters)
			, TraceDepthCentimeters(Config.Scalars[UERLSharedMap::TraceDepth] * SharedMapMetersToCentimeters)
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
			OutSlots.Reserve(NumSlots);
			bProceduralTerrain = TerrainConfig.NumLevels > 0;
			if (!bProceduralTerrain)
			{
				AuthoredGroundSamples.Reserve(NumSlots);
			}
			const FCollisionObjectQueryParams WorldStaticObjects(ECC_WorldStatic);
			FString AuthoredCollisionOwner;
			if (bProceduralTerrain
				&& FindAuthoredWorldStaticCollisionInAtlas(World, TerrainConfig, AuthoredCollisionOwner))
			{
				OutError = FString::Printf(
					TEXT("shared procedural terrain atlas overlaps authored WorldStatic collision '%s'"),
					*AuthoredCollisionOwner);
				return false;
			}
			for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
			{
				const int32 Column = SlotId % NumColumns;
				const int32 Row = SlotId / NumColumns;
				const double X = OriginXCentimeters + Column * SpacingXCentimeters;
				const double Y = OriginYCentimeters + Row * SpacingYCentimeters;
				FVector Origin(X, Y, 0.0);
				FVector GroundNormal = FVector::UpVector;
				AActor* AuthoredGroundActor = nullptr;
				UPrimitiveComponent* AuthoredGroundComponent = nullptr;
				if (!bProceduralTerrain)
				{
					const FVector Start(X, Y, TraceStartZCentimeters);
					const FVector End(X, Y, TraceStartZCentimeters - TraceDepthCentimeters);
					FHitResult Hit;
					if (!World.LineTraceSingleByObjectType(Hit, Start, End, WorldStaticObjects))
					{
						OutError = FString::Printf(
							TEXT("shared map has no WorldStatic ground below Slot %d at (%.3f, %.3f) m"),
							SlotId, X / SharedMapMetersToCentimeters, Y / SharedMapMetersToCentimeters);
						OutSlots.Reset();
						return false;
					}
					Origin = Hit.ImpactPoint;
					GroundNormal = Hit.ImpactNormal.GetSafeNormal();
					AuthoredGroundActor = Hit.GetActor();
					AuthoredGroundComponent = Hit.Component.Get();
					if (!AuthoredGroundActor)
					{
						OutError = FString::Printf(
							TEXT("shared map ground hit for Slot %d has no owning Actor"), SlotId);
						OutSlots.Reset();
						return false;
					}
				}

				FUERLSlotContext& Slot = OutSlots.AddDefaulted_GetRef();
				Slot.SlotId = SlotId;
				Slot.Origin = Origin;
				Slot.GroundHeight = Origin.Z;
				Slot.GroundNormal = GroundNormal;
				if (AuthoredGroundActor)
				{
					Slot.TerrainQueryActors.Add(AuthoredGroundActor);
					if (AuthoredGroundComponent)
					{
						Slot.GroundComponents.Add(AuthoredGroundComponent);
					}
					FUERLTerrainSpawnSample& Sample = AuthoredGroundSamples.AddDefaulted_GetRef();
					Sample.Origin = Origin;
					Sample.GroundHeight = Origin.Z;
					Sample.GroundNormal = GroundNormal;
				}
				Slot.CollisionProfile = CollisionPlan.Profile(SlotId);
			}
			if (bProceduralTerrain)
			{
				if (!TerrainGenerator.GenerateShared(World, TerrainConfig, OutSlots, OutError))
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
							TEXT("shared terrain tier 0 has no spawn sample for Slot %d"), Slot.SlotId);
						DestroySlots();
						OutSlots.Reset();
						return false;
					}
					Slot.Origin = Sample.Origin;
					Slot.GroundHeight = Sample.GroundHeight;
					Slot.GroundNormal = Sample.GroundNormal;
				}
			}
			SlotCount = NumSlots;
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
			if (!bProceduralTerrain)
			{
				if (TerrainLevel != 0 || !AuthoredGroundSamples.IsValidIndex(SlotId))
				{
					OutError = FString::Printf(
						TEXT("authored shared map has no ground sample for level %d Slot %d"),
						TerrainLevel, SlotId);
					return false;
				}
				const FUERLTerrainSpawnSample& Sample = AuthoredGroundSamples[SlotId];
				OutOrigin = Sample.Origin;
				OutGroundHeight = Sample.GroundHeight;
				OutGroundNormal = Sample.GroundNormal;
				return true;
			}
			FUERLTerrainSpawnSample Sample;
			if (!TerrainGenerator.TryGetSample(TerrainLevel, SlotId, Sample))
			{
				OutError = FString::Printf(
					TEXT("shared terrain level %d has no spawn sample for Slot %d"), TerrainLevel, SlotId);
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
			return SlotId >= 0 && SlotId < SlotCount
				? EUERLSlotFaultCode::None
				: EUERLSlotFaultCode::MissingObject;
		}

		void DestroySlots() override
		{
			TerrainGenerator.Destroy();
			AuthoredGroundSamples.Reset();
			bProceduralTerrain = false;
			SlotCount = 0;
		}

	private:
		double SpacingXCentimeters;
		double SpacingYCentimeters;
		int32 NumColumns;
		double OriginXCentimeters;
		double OriginYCentimeters;
		double TraceStartZCentimeters;
		double TraceDepthCentimeters;
		int32 SlotCount = 0;
		bool bProceduralTerrain = false;
		TArray<FUERLTerrainSpawnSample> AuthoredGroundSamples;
		FUERLTerrainGenerator TerrainGenerator;
	};

	class FSharedMapEnvironmentFactory final : public IUERLEnvironmentFactory
	{
	public:
		FSharedMapEnvironmentFactory()
		{
			Descriptor.Id = UERLSharedMap::EnvironmentId;
			Descriptor.Version = 1;
			Descriptor.CollisionScope = EUERLEnvironmentCollisionScope::SharedWorld;
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
				UERLSharedMap::SpacingX, UERLSharedMap::SpacingY, UERLSharedMap::Columns,
				UERLSharedMap::OriginX, UERLSharedMap::OriginY,
				UERLSharedMap::TraceStartZ, UERLSharedMap::TraceDepth,
			};
			for (const TPair<FName, double>& Pair : Input.Scalars)
			{
				if (!Allowed.Contains(Pair.Key) || !FMath::IsFinite(Pair.Value))
				{
					OutError = FString::Printf(
						TEXT("unknown or non-finite shared-world scalar '%s'"), *Pair.Key.ToString());
					return false;
				}
			}
			if (!Input.ResetDistributions.IsEmpty())
			{
				OutError = TEXT("shared-world Environment has no reset parameters");
				return false;
			}

			OutEffective = Input;
			OutEffective.Scalars.FindOrAdd(UERLSharedMap::SpacingX, 5.0);
			OutEffective.Scalars.FindOrAdd(UERLSharedMap::SpacingY, 5.0);
			OutEffective.Scalars.FindOrAdd(UERLSharedMap::Columns, 8.0);
			OutEffective.Scalars.FindOrAdd(UERLSharedMap::OriginX, 0.0);
			OutEffective.Scalars.FindOrAdd(UERLSharedMap::OriginY, 0.0);
			OutEffective.Scalars.FindOrAdd(UERLSharedMap::TraceStartZ, 10.0);
			OutEffective.Scalars.FindOrAdd(UERLSharedMap::TraceDepth, 20.0);

			const double ColumnValue = OutEffective.Scalars[UERLSharedMap::Columns];
			if (OutEffective.Scalars[UERLSharedMap::SpacingX] <= 0.0
				|| OutEffective.Scalars[UERLSharedMap::SpacingY] <= 0.0
				|| OutEffective.Scalars[UERLSharedMap::TraceDepth] <= 0.0
				|| ColumnValue < 1.0
				|| ColumnValue > FUERLSlotCollisionPlan::MaxSharedWorldSlots
				|| ColumnValue != FMath::RoundToDouble(ColumnValue))
			{
				OutError = TEXT("shared-world spacing and trace depth must be positive; columns must be an integer in 1..65536");
				return false;
			}
			return true;
		}

		TUniquePtr<IUERLEnvironment> Create(const FUERLProviderConfig& EffectiveConfig) const override
		{
			return MakeUnique<FSharedMapEnvironment>(EffectiveConfig);
		}

	private:
		FUERLEnvironmentDescriptor Descriptor;
	};
}

TSharedRef<IUERLEnvironmentFactory> UERLSharedMap::MakeFactory()
{
	return MakeShared<FSharedMapEnvironmentFactory>();
}
