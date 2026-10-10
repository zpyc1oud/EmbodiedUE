#include "UERLInputProvider.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#include <limits>

namespace UERLInputPrivate
{
	bool FiniteValue(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value) { return false; }
		if (Value->Type == EJson::Number) { return FMath::IsFinite(Value->AsNumber()); }
		if (Value->Type == EJson::Array)
		{
			for (const TSharedPtr<FJsonValue>& Item : Value->AsArray()) { if (!FiniteValue(Item)) { return false; } }
		}
		if (Value->Type == EJson::Object)
		{
			for (const auto& Pair : Value->AsObject()->Values)
			{
				if (Pair.Key.IsEmpty() || Pair.Key.TrimStartAndEnd() != Pair.Key || !FiniteValue(Pair.Value))
				{ return false; }
			}
		}
		return Value->Type != EJson::None;
	}

	bool ValidSpec(const FUERLInputSpec& Spec)
	{
		TSharedPtr<FJsonObject> Parameters;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Spec.ParametersJson);
		if (!FJsonSerializer::Deserialize(Reader, Parameters) || !Parameters) { return false; }
		for (const auto& Pair : Parameters->Values)
		{
			if (Pair.Key.IsEmpty() || Pair.Key.TrimStartAndEnd() != Pair.Key || !FiniteValue(Pair.Value)) { return false; }
		}
		TSet<FString> Names;
		for (const FString& Name : Spec.SceneBindings)
		{
			if (Name.IsEmpty() || Name.TrimStartAndEnd() != Name || Names.Contains(Name)) { return false; }
			Names.Add(Name);
		}
		return !Spec.Name.IsEmpty() && Spec.Name.TrimStartAndEnd() == Spec.Name
			&& !Spec.ProviderId.IsEmpty() && Spec.ProviderId.TrimStartAndEnd() == Spec.ProviderId
			&& Spec.ProviderVersion > 0
			&& Spec.Sampling == TEXT("completed_window") && Spec.MissingData == TEXT("fault");
	}
}

FUERLInputRegistry& FUERLInputRegistry::Get()
{
	static FUERLInputRegistry Registry;
	return Registry;
}

bool FUERLInputRegistry::RegisterFactory(const TSharedRef<IUERLInputFactory>& Factory, FString& OutError)
{
	OutError.Reset();
	const FString Id = Factory->GetId();
	const int32 Version = Factory->GetVersion();
	if (Id.IsEmpty() || Id.TrimStartAndEnd() != Id || Version < 1)
	{
		OutError = TEXT("input factory requires an id and positive version");
		return false;
	}
	FWriteScopeLock Guard(Lock);
	TMap<int32, TSharedPtr<IUERLInputFactory>>& Versions = Factories.FindOrAdd(Id);
	if (Versions.Contains(Version))
	{
		OutError = FString::Printf(TEXT("input factory '%s' version %d is already registered"), *Id, Version);
		return false;
	}
	Versions.Add(Version, Factory);
	return true;
}

void FUERLInputRegistry::UnregisterFactory(const FString& Id, int32 Version)
{
	FWriteScopeLock Guard(Lock);
	if (TMap<int32, TSharedPtr<IUERLInputFactory>>* Versions = Factories.Find(Id))
	{
		Versions->Remove(Version);
		if (Versions->IsEmpty()) { Factories.Remove(Id); }
	}
}

TSharedPtr<IUERLInputFactory> FUERLInputRegistry::Resolve(
	const FString& Id, int32 Version, FString& OutError) const
{
	OutError.Reset();
	FReadScopeLock Guard(Lock);
	if (const TMap<int32, TSharedPtr<IUERLInputFactory>>* Versions = Factories.Find(Id))
	{
		if (const TSharedPtr<IUERLInputFactory>* Factory = Versions->Find(Version)) { return *Factory; }
	}
	OutError = FString::Printf(TEXT("input factory '%s' version %d is unavailable"), *Id, Version);
	return nullptr;
}

FUERLCompiledInputSet::~FUERLCompiledInputSet() { Release(); }

