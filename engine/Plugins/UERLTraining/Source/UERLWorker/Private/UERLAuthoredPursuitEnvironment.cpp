#include "UERLAuthoredPursuitEnvironment.h"

#include "UERLBatchBinding.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"

namespace UERLAuthoredPursuit
{
	const FName EnvironmentId(TEXT("uerl.environment.authored_pursuit"));
	const FName OriginX(TEXT("environment.origin_x_m"));
	const FName OriginY(TEXT("environment.origin_y_m"));
	const FName TraceStartZ(TEXT("environment.trace_start_z_m"));
	const FName TraceDepth(TEXT("environment.trace_depth_m"));
	const FName TargetPositionField(TEXT("environment.pursuit_target_position"));
}

namespace
{
	constexpr double AuthoredPursuitMetersToCentimeters = 100.0;

	FUERLFieldDescriptor TargetPositionDescriptor()
	{
		FUERLFieldDescriptor Field;
		Field.Name = UERLAuthoredPursuit::TargetPositionField;
		Field.Shape = { 3 };
		Field.Unit = TEXT("m");
		Field.CoordinateFrame = TEXT("slot/local");
		Field.Semantic = TEXT("pursuit_target_position");
		Field.Source = TEXT("uerl.environment");
		Field.Width = 3;
		return Field;
	}

	class FAuthoredPursuitEnvironment final : public IUERLEnvironment
	{
	public:
		explicit FAuthoredPursuitEnvironment(const FUERLProviderConfig& Config)
			: OriginXCentimeters(Config.Scalars[UERLAuthoredPursuit::OriginX] * AuthoredPursuitMetersToCentimeters)
			, OriginYCentimeters(Config.Scalars[UERLAuthoredPursuit::OriginY] * AuthoredPursuitMetersToCentimeters)
			, TraceStartZCentimeters(Config.Scalars[UERLAuthoredPursuit::TraceStartZ] * AuthoredPursuitMetersToCentimeters)
			, TraceDepthCentimeters(Config.Scalars[UERLAuthoredPursuit::TraceDepth] * AuthoredPursuitMetersToCentimeters)
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
			if (NumSlots != 1 || TerrainConfig.NumLevels != 0)
			{
				OutError = TEXT("authored pursuit requires exactly one Slot and no procedural terrain");
				return false;
			}
			APawn* PlayerPawn = UGameplayStatics::GetPlayerPawn(&World, 0);
			if (!PlayerPawn)
			{
				OutError = TEXT("authored pursuit requires Player 0 to possess a Pawn before Worker initialization");
				return false;
			}

			const FVector Start(OriginXCentimeters, OriginYCentimeters, TraceStartZCentimeters);
			const FVector End = Start - FVector(0.0, 0.0, TraceDepthCentimeters);
			FHitResult Hit;
			const FCollisionObjectQueryParams WorldStaticObjects(ECC_WorldStatic);
			if (!World.LineTraceSingleByObjectType(Hit, Start, End, WorldStaticObjects) || !Hit.GetActor())
			{
				OutError = FString::Printf(
					TEXT("authored pursuit has no WorldStatic ground at (%.3f, %.3f) m"),
					OriginXCentimeters / AuthoredPursuitMetersToCentimeters,
					OriginYCentimeters / AuthoredPursuitMetersToCentimeters);
				return false;
			}

			FUERLSlotContext& Slot = OutSlots.AddDefaulted_GetRef();
			Slot.SlotId = 0;
			Slot.Origin = Hit.ImpactPoint;
			Slot.GroundHeight = Hit.ImpactPoint.Z;
			Slot.GroundNormal = Hit.ImpactNormal.GetSafeNormal();
			Slot.TerrainQueryActors.Add(Hit.GetActor());
			if (Hit.Component.IsValid())
			{
				Slot.GroundComponents.Add(Hit.Component);
			}
			Slot.CollisionProfile = CollisionPlan.Profile(0);
			GroundOrigin = Slot.Origin;
			GroundRotation = FQuat::FindBetweenNormals(FVector::UpVector, Slot.GroundNormal);
			TargetPawn = PlayerPawn;
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
			if (SlotId != 0 || TerrainLevel != 0 || !TargetPawn.IsValid())
			{
				OutError = TEXT("authored pursuit ground frame is unavailable");
				return false;
			}
			OutOrigin = GroundOrigin;
			OutGroundHeight = GroundOrigin.Z;
			OutGroundNormal = GroundRotation.RotateVector(FVector::UpVector);
			return true;
		}

