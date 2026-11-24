#include "UERLTerrainAtlas.h"
#include "UERLSubTerrainGenerator.h"
#include "UERLTerrainInternal.h"

bool FUERLTerrainAtlas::BuildPlan(
	const FUERLTerrainConfig& Config,
	const TArray<FVector>& SlotOrigins,
	TArray<FUERLTerrainTierPlan>& OutPlans,
	FString& OutError)
{
	using namespace UERLTerrainInternal;

	OutPlans.Reset();
	if (!Config.IsValid())
	{
		OutError = TEXT("terrain configuration is invalid");
		return false;
	}
	if (SlotOrigins.Num() <= 0)
	{
		OutError = TEXT("terrain generation requires at least one Slot origin");
		return false;
	}

	OutPlans.Reserve(Config.Tiers.Num());
	for (const FUERLTerrainTierConfig& Tier : Config.Tiers)
	{
		FUERLTerrainTierPlan& Plan = OutPlans.AddDefaulted_GetRef();
		Plan.Level = Tier.Level;
		Plan.Samples.Reserve(SlotOrigins.Num());
		for (int32 SlotId = 0; SlotId < SlotOrigins.Num(); ++SlotId)
		{
			const FVector PatchOrigin = PatchOriginMeters(Config, Tier.Level, SlotOrigins[SlotId]);
			TArray<FVector2D> ResetCenters = { FVector2D::ZeroVector };
			FUERLTerrainSpawnSample& Sample = Plan.Samples.AddDefaulted_GetRef();
			Sample.Origin = PatchOrigin * CentimetresPerMetre;
			Sample.GroundHeight = Sample.Origin.Z;
			Sample.GroundNormal = FVector::UpVector;

			const IUERLSubTerrainGenerator* Generator =
				FindSubTerrainGenerator(PrimitiveId(Tier.Primitive));
			if (Generator == nullptr)
			{
				OutError = FString::Printf(
					TEXT("unsupported terrain primitive '%s'"),
					*PrimitiveId(Tier.Primitive).ToString());
				return false;
			}
			FUERLSubTerrainRequest Request;
			if (!MakeRequest(Config, Tier, SlotId, PatchOrigin, ResetCenters, Request, OutError))
			{
				return false;
			}
			FUERLTerrainPatch Patch;
			if (!Generator->Generate(Request, Patch, OutError))
			{
				return false;
			}
			AppendPatchToPlan(Patch, Plan);
			if (Tier.Primitive == EUERLTerrainPrimitive::Heightfield)
			{
				checkf(Patch.ResetHeights.Num() == ResetCenters.Num(),
					TEXT("heightfield reset-height count must match reset centers"));
				Sample.GroundHeight = (PatchOrigin.Z + Patch.ResetHeights[0])
					* CentimetresPerMetre;
				Sample.Origin = (PatchOrigin + FVector(
					ResetCenters[0].X, ResetCenters[0].Y, Patch.ResetHeights[0]))
					* CentimetresPerMetre;
			}
			else if (Tier.Primitive == EUERLTerrainPrimitive::Boxes)
			{
				bool bAdjustResetHeight = false;
				if (!ShouldAdjustBoxesResetHeight(Tier, bAdjustResetHeight, OutError))
				{
					return false;
				}
				if (bAdjustResetHeight)
				{
					checkf(Patch.ResetHeights.Num() == ResetCenters.Num(),
						TEXT("box reset-height count must match reset centers"));
					Sample.GroundHeight = (PatchOrigin.Z + Patch.ResetHeights[0]) * CentimetresPerMetre;
					Sample.Origin.Z = Sample.GroundHeight;
				}
			}
		}
	}
	return true;
}

