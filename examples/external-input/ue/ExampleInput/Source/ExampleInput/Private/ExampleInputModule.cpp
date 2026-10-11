#include "Modules/ModuleManager.h"
#include "UERLInputProvider.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace ExampleInput
{
	bool ReadValue(const FUERLInputSpec& Spec, float& OutValue, FString& OutError)
	{
		TSharedPtr<FJsonObject> Json;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Spec.ParametersJson), Json) || !Json)
		{
			OutError = TEXT("constant parameters must be an object"); return false;
		}
		for (const auto& Pair : Json->Values)
		{
			if (Pair.Key != TEXT("value")) { OutError = TEXT("constant input accepts only value"); return false; }
		}
		double Value = 1.0;
		if ((Json->HasField(TEXT("value")) && !Json->TryGetNumberField(TEXT("value"), Value))
			|| !FMath::IsFinite(Value) || FMath::Abs(Value) > MAX_flt)
		{
			OutError = TEXT("constant value must be a finite float32 number"); return false;
		}
		OutValue = static_cast<float>(Value); return true;
	}
	class FProvider final : public IUERLInputProvider
	{
	public:
		explicit FProvider(float InValue) : Value(InValue) {}
		bool Bind(TConstArrayView<FUERLInputSlotBinding>, FString&) override { return true; }
		bool Sample(int32, const FUERLInputSampleStamp&, TArrayView<float> Values, FString& Error) override
		{
			if (Values.Num() != 1) { Error = TEXT("constant output width must be one"); return false; }
			Values[0] = Value; return true;
		}
		bool Reset(TConstArrayView<int32>, FString&) override { return true; }
		void Release() override {}
	private:
		float Value;
	};
	class FFactory final : public IUERLInputFactory
	{
	public:
		FString GetId() const override { return TEXT("example.constant"); }
		int32 GetVersion() const override { return 1; }
		bool Validate(const FUERLInputSpec& Spec, FUERLInputSpec& Effective,
			TArray<FUERLFieldDescriptor>& Fields, FString& Error) const override
		{
			if (!Spec.Attachment.IsEmpty() || !Spec.SceneBindings.IsEmpty())
			{
				Error = TEXT("constant input does not use scene bindings"); return false;
			}
			float Value;
			if (!ReadValue(Spec, Value, Error)) { return false; }
			Effective = Spec;
			TSharedRef<FJsonObject> Parameters = MakeShared<FJsonObject>();
			Parameters->SetNumberField(TEXT("value"), Value);
			Effective.ParametersJson.Reset();
			FJsonSerializer::Serialize(Parameters, TJsonWriterFactory<>::Create(&Effective.ParametersJson));
			FUERLFieldDescriptor Field;
			Field.Name = FName(*FString::Printf(TEXT("input.%s.value"), *Spec.Name));
			Field.Shape = {1}; Field.Width = 1; Field.Unit = TEXT("1"); Field.CoordinateFrame = TEXT("none");
			Field.Semantic = TEXT("constant"); Field.Source = GetId(); Fields.Add(Field);
			return true;
		}
		TUniquePtr<IUERLInputProvider> Create(const FUERLInputSpec& Spec) const override
		{
			float Value; FString Error;
			if (!ReadValue(Spec, Value, Error)) { return nullptr; }
			return MakeUnique<FProvider>(Value);
		}
	};
}

class FExampleInputModule final : public IModuleInterface
{
public:
	void StartupModule() override
	{
		FString Error;
		bRegistered = FUERLInputRegistry::Get().RegisterFactory(MakeShared<ExampleInput::FFactory>(), Error);
		if (!bRegistered) { UE_LOG(LogTemp, Error, TEXT("ExampleInput: %s"), *Error); }
	}
	void ShutdownModule() override
	{
		if (bRegistered) { FUERLInputRegistry::Get().UnregisterFactory(TEXT("example.constant"), 1); }
	}
private:
	bool bRegistered = false;
};
IMPLEMENT_MODULE(FExampleInputModule, ExampleInput);