		bool ResetSlots(const FUERLResetBatch&, FString&) override
		{
			return true;
		}

		void CollectState(const TArray<int32>& Slots, FUERLNamedStateWriter& Writer) const override
		{
			const FVector LocalPosition = GroundRotation.Inverse().RotateVector(
				TargetPawn->GetActorLocation() - GroundOrigin) / AuthoredPursuitMetersToCentimeters;
			const float Values[] = {
				static_cast<float>(LocalPosition.X),
				static_cast<float>(LocalPosition.Y),
				static_cast<float>(LocalPosition.Z),
			};
			for (const int32 SlotId : Slots)
			{
				checkf(SlotId == 0, TEXT("authored pursuit received invalid Slot %d"), SlotId);
				checkf(
					Writer.WriteVector(
						SlotId,
						UERLAuthoredPursuit::TargetPositionField,
						TConstArrayView<float>(Values, UE_ARRAY_COUNT(Values))),
					TEXT("authored pursuit failed to write target position"));
			}
		}

		EUERLSlotFaultCode ValidateSlot(int32 SlotId, FString& OutReason) const override
		{
			if (SlotId != 0 || !TargetPawn.IsValid())
			{
				OutReason = TEXT("authored pursuit Player Pawn is missing");
				return EUERLSlotFaultCode::MissingObject;
			}
			return EUERLSlotFaultCode::None;
		}

		void DestroySlots() override
		{
			TargetPawn.Reset();
			GroundOrigin = FVector::ZeroVector;
			GroundRotation = FQuat::Identity;
		}

	private:
		double OriginXCentimeters;
		double OriginYCentimeters;
		double TraceStartZCentimeters;
		double TraceDepthCentimeters;
		FVector GroundOrigin = FVector::ZeroVector;
		FQuat GroundRotation = FQuat::Identity;
		TWeakObjectPtr<APawn> TargetPawn;
	};

	class FAuthoredPursuitEnvironmentFactory final : public IUERLEnvironmentFactory
	{
	public:
		FAuthoredPursuitEnvironmentFactory()
		{
			Descriptor.Id = UERLAuthoredPursuit::EnvironmentId;
			Descriptor.Version = 1;
			Descriptor.CollisionScope = EUERLEnvironmentCollisionScope::SharedWorld;
			Descriptor.StateFields.Add(TargetPositionDescriptor());
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
				UERLAuthoredPursuit::OriginX,
				UERLAuthoredPursuit::OriginY,
				UERLAuthoredPursuit::TraceStartZ,
				UERLAuthoredPursuit::TraceDepth,
			};
			for (const TPair<FName, double>& Pair : Input.Scalars)
			{
				if (!Allowed.Contains(Pair.Key) || !FMath::IsFinite(Pair.Value))
				{
					OutError = FString::Printf(
						TEXT("unknown or non-finite authored-pursuit scalar '%s'"), *Pair.Key.ToString());
					return false;
				}
			}
			if (!Input.ResetDistributions.IsEmpty() || !Input.AssetPath.IsEmpty()
				|| !Input.Actuators.IsEmpty() || !Input.ResetBindings.IsEmpty())
			{
				OutError = TEXT("authored pursuit accepts only Environment scalars");
				return false;
			}
			OutEffective = Input;
			OutEffective.Scalars.FindOrAdd(UERLAuthoredPursuit::OriginX, 0.0);
			OutEffective.Scalars.FindOrAdd(UERLAuthoredPursuit::OriginY, 0.0);
			OutEffective.Scalars.FindOrAdd(UERLAuthoredPursuit::TraceStartZ, 10.0);
			OutEffective.Scalars.FindOrAdd(UERLAuthoredPursuit::TraceDepth, 20.0);
			if (OutEffective.Scalars[UERLAuthoredPursuit::TraceDepth] <= 0.0)
			{
				OutError = TEXT("authored pursuit trace depth must be positive");
				return false;
			}
			return true;
		}

		TUniquePtr<IUERLEnvironment> Create(const FUERLProviderConfig& EffectiveConfig) const override
		{
			return MakeUnique<FAuthoredPursuitEnvironment>(EffectiveConfig);
		}

	private:
		FUERLEnvironmentDescriptor Descriptor;
	};
}

TSharedRef<IUERLEnvironmentFactory> UERLAuthoredPursuit::MakeFactory()
{
	return MakeShared<FAuthoredPursuitEnvironmentFactory>();
}
