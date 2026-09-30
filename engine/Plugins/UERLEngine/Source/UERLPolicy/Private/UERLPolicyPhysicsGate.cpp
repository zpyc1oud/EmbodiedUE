#include "UERLPolicyPhysicsGate.h"

namespace
{
	constexpr double GateTolerance = 1.0e-6;

	void AddItem(
		TArray<FUERLPhysicsGateItem>& Items,
		FName Item,
		const FString& Actual,
		const FString& Requirement,
		const FString& Change)
	{
		FUERLPhysicsGateItem& Entry = Items.AddDefaulted_GetRef();
		Entry.Item = Item;
		Entry.Actual = Actual;
		Entry.Requirement = Requirement;
		Entry.Change = Change;
	}

	FString BoolText(bool Value)
	{
		return Value ? TEXT("true") : TEXT("false");
	}

	FString FormatDouble(double Value)
	{
		return FMath::IsFinite(Value)
			? FString::Printf(TEXT("%.9g"), Value)
			: TEXT("non-finite");
	}

	double BoundaryTolerance(double Quotient)
	{
		return FMath::Max(GateTolerance, 8.0 * UE_DOUBLE_SMALL_NUMBER * FMath::Max(1.0, FMath::Abs(Quotient)));
	}
}

FString FUERLPhysicsGateReport::ToString() const
{
	FString Result = bPassed ? TEXT("deployment physics gate passed") : TEXT("deployment physics gate failed");
	for (const FUERLPhysicsGateItem& Entry : Failures)
	{
		Result += FString::Printf(
			TEXT("\n%s: actual=%s; requirement=%s; change=%s"),
			*Entry.Item.ToString(), *Entry.Actual, *Entry.Requirement, *Entry.Change);
	}
	for (const FUERLPhysicsGateItem& Entry : Diagnostics)
	{
		Result += FString::Printf(
			TEXT("\n%s: actual=%s; requirement=%s; change=%s"),
			*Entry.Item.ToString(), *Entry.Actual, *Entry.Requirement, *Entry.Change);
	}
	return Result;
}

const FUERLPhysicsGateItem* FUERLPhysicsGateReport::FindFailure(FName Item) const
{
	return Failures.FindByPredicate([Item](const FUERLPhysicsGateItem& Entry)
	{
		return Entry.Item == Item;
	});
}