void FUERLCompiledInputSet::Release()
{
	for (FCompiledProvider& Provider : Providers)
	{
		if (Provider.Instance) { Provider.Instance->Release(); }
	}
	Providers.Reset();
	Fields.Reset();
	Slots.Reset();
	bBound = false;
}

bool FUERLCompiledInputSet::Bind(const FUERLInputRegistry& Registry,
	TConstArrayView<FUERLInputSpec> Specs, TConstArrayView<FUERLInputSlotBinding> Bindings, FString& OutError)
{
	OutError.Reset();
	Release();
	TSet<FString> Names;
	TSet<FName> FieldNames;
	int32 Width = 0;
	for (const FUERLInputSlotBinding& Binding : Bindings)
	{
		if (Binding.Slot.SlotId < 0 || Slots.Contains(Binding.Slot.SlotId))
		{
			OutError = TEXT("input binding requires unique nonnegative Slot ids");
			Release();
			return false;
		}
		Slots.Add(Binding.Slot.SlotId);
	}
	for (const FUERLInputSpec& Spec : Specs)
	{
		if (!UERLInputPrivate::ValidSpec(Spec) || Names.Contains(Spec.Name))
		{
			OutError = FString::Printf(TEXT("input '%s' has an invalid or duplicate specification"), *Spec.Name);
			Release(); return false;
		}
		Names.Add(Spec.Name);
		FCompiledProvider Provider;
		Provider.Factory = Registry.Resolve(Spec.ProviderId, Spec.ProviderVersion, OutError);
		TArray<FUERLFieldDescriptor> Outputs;
		if (!Provider.Factory || !Provider.Factory->Validate(Spec, Provider.Spec, Outputs, OutError))
		{
			Release(); return false;
		}
		if (!UERLInputPrivate::ValidSpec(Provider.Spec) || Provider.Spec.Name != Spec.Name
			|| Provider.Spec.ProviderId != Spec.ProviderId || Provider.Spec.ProviderVersion != Spec.ProviderVersion
			|| Outputs.IsEmpty())
		{
			OutError = FString::Printf(TEXT("input '%s' factory changed identity or returned no valid outputs"), *Spec.Name);
			Release(); return false;
		}
		for (const FUERLInputSlotBinding& Binding : Bindings)
		{
			for (const FString& Required : Provider.Spec.SceneBindings)
			{
				if (!Binding.SceneBindings.Contains(Required))
				{
					OutError = FString::Printf(TEXT("input '%s' Slot %d is missing scene binding '%s'"),
						*Spec.Name, Binding.Slot.SlotId, *Required);
					Release(); return false;
				}
			}
		}
		Provider.Offset = Width;
		for (const FUERLFieldDescriptor& Output : Outputs)
		{
			if (!Output.IsValid() || Output.Shape.IsEmpty() || FieldNames.Contains(Output.Name)
				|| Output.Name.ToString().TrimStartAndEnd() != Output.Name.ToString()
				|| Output.Unit.TrimStartAndEnd() != Output.Unit
				|| Output.CoordinateFrame.TrimStartAndEnd() != Output.CoordinateFrame
				|| Output.Semantic.TrimStartAndEnd() != Output.Semantic || Output.Source.TrimStartAndEnd() != Output.Source
				|| Width > MAX_int32 - Output.Width)
			{
				OutError = FString::Printf(TEXT("input '%s' has an invalid or duplicate field '%s'"),
					*Spec.Name, *Output.Name.ToString());
				Release(); return false;
			}
			FieldNames.Add(Output.Name);
			Fields.Add(Output);
			Width += Output.Width;
		}
		Provider.Width = Width - Provider.Offset;
		Providers.Add(MoveTemp(Provider));
	}
	for (FCompiledProvider& Provider : Providers)
	{
		Provider.Instance = Provider.Factory->Create(Provider.Spec);
		if (!Provider.Instance || !Provider.Instance->Bind(Bindings, OutError))
		{
			if (OutError.IsEmpty()) { OutError = FString::Printf(TEXT("input '%s' could not bind"), *Provider.Spec.Name); }
			Release(); return false;
		}
	}
	for (TPair<int32, FSlotCache>& Pair : Slots) { Pair.Value.Values.SetNumUninitialized(Width); }
	bBound = true;
	return true;
}

