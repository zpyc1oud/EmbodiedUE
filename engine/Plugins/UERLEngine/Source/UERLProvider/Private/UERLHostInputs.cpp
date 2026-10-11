#include "UERLHostInputs.h"
#include "UERLStateInput.h"

namespace
{
	TSharedPtr<IUERLInputFactory> FactoryFor(const FUERLInputSpec& Spec,
		const TArray<FUERLFieldDescriptor>& HostFields, FString& Error)
	{
		if (Spec.ProviderId == TEXT("uerl.state") && Spec.ProviderVersion == 1)
		{ return MakeShared<FUERLStateInputFactory>(HostFields); }
		return FUERLInputRegistry::Get().Resolve(Spec.ProviderId, Spec.ProviderVersion, Error);
	}
}

bool ResolveUERLHostInputs(TArray<FUERLResolvedInput>& Inputs,
	const TArray<FUERLFieldDescriptor>& HostFields, FString& OutError)
{
	TSet<FString> Names;
	TSet<FName> Outputs;
	TArray<FUERLResolvedInput> Resolved;
	for (const FUERLResolvedInput& Input : Inputs)
	{
		const TSharedPtr<IUERLInputFactory> Factory = FactoryFor(Input.Spec, HostFields, OutError);
		FUERLResolvedInput Effective;
		if (!Factory || !Factory->Validate(Input.Spec, Effective.Spec, Effective.Fields, OutError)) { return false; }
		if (Names.Contains(Effective.Spec.Name) || Effective.Spec.Name != Input.Spec.Name
			|| Effective.Spec.ProviderId != Input.Spec.ProviderId || Effective.Spec.ProviderVersion != Input.Spec.ProviderVersion
			|| Effective.Fields.IsEmpty())
		{ OutError = TEXT("invalid or duplicate input factory result"); return false; }
		Names.Add(Effective.Spec.Name);
		if (!Input.Fields.IsEmpty())
		{
			if (Input.Fields.Num() != Effective.Fields.Num())
			{ OutError = TEXT("persisted input field count does not match installed provider"); return false; }
			for (int32 I = 0; I < Input.Fields.Num(); ++I)
			{
				if (!SameUERLInputField(Input.Fields[I], Effective.Fields[I]))
				{ OutError = TEXT("persisted input field meaning does not match installed provider"); return false; }
			}
		}
		for (const FUERLFieldDescriptor& Field : Effective.Fields)
		{
			if (!Field.IsValid() || Outputs.Contains(Field.Name))
			{ OutError = TEXT("invalid or duplicate input output field"); return false; }
			for (const FUERLFieldDescriptor& HostField : HostFields)
			{
				if (HostField.Name == Field.Name && (Effective.Spec.ProviderId != TEXT("uerl.state")
					|| !SameUERLInputField(HostField, Field)))
				{ OutError = TEXT("input output collides with host state"); return false; }
			}
			Outputs.Add(Field.Name);
		}
		Resolved.Add(MoveTemp(Effective));
	}
	Inputs = MoveTemp(Resolved); OutError.Reset(); return true;
}

bool FUERLHostInputs::Configure(const TArray<FUERLResolvedInput>& Inputs,
	const TArray<FUERLFieldDescriptor>& HostFields, const TArray<FUERLFieldDescriptor>& SelectedFields, FString& OutError)
{
	Release(); Requirements = Inputs;
	if (!ResolveUERLHostInputs(Requirements, HostFields, OutError)) { Release(); return false; }
	TMap<FName, int32> SourceOffsets, InputOffsets;
	TMap<FName, FUERLFieldDescriptor> InputFields;
	int32 SourceWidth = 0, InputWidth = 0;
	auto AddSource = [&](const FUERLFieldDescriptor& Field)
	{
		if (!SourceOffsets.Contains(Field.Name))
		{ SourceOffsets.Add(Field.Name, SourceWidth); SourceWidth += Field.Width; Sources.Add(Field); }
	};
	for (const FUERLResolvedInput& Input : Requirements)
	{
		for (const FUERLFieldDescriptor& Field : Input.Fields)
		{
			InputOffsets.Add(Field.Name, InputWidth); InputWidth += Field.Width;
			InputFields.Add(Field.Name, Field);
			if (Input.Spec.ProviderId == TEXT("uerl.state")) { AddSource(Field); }
		}
	}
	for (const FUERLFieldDescriptor& Field : SelectedFields)
	{
		if (const int32* Offset = InputOffsets.Find(Field.Name))
		{
			if (!SameUERLInputField(InputFields[Field.Name], Field))
			{ OutError = TEXT("selected input field meaning does not match provider"); Release(); return false; }
			Routes.Add({true, *Offset, Field.Width});
		}
		else
		{
			const FUERLFieldDescriptor* Source = HostFields.FindByPredicate([&](const auto& F) { return F.Name == Field.Name; });
			if (!Source || !SameUERLInputField(*Source, Field))
			{ OutError = TEXT("selected input field is unavailable or has changed meaning"); Release(); return false; }
			AddSource(*Source); Routes.Add({false, SourceOffsets[Field.Name], Field.Width});
		}
	}
	return true;
}