FUERLPhysicsGateReport CheckDeployPhysicsRequirements(
	const FUERLPhysicsSnapshot& Snapshot,
	const FUERLPolicyArtifactTiming& Timing)
{
	FUERLPhysicsGateReport Report;
	auto Fail = [&Report](
		FName Item,
		const FString& Actual,
		const FString& Requirement,
		const FString& Change)
	{
		Report.bPassed = false;
		AddItem(Report.Failures, Item, Actual, Requirement, Change);
	};

	const bool bTimingDtValid = FMath::IsFinite(Timing.PhysicsDt) && Timing.PhysicsDt > 0.0;
	const bool bTimingRangeValid = Timing.DecimationMin > 0 && Timing.DecimationMax >= Timing.DecimationMin;
	if (!bTimingDtValid)
	{
		Fail(
			TEXT("timing.physics_dt"),
			FormatDouble(Timing.PhysicsDt),
			TEXT("finite and > 0"),
			TEXT("regenerate the artifact timing.physics_dt"));
	}
	if (!bTimingRangeValid)
	{
		Fail(
			TEXT("timing.decimation"),
			FString::Printf(TEXT("[%d,%d]"), Timing.DecimationMin, Timing.DecimationMax),
			TEXT("positive closed range with min <= max"),
			TEXT("regenerate the artifact timing.decimation_min/max"));
	}

	if (!Snapshot.bHasSolver || Snapshot.SolverPath != FName(TEXT("Chaos")))
	{
		Fail(
			TEXT("solver_path"),
			Snapshot.bHasSolver ? Snapshot.SolverPath.ToString() : TEXT("none"),
			TEXT("the current World Chaos solver"),
			TEXT("run deployment in the World that owns the synchronous Chaos solver"));
	}
	if (Snapshot.bTickPhysicsAsync)
	{
		Fail(
			TEXT("bTickPhysicsAsync"),
			BoolText(Snapshot.bTickPhysicsAsync),
			TEXT("false"),
			TEXT("Project Settings > Engine > Physics > tick physics synchronously"));
	}
	if (Snapshot.bSubsteppingAsync)
	{
		Fail(
			TEXT("bSubsteppingAsync"),
			BoolText(Snapshot.bSubsteppingAsync),
			TEXT("false"),
			TEXT("Project Settings > Engine > Physics > disable async substepping"));
	}
	if (!Snapshot.bSubstepping)
	{
		Fail(
			TEXT("bSubstepping"),
			BoolText(Snapshot.bSubstepping),
			TEXT("true"),
			TEXT("Project Settings > Engine > Physics > enable synchronous substepping"));
	}

	const bool bSubstepDeltaValid = FMath::IsFinite(Snapshot.MaxSubstepDeltaTime)
		&& Snapshot.MaxSubstepDeltaTime > 0.0
		&& bTimingDtValid
		&& Snapshot.MaxSubstepDeltaTime <= Timing.PhysicsDt + GateTolerance;
	if (!bSubstepDeltaValid)
	{
		Fail(
			TEXT("MaxSubstepDeltaTime"),
			FormatDouble(Snapshot.MaxSubstepDeltaTime),
			FString::Printf(TEXT("finite, > 0, and <= physics_dt %s"), *FormatDouble(Timing.PhysicsDt)),
			TEXT("Project Settings > Engine > Physics > Max Substep Delta Time"));
	}

	if (Snapshot.MaxSubsteps < 0)
	{
		Fail(
			TEXT("MaxSubsteps"),
			FString::FromInt(Snapshot.MaxSubsteps),
			TEXT("a positive finite substep capacity"),
			TEXT("Project Settings > Engine > Physics > Max Substeps"));
	}
	if (bSubstepDeltaValid && bTimingRangeValid)
	{
		const double RequiredRatio = Timing.DtMax() / Snapshot.MaxSubstepDeltaTime;
		const int32 RequiredSubsteps = FMath::Max(2, FMath::CeilToInt(RequiredRatio - BoundaryTolerance(RequiredRatio)));
		if (Snapshot.MaxSubsteps < RequiredSubsteps)
		{
			Fail(
				TEXT("MaxSubsteps"),
				FString::Printf(TEXT("%d"), Snapshot.MaxSubsteps),
				FString::Printf(TEXT(">= %d for DtMax=%s / h=%s"),
					RequiredSubsteps, *FormatDouble(Timing.DtMax()), *FormatDouble(Snapshot.MaxSubstepDeltaTime)),
				TEXT("Project Settings > Engine > Physics > Max Substeps"));
		}
	}

	const bool bMinDeltaValid = FMath::IsFinite(Snapshot.MinPhysicsDeltaTime)
		&& Snapshot.MinPhysicsDeltaTime >= 0.0;
	if (!bMinDeltaValid)
	{
		Fail(
			TEXT("MinPhysicsDeltaTime"),
			FormatDouble(Snapshot.MinPhysicsDeltaTime),
			TEXT("finite and >= 0"),
			TEXT("Project Settings > Engine > Physics > Min Physics Delta Time"));
	}
	else if (bTimingDtValid && bTimingRangeValid && Snapshot.MinPhysicsDeltaTime > Timing.DtMin() + GateTolerance)
	{
		Fail(
			TEXT("MinPhysicsDeltaTime"),
			FormatDouble(Snapshot.MinPhysicsDeltaTime),
			FString::Printf(TEXT("<= declared DtMin %s"), *FormatDouble(Timing.DtMin())),
			TEXT("Project Settings > Engine > Physics > Min Physics Delta Time"));
	}

	if (!FMath::IsFinite(Snapshot.WorldTimeDilation)
		|| !FMath::IsNearlyEqual(Snapshot.WorldTimeDilation, 1.0, GateTolerance))
	{
		Fail(
			TEXT("world_time_dilation"),
			FormatDouble(Snapshot.WorldTimeDilation),
			TEXT("1.0"),
			TEXT("World Settings > Time Dilation"));
	}
	if (!Snapshot.bOwnerTimeDilationKnown)
	{
		Fail(
			TEXT("owner_time_dilation"),
			TEXT("unknown"),
			TEXT("known and 1.0"),
			TEXT("pass the deployed actor owner to the World physics snapshot helper"));
	}
	else if (!FMath::IsFinite(Snapshot.OwnerTimeDilation)
		|| !FMath::IsNearlyEqual(Snapshot.OwnerTimeDilation, 1.0, GateTolerance))
	{
		Fail(
			TEXT("owner_time_dilation"),
			FormatDouble(Snapshot.OwnerTimeDilation),
			TEXT("1.0"),
			TEXT("deployed actor > Custom Time Dilation"));
	}

	AddItem(
		Report.Diagnostics,
		TEXT("MaxPhysicsDeltaTime"),
		FormatDouble(Snapshot.MaxPhysicsDeltaTime),
		TEXT("display actual value; no failure in synchronous substep mode"),
		TEXT("no change required by this gate"));
	AddItem(
		Report.Diagnostics,
		TEXT("gravity_z"),
		FormatDouble(Snapshot.GravityZ),
		TEXT("display actual World gravity; no artifact baseline is declared"),
		TEXT("no change required by this gate"));
	return Report;
}
