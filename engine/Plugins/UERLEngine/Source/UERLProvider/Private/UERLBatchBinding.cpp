#include "UERLBatchBinding.h"

namespace
{
	bool BuildAvailableMap(
		const TArray<FUERLFieldDescriptor>& Fields,
		TMap<FName, FUERLFieldDescriptor>& Out,
		FString& OutError)
	{
		for (const FUERLFieldDescriptor& Field : Fields)
		{
			if (!Field.IsValid() || Out.Contains(Field.Name))
			{
				OutError = FString::Printf(TEXT("invalid or duplicate available field '%s'"), *Field.Name.ToString());
				return false;
			}
			Out.Add(Field.Name, Field);
		}
		return true;
	}

	bool BindSelected(
		const TArray<FUERLBatchFieldBinding>& Selected,
		const TMap<FName, FUERLFieldDescriptor>& Available,
		TMap<FName, FUERLBatchFieldBinding>& Out,
		bool bAllowObservationNameAlias,
		FString& OutError)
	{
		for (const FUERLBatchFieldBinding& Binding : Selected)
		{
			const FUERLFieldDescriptor* Offered = Available.Find(Binding.Field.Name);
			if (!Offered && bAllowObservationNameAlias && Binding.Field.Observation.Type != EUERLObservationType::None)
			{
				for (const TPair<FName, FUERLFieldDescriptor>& Pair : Available)
				{
					const FUERLFieldDescriptor& Candidate = Pair.Value;
					if (Candidate.Observation == Binding.Field.Observation
						&& Candidate.DType == Binding.Field.DType
						&& Candidate.Shape == Binding.Field.Shape
						&& Candidate.Width == Binding.Field.Width
						&& Candidate.Unit == Binding.Field.Unit
						&& Candidate.CoordinateFrame == Binding.Field.CoordinateFrame
						&& Candidate.Semantic == Binding.Field.Semantic
						&& Candidate.Source == Binding.Field.Source)
					{
						Offered = &Pair.Value;
						break;
					}
				}
			}
			if (!Offered || Offered->DType != Binding.Field.DType
				|| Offered->Shape != Binding.Field.Shape
				|| Offered->Width != Binding.Field.Width
				|| Offered->Unit != Binding.Field.Unit
				|| Offered->CoordinateFrame != Binding.Field.CoordinateFrame
				|| Offered->Semantic != Binding.Field.Semantic
				|| Offered->Source != Binding.Field.Source
				|| !(Offered->Observation == Binding.Field.Observation)
				|| Offered->ActuatorColumns != Binding.Field.ActuatorColumns)
			{
				OutError = FString::Printf(TEXT("selected field '%s' was not offered with identical metadata"),
					*Binding.Field.Name.ToString());
				return false;
			}
			Out.Add(Binding.Field.Name, Binding);
		}
		return true;
	}
}

bool FUERLBatchBinding::Compile(
	const FUERLBatchSchema& Schema,
	const TArray<FUERLFieldDescriptor>& AvailableActions,
	const TArray<FUERLFieldDescriptor>& AvailableStates,
	FString& OutError)
{
	Actions.Reset();
	States.Reset();
	CompiledActionWidth = 0;
	CompiledStateWidth = 0;
	bCompiled = false;
	if (!Schema.IsValid())
	{
		OutError = TEXT("Bridge supplied an invalid selected schema");
		return false;
	}

	TMap<FName, FUERLFieldDescriptor> ActionMap;
	TMap<FName, FUERLFieldDescriptor> StateMap;
	if (!BuildAvailableMap(AvailableActions, ActionMap, OutError)
		|| !BuildAvailableMap(AvailableStates, StateMap, OutError)
		|| !BindSelected(Schema.ActionFields, ActionMap, Actions, false, OutError)
		|| !BindSelected(Schema.StateFields, StateMap, States, true, OutError))
	{
		Actions.Reset();
		States.Reset();
		return false;
	}

	CompiledActionWidth = Schema.ActionWidth;
	CompiledStateWidth = Schema.StateWidth;
	bCompiled = true;
	return true;
}

