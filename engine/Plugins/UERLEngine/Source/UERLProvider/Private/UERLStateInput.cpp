#include "UERLStateInput.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UERLStateInputPrivate
{
	bool SameField(const FUERLFieldDescriptor& A, const FUERLFieldDescriptor& B)
	{
		return A.Name == B.Name && A.DType == B.DType && A.Shape == B.Shape && A.Width == B.Width
			&& A.Unit == B.Unit && A.CoordinateFrame == B.CoordinateFrame
			&& A.Semantic == B.Semantic && A.Source == B.Source;
	}

	class FProvider final : public IUERLInputProvider
	{
	public:
		explicit FProvider(TArray<FUERLFieldDescriptor> InFields) : Fields(MoveTemp(InFields)) {}
		bool Bind(TConstArrayView<FUERLInputSlotBinding> Bindings, FString& OutError) override
		{
			Release();
			for (const FUERLInputSlotBinding& Binding : Bindings)
			{
				TArray<FUERLInputStateReader>& Readers = Slots.Add(Binding.Slot.SlotId);
				for (const FUERLFieldDescriptor& Field : Fields)
				{
					const FUERLInputStateReader* Reader = Binding.StateReaders.Find(Field.Name);
					if (!Reader || !Reader->Read || !SameField(Field, Reader->Field))
					{
						OutError = FString::Printf(TEXT("Slot %d state field '%s' has no matching reader"),
							Binding.Slot.SlotId, *Field.Name.ToString());
						Release(); return false;
					}
					Readers.Add(*Reader);
				}
			}
			return true;
		}
		bool Sample(int32 SlotId, const FUERLInputSampleStamp& Stamp,
			TArrayView<float> OutValues, FString& OutError) override
		{
			const TArray<FUERLInputStateReader>* Readers = Slots.Find(SlotId);
			if (!Readers) { OutError = TEXT("state Slot is not bound"); return false; }
			int32 Offset = 0;
			for (const FUERLInputStateReader& Reader : *Readers)
			{
				FUERLInputSampleStamp Actual;
				TConstArrayView<float> Values;
				if (!Reader.Read(Actual, Values, OutError))
				{
					OutError = FString::Printf(TEXT("state field '%s': %s"), *Reader.Field.Name.ToString(), *OutError);
					return false;
				}
				if (Actual.Sequence != Stamp.Sequence || Actual.ResetGeneration != Stamp.ResetGeneration
					|| Actual.SolverTimeSeconds != Stamp.SolverTimeSeconds)
				{
					OutError = FString::Printf(TEXT("state field '%s' snapshot does not match the completed window/reset"),
						*Reader.Field.Name.ToString()); return false;
				}
				if (Values.Num() != Reader.Field.Width || Offset > OutValues.Num() - Values.Num())
				{
					OutError = FString::Printf(TEXT("state field '%s' snapshot width mismatch"), *Reader.Field.Name.ToString());
					return false;
				}
				for (float Value : Values) { OutValues[Offset++] = Value; }
			}
			return Offset == OutValues.Num();
		}
		bool Reset(TConstArrayView<int32>, FString&) override { return true; }
		void Release() override { Slots.Reset(); }
	private:
		TArray<FUERLFieldDescriptor> Fields;
		TMap<int32, TArray<FUERLInputStateReader>> Slots;
	};
}

FUERLStateInputFactory::FUERLStateInputFactory(TArray<FUERLFieldDescriptor> InFields)
	: Fields(MoveTemp(InFields)) {}

bool FUERLStateInputFactory::Validate(const FUERLInputSpec& Spec, FUERLInputSpec& OutSpec,
	TArray<FUERLFieldDescriptor>& OutFields, FString& OutError) const
{
	OutError.Reset(); OutFields.Reset();
	if (Spec.ProviderId != GetId() || Spec.ProviderVersion != GetVersion()
		|| !Spec.Attachment.IsEmpty() || !Spec.SceneBindings.IsEmpty())
	{
		OutError = TEXT("uerl.state version 1 retains the host frame and accepts no attachment/scene bindings");
		return false;
	}
	TMap<FName, const FUERLFieldDescriptor*> Catalog;
	for (const FUERLFieldDescriptor& Field : Fields)
	{
		if (!Field.IsValid() || Field.Shape.IsEmpty() || Catalog.Contains(Field.Name))
		{
			OutError = TEXT("state catalog contains an invalid or duplicate field"); return false;
		}
		Catalog.Add(Field.Name, &Field);
	}
	TSharedPtr<FJsonObject> Json;
	const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Spec.ParametersJson), Json)
		|| !Json || Json->Values.Num() != 1 || !Json->TryGetArrayField(TEXT("fields"), Names)
		|| !Names || Names->IsEmpty())
	{
		OutError = TEXT("state parameters require only a nonempty fields list"); return false;
	}
	TSet<FName> Seen;
	TArray<TSharedPtr<FJsonValue>> EffectiveNames;
	for (const TSharedPtr<FJsonValue>& Value : *Names)
	{
		FString Name;
		if (!Value || !Value->TryGetString(Name) || Name.IsEmpty() || Name.TrimStartAndEnd() != Name)
		{
			OutError = TEXT("expected state field names without outer whitespace"); return false;
		}
		const FName Key(*Name);
		const FUERLFieldDescriptor* const* Field = Catalog.Find(Key);
		if (!Field || Seen.Contains(Key))
		{
			OutError = FString::Printf(TEXT("state field '%s' is unavailable or duplicated"), *Name); return false;
		}
		Seen.Add(Key);
		OutFields.Add(**Field);
		EffectiveNames.Add(MakeShared<FJsonValueString>((*Field)->Name.ToString()));
	}
	OutSpec = Spec;
	const TSharedRef<FJsonObject> Effective = MakeShared<FJsonObject>();
	Effective->SetArrayField(TEXT("fields"), EffectiveNames);
	OutSpec.ParametersJson.Reset();
	FJsonSerializer::Serialize(Effective, TJsonWriterFactory<>::Create(&OutSpec.ParametersJson));
	return true;
}

TUniquePtr<IUERLInputProvider> FUERLStateInputFactory::Create(const FUERLInputSpec& Spec) const
{
	FUERLInputSpec Effective;
	TArray<FUERLFieldDescriptor> Selected;
	FString Error;
	if (!Validate(Spec, Effective, Selected, Error)) { return nullptr; }
	return MakeUnique<UERLStateInputPrivate::FProvider>(MoveTemp(Selected));
}