bool FUERLCompiledInputSet::Sample(int32 SlotId, const FUERLInputSampleStamp& Stamp,
	TConstArrayView<float>& OutValues, FString& OutError)
{
	OutError.Reset();
	OutValues = TConstArrayView<float>();
	FSlotCache* Cache = Slots.Find(SlotId);
	if (!bBound || !Cache || Cache->bFaulted)
	{
		OutError = FString::Printf(TEXT("input Slot %d is unbound or faulted; bind/reset before sampling"), SlotId);
		return false;
	}
	if (Stamp.Sequence < 0 || Stamp.ResetGeneration != Cache->Generation
		|| !FMath::IsFinite(Stamp.SolverTimeSeconds) || Stamp.SolverTimeSeconds < 0.0)
	{
		OutError = FString::Printf(TEXT("input Slot %d has an invalid clock or reset generation"), SlotId);
		return false;
	}
	if (Cache->bPublished)
	{
		if (Stamp.Sequence == Cache->Stamp.Sequence && Stamp.SolverTimeSeconds == Cache->Stamp.SolverTimeSeconds)
		{
			OutValues = MakeArrayView(Cache->Values); return true;
		}
		if (Stamp.Sequence <= Cache->Stamp.Sequence || Stamp.SolverTimeSeconds <= Cache->Stamp.SolverTimeSeconds)
		{
			OutError = FString::Printf(TEXT("input Slot %d received a stale or inconsistent completed clock"), SlotId);
			return false;
		}
	}
	Cache->bPublished = false;
	for (float& Value : Cache->Values) { Value = std::numeric_limits<float>::quiet_NaN(); }
	for (FCompiledProvider& Provider : Providers)
	{
		TArrayView<float> Values(Cache->Values.GetData() + Provider.Offset, Provider.Width);
		if (!Provider.Instance->Sample(SlotId, Stamp, Values, OutError))
		{
			OutError = FString::Printf(TEXT("input '%s' Slot %d: %s"), *Provider.Spec.Name, SlotId, *OutError);
			Cache->bFaulted = true; return false;
		}
		for (float Value : Values)
		{
			if (!FMath::IsFinite(Value))
			{
				OutError = FString::Printf(TEXT("input '%s' Slot %d did not publish all finite fields"),
					*Provider.Spec.Name, SlotId);
				Cache->bFaulted = true; return false;
			}
		}
	}
	Cache->Stamp = Stamp;
	Cache->bPublished = true;
	OutValues = MakeArrayView(Cache->Values);
	return true;
}

bool FUERLCompiledInputSet::Reset(TConstArrayView<int32> SlotIds, FString& OutError)
{
	OutError.Reset();
	if (!bBound) { OutError = TEXT("input set must be bound before reset"); return false; }
	TSet<int32> Seen;
	for (int32 SlotId : SlotIds)
	{
		if (!Slots.Contains(SlotId) || Seen.Contains(SlotId) || Slots[SlotId].Generation == MAX_int64)
		{
			OutError = TEXT("input reset requires unique bound Slot ids and an available generation"); return false;
		}
		Seen.Add(SlotId);
	}
	for (int32 SlotId : SlotIds)
	{
		FSlotCache& Cache = Slots[SlotId];
		++Cache.Generation;
		Cache.bPublished = false;
		Cache.bFaulted = true;
	}
	for (FCompiledProvider& Provider : Providers)
	{
		if (!Provider.Instance->Reset(SlotIds, OutError)) { return false; }
	}
	for (int32 SlotId : SlotIds) { Slots[SlotId].bFaulted = false; }
	return true;
}

int64 FUERLCompiledInputSet::GetResetGeneration(int32 SlotId) const
{
	const FSlotCache* Cache = Slots.Find(SlotId);
	return Cache ? Cache->Generation : INDEX_NONE;
}

TArray<FUERLInputSpec> FUERLCompiledInputSet::GetEffectiveSpecs() const
{
	TArray<FUERLInputSpec> Result;
	Result.Reserve(Providers.Num());
	for (const FCompiledProvider& Provider : Providers) { Result.Add(Provider.Spec); }
	return Result;
}