void FUERLBatchBinding::Reset()
{
	Actions.Reset();
	States.Reset();
	CompiledActionWidth = 0;
	CompiledStateWidth = 0;
	bCompiled = false;
}

bool FUERLNamedActionReader::ReadVector(
	int32 Row,
	const FUERLBatchFieldBinding& FieldBinding,
	TArrayView<float> OutValues) const
{
	if (!View.IsValid() || OutValues.GetData() == nullptr || OutValues.Num() <= 0
		|| OutValues.Num() != FieldBinding.Field.Width || Row < 0 || Row >= View.NumRows
		|| FieldBinding.Column < 0 || FieldBinding.Column > View.NumColumns
		|| OutValues.Num() > View.NumColumns - FieldBinding.Column)
	{
		return false;
	}
	FMemory::Memcpy(
		OutValues.GetData(),
		View.Data + static_cast<int64>(Row) * View.NumColumns + FieldBinding.Column,
		OutValues.Num() * sizeof(float));
	return true;
}

bool FUERLNamedActionReader::ReadScalar(
	int32 Row,
	const FUERLBatchFieldBinding& FieldBinding,
	float& OutValue) const
{
	return ReadVector(Row, FieldBinding, TArrayView<float>(&OutValue, 1));
}

bool FUERLNamedActionReader::ReadVector(int32 Row, FName Field, TArrayView<float> OutValues) const
{
	const FUERLBatchFieldBinding* FieldBinding = Binding.FindAction(Field);
	return FieldBinding ? ReadVector(Row, *FieldBinding, OutValues) : false;
}

bool FUERLNamedActionReader::ReadScalar(int32 Row, FName Field, float& OutValue) const
{
	return ReadVector(Row, Field, TArrayView<float>(&OutValue, 1));
}

bool FUERLNamedStateWriter::WriteVector(
	int32 Row,
	const FUERLBatchFieldBinding& FieldBinding,
	TConstArrayView<float> Values)
{
	if (!View.IsValid() || Values.GetData() == nullptr || Values.Num() <= 0
		|| Values.Num() != FieldBinding.Field.Width || Row < 0 || Row >= View.NumRows
		|| FieldBinding.Column < 0 || FieldBinding.Column > View.NumColumns
		|| Values.Num() > View.NumColumns - FieldBinding.Column)
	{
		return false;
	}
	FMemory::Memcpy(&View.At(Row, FieldBinding.Column), Values.GetData(), Values.Num() * sizeof(float));
	return true;
}

bool FUERLNamedStateWriter::WriteScalar(
	int32 Row,
	const FUERLBatchFieldBinding& FieldBinding,
	float Value)
{
	return WriteVector(Row, FieldBinding, TConstArrayView<float>(&Value, 1));
}

bool FUERLNamedStateWriter::WriteVector(int32 Row, FName Field, TConstArrayView<float> Values)
{
	const FUERLBatchFieldBinding* FieldBinding = Binding.FindState(Field);
	return FieldBinding ? WriteVector(Row, *FieldBinding, Values) : false;
}

bool FUERLNamedStateWriter::WriteScalar(int32 Row, FName Field, float Value)
{
	return WriteVector(Row, Field, TConstArrayView<float>(&Value, 1));
}

void FUERLNamedStateWriter::FillRows(const TArray<int32>& Rows, float Value)
{
	if (!View.IsValid())
	{
		return;
	}
	for (int32 Row : Rows)
	{
		if (Row >= 0 && Row < View.NumRows)
		{
			for (int32 Column = 0; Column < View.NumColumns; ++Column)
			{
				View.At(Row, Column) = Value;
			}
		}
	}
}

void FUERLNamedStateWriter::FillAll(float Value)
{
	if (View.IsValid())
	{
		for (int32 Row = 0; Row < View.NumRows; ++Row)
		{
			for (int32 Column = 0; Column < View.NumColumns; ++Column)
			{
				View.At(Row, Column) = Value;
			}
		}
	}
}
