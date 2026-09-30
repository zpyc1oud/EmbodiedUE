#include "UERLSafetyMonitor.h"

void FUERLSafetyMonitor::Initialize(int32 InNumSlots, int32 InStateWidth)
{
	NumSlots = InNumSlots;
	StateWidth = InStateWidth;
	LastValidStates.SetNumZeroed(NumSlots * StateWidth);
	HasLastValid.SetNumZeroed(NumSlots);
}

void FUERLSafetyMonitor::Reset()
{
	NumSlots = 0;
	StateWidth = 0;
	LastValidStates.Reset();
	HasLastValid.Reset();
}

bool FUERLSafetyMonitor::IsFiniteRow(FUERLMutableBatchView States, int32 Row) const
{
	for (int32 Column = 0; Column < StateWidth; ++Column)
	{
		if (!FMath::IsFinite(States.At(Row, Column)))
		{
			return false;
		}
	}
	return true;
}

void FUERLSafetyMonitor::SaveRow(FUERLMutableBatchView States, int32 Row)
{
	FMemory::Memcpy(LastValidStates.GetData() + Row * StateWidth,
		States.Data + Row * States.NumColumns, StateWidth * sizeof(float));
	HasLastValid[Row] = 1;
}

void FUERLSafetyMonitor::RestoreRow(FUERLMutableBatchView States, int32 Row) const
{
	FMemory::Memcpy(States.Data + Row * States.NumColumns,
		LastValidStates.GetData() + Row * StateWidth, StateWidth * sizeof(float));
}

bool FUERLSafetyMonitor::AcceptInitial(
	FUERLMutableBatchView States,
	const TArray<EUERLSlotFaultCode>& ProviderFaults,
	FUERLFaultBatchView Faults,
	FString& OutError)
{
	if (!States.IsValid() || !Faults.IsValid() || States.NumRows != NumSlots
		|| States.NumColumns != StateWidth || Faults.NumRows != NumSlots || ProviderFaults.Num() != NumSlots)
	{
		OutError = TEXT("initial Safety views do not match configured batch");
		return false;
	}
	for (int32 Row = 0; Row < NumSlots; ++Row)
	{
		EUERLSlotFaultCode Code = ProviderFaults[Row];
		if (Code == EUERLSlotFaultCode::None && !IsFiniteRow(States, Row))
		{
			Code = EUERLSlotFaultCode::NonFiniteStagingState;
		}
		if (Code != EUERLSlotFaultCode::None)
		{
			OutError = FString::Printf(TEXT("initial state for slot %d is invalid (fault=%d)"), Row, static_cast<int32>(Code));
			return false;
		}
		SaveRow(States, Row);
		Faults.At(Row) = static_cast<uint8>(EUERLSlotFaultCode::None);
	}
	return true;
}

bool FUERLSafetyMonitor::SanitizeTransition(
	FUERLMutableBatchView States,
	const TArray<EUERLSlotFaultCode>& ProviderFaults,
	FUERLFaultBatchView Faults,
	FString& OutError)
{
	if (!States.IsValid() || !Faults.IsValid() || ProviderFaults.Num() != NumSlots)
	{
		OutError = TEXT("transition Safety views do not match configured batch");
		return false;
	}
	for (int32 Row = 0; Row < NumSlots; ++Row)
	{
		EUERLSlotFaultCode Code = ProviderFaults[Row];
		if (Code == EUERLSlotFaultCode::None && !IsFiniteRow(States, Row))
		{
			Code = EUERLSlotFaultCode::NonFiniteStagingState;
		}
		if (Code == EUERLSlotFaultCode::None)
		{
			SaveRow(States, Row);
		}
		else if (HasLastValid.IsValidIndex(Row) && HasLastValid[Row])
		{
			RestoreRow(States, Row);
		}
		else
		{
			OutError = FString::Printf(TEXT("slot %d fault has no finite fallback"), Row);
			return false;
		}
		Faults.At(Row) = static_cast<uint8>(Code);
	}
	return true;
}

bool FUERLSafetyMonitor::AcceptReset(
	FUERLMutableBatchView States,
	const TArray<int32>& ResetSlots,
	const TArray<EUERLSlotFaultCode>& ProviderFaults,
	FUERLFaultBatchView Faults,
	FString& OutError)
{
	if (!States.IsValid() || !Faults.IsValid() || ProviderFaults.Num() != NumSlots)
	{
		OutError = TEXT("reset Safety views do not match configured batch");
		return false;
	}
	for (int32 Row : ResetSlots)
	{
		if (Row < 0 || Row >= NumSlots || ProviderFaults[Row] != EUERLSlotFaultCode::None || !IsFiniteRow(States, Row))
		{
			OutError = FString::Printf(TEXT("reset failed to recover slot %d"), Row);
			return false;
		}
		SaveRow(States, Row);
		Faults.At(Row) = static_cast<uint8>(EUERLSlotFaultCode::None);
	}
	return true;
}