bool FUERLHostInputs::Bind(TArray<FUERLInputSlotBinding> Bindings, FString& OutError)
{
	Set.Release(); Snapshots.Reset();
	for (FUERLInputSlotBinding& Binding : Bindings)
	{
		const int32 SlotId = Binding.Slot.SlotId;
		Snapshots.Add(SlotId);
		int32 Offset = 0;
		for (const FUERLFieldDescriptor& Field : Sources)
		{
			FUERLInputStateReader Reader; Reader.Field = Field;
			const int32 Width = Field.Width;
			Reader.Read = [this, SlotId, Offset, Width](FUERLInputSampleStamp& Stamp,
				TConstArrayView<float>& Values, FString& Error)
			{
				const FSnapshot* Snapshot = Snapshots.Find(SlotId);
				if (!Snapshot || Snapshot->Values.Num() < Offset + Width)
				{ Error = TEXT("host input snapshot is unavailable"); return false; }
				Stamp = Snapshot->Stamp; Values = MakeArrayView(Snapshot->Values).Slice(Offset, Width); return true;
			};
			Binding.StateReaders.Add(Field.Name, MoveTemp(Reader)); Offset += Width;
		}
	}
	FUERLInputRegistry Registry;
	TSet<FString> Registered;
	TArray<FUERLInputSpec> Specs;
	for (const FUERLResolvedInput& Input : Requirements)
	{
		const FString Key = Input.Spec.ProviderId + TEXT(":") + LexToString(Input.Spec.ProviderVersion);
		if (!Registered.Contains(Key))
		{
			const TSharedPtr<IUERLInputFactory> Factory = FactoryFor(Input.Spec, Sources, OutError);
			if (!Factory || !Registry.RegisterFactory(Factory.ToSharedRef(), OutError)) { return false; }
			Registered.Add(Key);
		}
		Specs.Add(Input.Spec);
	}
	return Set.Bind(Registry, Specs, Bindings, OutError);
}

bool FUERLHostInputs::Publish(int32 SlotId, int64 Sequence, double SolverTime,
	TConstArrayView<float> HostValues, TArray<float>& OutValues, FString& OutError)
{
	OutValues.Reset();
	FSnapshot* Snapshot = Snapshots.Find(SlotId);
	int32 ExpectedWidth = 0;
	for (const auto& Field : Sources) { ExpectedWidth += Field.Width; }
	if (!Snapshot || HostValues.Num() != ExpectedWidth)
	{ OutError = TEXT("host input state width/Slot mismatch"); return false; }
	const FUERLInputSampleStamp Stamp{Sequence, Set.GetResetGeneration(SlotId), SolverTime};
	if (Snapshot->Values.Num() != ExpectedWidth || Snapshot->Stamp.Sequence != Stamp.Sequence
		|| Snapshot->Stamp.ResetGeneration != Stamp.ResetGeneration || Snapshot->Stamp.SolverTimeSeconds != SolverTime)
	{
		Snapshot->Stamp = Stamp;
		Snapshot->Values = TArray<float>(HostValues);
	}
	TConstArrayView<float> InputValues;
	if (!Set.Sample(SlotId, Stamp, InputValues, OutError)) { return false; }
	for (const FRoute& Route : Routes)
	{
		const TConstArrayView<float> Source = Route.bInput ? InputValues : MakeArrayView(Snapshot->Values);
		OutValues.Append(Source.GetData() + Route.Offset, Route.Width);
	}
	return true;
}

void FUERLHostInputs::Release()
{
	Set.Release(); Snapshots.Reset(); Requirements.Reset(); Sources.Reset(); Routes.Reset();
}
