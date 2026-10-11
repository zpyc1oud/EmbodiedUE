#include "UERLInputContract.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	bool Keys(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Required)
	{
		if (!Object || Object->Values.Num() != Required.Num()) { return false; }
		for (const FString& Name : Required) { if (!Object->HasField(Name)) { return false; } }
		return true;
	}
	bool Name(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, FString& Out)
	{
		return Object->TryGetStringField(Key, Out) && !Out.IsEmpty() && Out.TrimStartAndEnd() == Out;
	}
	TArray<TSharedPtr<FJsonValue>> Strings(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Result;
		for (const FString& Value : Values) { Result.Add(MakeShared<FJsonValueString>(Value)); }
		return Result;
	}
}

bool SameUERLInputField(const FUERLFieldDescriptor& A, const FUERLFieldDescriptor& B)
{
	return A.Name == B.Name && A.Shape == B.Shape && A.Width == B.Width && A.DType == B.DType
		&& A.Unit == B.Unit && A.CoordinateFrame == B.CoordinateFrame && A.Semantic == B.Semantic && A.Source == B.Source;
}

bool ParseUERLInputContract(const TSharedPtr<FJsonObject>& Json,
	TArray<FUERLResolvedInput>& Out, bool bRequireFields, FString& OutError)
{
	Out.Reset();
	OutError = TEXT("invalid version 1 input_contract");
	double Version = 0;
	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Keys(Json, {TEXT("version"), TEXT("requirements")})
		|| !Json->TryGetNumberField(TEXT("version"), Version) || Version != 1
		|| !Json->TryGetArrayField(TEXT("requirements"), Rows)) { return false; }
	TSet<FString> Names;
	TSet<FName> FieldNames;
	TArray<FUERLResolvedInput> Parsed;
	for (const TSharedPtr<FJsonValue>& Row : *Rows)
	{
		if (!Row || Row->Type != EJson::Object) { return false; }
		const TSharedPtr<FJsonObject> Object = Row->AsObject();
		const TSharedPtr<FJsonObject>* SpecObject = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Fields = nullptr;
		if (!Keys(Object, {TEXT("spec"), TEXT("fields")})
			|| !Object->TryGetObjectField(TEXT("spec"), SpecObject)
			|| !Object->TryGetArrayField(TEXT("fields"), Fields) || (bRequireFields && Fields->IsEmpty())) { return false; }
		const TSharedPtr<FJsonObject>& S = *SpecObject;
		if (!Keys(S, {TEXT("name"), TEXT("provider_id"), TEXT("provider_version"), TEXT("parameters"),
			TEXT("attachment"), TEXT("scene_bindings"), TEXT("sampling"), TEXT("missing_data")})) { return false; }
		FUERLResolvedInput Input;
		double ProviderVersion = 0;
		const TSharedPtr<FJsonObject>* Parameters = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Bindings = nullptr;
		if (!Name(S, TEXT("name"), Input.Spec.Name) || !Name(S, TEXT("provider_id"), Input.Spec.ProviderId)
			|| !S->TryGetNumberField(TEXT("provider_version"), ProviderVersion) || !FMath::IsFinite(ProviderVersion)
			|| ProviderVersion < 1 || ProviderVersion > MAX_int32 || FMath::FloorToDouble(ProviderVersion) != ProviderVersion
			|| !S->TryGetObjectField(TEXT("parameters"), Parameters)
			|| !S->TryGetArrayField(TEXT("scene_bindings"), Bindings)
			|| !S->TryGetStringField(TEXT("sampling"), Input.Spec.Sampling) || Input.Spec.Sampling != TEXT("completed_window")
			|| !S->TryGetStringField(TEXT("missing_data"), Input.Spec.MissingData) || Input.Spec.MissingData != TEXT("fault")
			|| Names.Contains(Input.Spec.Name)) { return false; }
		Names.Add(Input.Spec.Name);
		Input.Spec.ProviderVersion = static_cast<int32>(ProviderVersion);
		Input.Spec.ParametersJson.Reset();
		FJsonSerializer::Serialize(Parameters->ToSharedRef(), TJsonWriterFactory<>::Create(&Input.Spec.ParametersJson));
		if (!S->HasTypedField<EJson::Null>(TEXT("attachment")) && !Name(S, TEXT("attachment"), Input.Spec.Attachment)) { return false; }
		for (const TSharedPtr<FJsonValue>& Binding : *Bindings)
		{
			FString Value;
			if (!Binding || !Binding->TryGetString(Value) || Value.IsEmpty() || Value.TrimStartAndEnd() != Value
				|| Input.Spec.SceneBindings.Contains(Value)) { return false; }
			Input.Spec.SceneBindings.Add(Value);
		}
		for (const TSharedPtr<FJsonValue>& FieldValue : *Fields)
		{
			if (!FieldValue || FieldValue->Type != EJson::Object) { return false; }
			const TSharedPtr<FJsonObject> F = FieldValue->AsObject();
			if (!Keys(F, {TEXT("name"), TEXT("shape"), TEXT("dtype"), TEXT("unit"), TEXT("frame"), TEXT("semantic"), TEXT("source")})) { return false; }
			FUERLFieldDescriptor Field;
			FString FieldName;
			const TArray<TSharedPtr<FJsonValue>>* Shape = nullptr;
			if (!Name(F, TEXT("name"), FieldName) || !Name(F, TEXT("dtype"), Field.DType)
				|| !Name(F, TEXT("unit"), Field.Unit) || !Name(F, TEXT("frame"), Field.CoordinateFrame)
				|| !Name(F, TEXT("semantic"), Field.Semantic) || !Name(F, TEXT("source"), Field.Source)
				|| !F->TryGetArrayField(TEXT("shape"), Shape)) { return false; }
			Field.Name = FName(*FieldName);
			Field.Width = 1;
			for (const TSharedPtr<FJsonValue>& Dimension : *Shape)
			{
				double D = 0;
				if (!Dimension || !Dimension->TryGetNumber(D) || !FMath::IsFinite(D) || D < 1 || D > MAX_int32
					|| FMath::FloorToDouble(D) != D || Field.Width > MAX_int32 / static_cast<int32>(D)) { return false; }
				Field.Shape.Add(static_cast<int32>(D)); Field.Width *= static_cast<int32>(D);
			}
			if (!Field.IsValid() || FieldNames.Contains(Field.Name)) { return false; }
			FieldNames.Add(Field.Name); Input.Fields.Add(MoveTemp(Field));
		}
		Parsed.Add(MoveTemp(Input));
	}
	Out = MoveTemp(Parsed); OutError.Reset(); return true;
}