bool FUERLTerrainAtlas::BuildSharedPlan(
	const FUERLTerrainConfig& Config,
	const TArray<FVector>& SlotOrigins,
	TArray<FUERLTerrainTierPlan>& OutPlans,
	FString& OutError)
{
	using namespace UERLTerrainInternal;

	OutPlans.Reset();
	if (!Config.IsValid())
	{
		OutError = TEXT("terrain configuration is invalid");
		return false;
	}
	if (SlotOrigins.IsEmpty())
	{
		OutError = TEXT("shared terrain generation requires at least one Slot origin");
		return false;
	}

	for (const FVector& OriginCentimetres : SlotOrigins)
	{
		const FVector OriginMetres = OriginCentimetres / CentimetresPerMetre;
		if (FMath::Abs(OriginMetres.X) + Config.BorderWidth > Config.CellSize[0] * 0.5
			|| FMath::Abs(OriginMetres.Y) + Config.BorderWidth > Config.CellSize[1] * 0.5)
		{
			OutError = TEXT("shared terrain spawn grid does not fit inside cell_size");
			return false;
		}
	}

	const double TierStrideX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
	OutPlans.Reserve(Config.Tiers.Num());
	for (const FUERLTerrainTierConfig& Tier : Config.Tiers)
	{
		FUERLTerrainTierPlan& Plan = OutPlans.AddDefaulted_GetRef();
		Plan.Level = Tier.Level;
		const FVector PatchOrigin(Tier.Level * TierStrideX, 0.0, 0.0);
		TArray<FVector2D> ResetCenters;
		ResetCenters.Reserve(SlotOrigins.Num());
		Plan.Samples.Reserve(SlotOrigins.Num());
		for (const FVector& OriginCentimetres : SlotOrigins)
		{
			const FVector OriginMetres = OriginCentimetres / CentimetresPerMetre;
			// Shared atlas tiles are translated by PatchOrigin. Slot origins remain
			// local coordinates inside every tile, so they must not include that
			// per-level world translation when matching the reset sampling window.
			ResetCenters.Add(FVector2D(OriginMetres.X, OriginMetres.Y));
			FUERLTerrainSpawnSample& Sample = Plan.Samples.AddDefaulted_GetRef();
			Sample.Origin = (PatchOrigin + OriginMetres)
				* CentimetresPerMetre;
			Sample.GroundHeight = Sample.Origin.Z;
			Sample.GroundNormal = FVector::UpVector;
		}

		const IUERLSubTerrainGenerator* Generator =
			FindSubTerrainGenerator(PrimitiveId(Tier.Primitive));
		if (Generator == nullptr)
		{
			OutError = FString::Printf(
				TEXT("unsupported terrain primitive '%s'"),
				*PrimitiveId(Tier.Primitive).ToString());
			return false;
		}
		FUERLSubTerrainRequest Request;
		if (!MakeRequest(Config, Tier, INDEX_NONE, PatchOrigin, ResetCenters, Request, OutError))
		{
			return false;
		}
		FUERLTerrainPatch Patch;
		if (!Generator->Generate(Request, Patch, OutError))
		{
			return false;
		}
		AppendPatchToPlan(Patch, Plan);
		if (Tier.Primitive == EUERLTerrainPrimitive::Heightfield)
		{
			checkf(Patch.ResetHeights.Num() == ResetCenters.Num()
				&& Patch.ResetHeights.Num() == Plan.Samples.Num(),
				TEXT("shared heightfield reset-height count must match reset centers and samples"));
			for (int32 SlotId = 0; SlotId < Plan.Samples.Num(); ++SlotId)
			{
				FUERLTerrainSpawnSample& Sample = Plan.Samples[SlotId];
				Sample.GroundHeight = (PatchOrigin.Z + Patch.ResetHeights[SlotId])
					* CentimetresPerMetre;
				Sample.Origin.Z = Sample.GroundHeight;
			}
		}
		else if (Tier.Primitive == EUERLTerrainPrimitive::Boxes)
		{
			bool bAdjustResetHeight = false;
			if (!ShouldAdjustBoxesResetHeight(Tier, bAdjustResetHeight, OutError))
			{
				return false;
			}
			if (bAdjustResetHeight)
			{
				checkf(Patch.ResetHeights.Num() == ResetCenters.Num()
					&& Patch.ResetHeights.Num() == Plan.Samples.Num(),
					TEXT("shared box reset-height count must match reset centers and samples"));
				for (int32 SlotId = 0; SlotId < Plan.Samples.Num(); ++SlotId)
				{
					FUERLTerrainSpawnSample& Sample = Plan.Samples[SlotId];
					Sample.GroundHeight = (PatchOrigin.Z + Patch.ResetHeights[SlotId])
						* CentimetresPerMetre;
					Sample.Origin.Z = Sample.GroundHeight;
				}
			}
		}
	}
	return true;
}

double FUERLTerrainAtlas::Difficulty(int32 Level, int32 NumLevels)
{
	if (NumLevels <= 1 || Level <= 0)
	{
		return 0.0;
	}
	const int32 Clamped = FMath::Clamp(Level, 0, NumLevels - 1);
	return static_cast<double>(Clamped) / static_cast<double>(NumLevels - 1);
}

bool FUERLTerrainAtlas::ResolveOrigin(
	const FUERLTerrainConfig& Config,
	int32 Level,
	int32 Column,
	FVector& OutOriginMeters,
	FString& OutError)
{
	OutOriginMeters = FVector::ZeroVector;
	if (!Config.IsValid())
	{
		OutError = TEXT("terrain configuration is invalid");
		return false;
	}
	if (Level < 0 || Level >= Config.NumLevels || Column < 0)
	{
		OutError = TEXT("terrain atlas level or column is out of range");
		return false;
	}
	const double StrideX = Config.CellSize[0] + 2.0 * Config.BorderWidth;
	const double StrideY = Config.CellSize[1] + 2.0 * Config.BorderWidth;
	OutOriginMeters = FVector(static_cast<double>(Level) * StrideX, static_cast<double>(Column) * StrideY, 0.0);
	OutError.Reset();
	return true;
}