TSharedRef<FJsonObject> WriteUERLInputContract(TConstArrayView<FUERLResolvedInput> Inputs)
{
	TArray<TSharedPtr<FJsonValue>> Rows;
	for (const FUERLResolvedInput& Input : Inputs)
	{
		const TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
		S->SetStringField(TEXT("name"), Input.Spec.Name);
		S->SetStringField(TEXT("provider_id"), Input.Spec.ProviderId);
		S->SetNumberField(TEXT("provider_version"), Input.Spec.ProviderVersion);
		TSharedPtr<FJsonObject> Parameters;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Input.Spec.ParametersJson), Parameters);
		S->SetObjectField(TEXT("parameters"), Parameters);
		if (Input.Spec.Attachment.IsEmpty()) { S->SetField(TEXT("attachment"), MakeShared<FJsonValueNull>()); }
		else { S->SetStringField(TEXT("attachment"), Input.Spec.Attachment); }
		S->SetArrayField(TEXT("scene_bindings"), Strings(Input.Spec.SceneBindings));
		S->SetStringField(TEXT("sampling"), Input.Spec.Sampling);
		S->SetStringField(TEXT("missing_data"), Input.Spec.MissingData);
		TArray<TSharedPtr<FJsonValue>> Fields;
		for (const FUERLFieldDescriptor& Field : Input.Fields)
		{
			const TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("name"), Field.Name.ToString());
			F->SetStringField(TEXT("dtype"), Field.DType);
			F->SetStringField(TEXT("unit"), Field.Unit);
			F->SetStringField(TEXT("frame"), Field.CoordinateFrame);
			F->SetStringField(TEXT("semantic"), Field.Semantic);
			F->SetStringField(TEXT("source"), Field.Source);
			TArray<TSharedPtr<FJsonValue>> Shape;
			for (int32 Dimension : Field.Shape) { Shape.Add(MakeShared<FJsonValueNumber>(Dimension)); }
			F->SetArrayField(TEXT("shape"), Shape);
			Fields.Add(MakeShared<FJsonValueObject>(F));
		}
		const TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
		Row->SetObjectField(TEXT("spec"), S); Row->SetArrayField(TEXT("fields"), Fields);
		Rows.Add(MakeShared<FJsonValueObject>(Row));
	}
	const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetNumberField(TEXT("version"), 1); Result->SetArrayField(TEXT("requirements"), Rows);
	return Result;
}
