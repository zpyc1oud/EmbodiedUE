#include "UERLPlan.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace
{
	constexpr int32 GUERLPlanVersion = 1;

	bool Fail(FString& OutError, const FString& Message)
	{
		OutError = Message;
		return false;
	}

	bool RequireObject(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Context,
		FString& OutError)
	{
		if (!Object.IsValid())
		{
			return Fail(OutError, FString::Printf(TEXT("%s must be a JSON object"), Context));
		}
		return true;
	}

	bool ParseNameArray(
		const TArray<TSharedPtr<FJsonValue>>* Values,
		const TCHAR* Context,
		TArray<FName>& OutNames,
		FString& OutError)
	{
		OutNames.Reset();
		if (!Values)
		{
			return Fail(OutError, FString::Printf(TEXT("%s must be an array"), Context));
		}
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			FString Text;
			if (!Value.IsValid() || !Value->TryGetString(Text) || Text.IsEmpty())
			{
				return Fail(
					OutError,
					FString::Printf(TEXT("%s[%d] must be a non-empty string"), Context, Index));
			}
			OutNames.Add(FName(*Text));
		}
		return true;
	}

	bool ParsePositiveInt(const TSharedPtr<FJsonValue>& Value, const TCHAR* Context, int32& Out, FString& OutError)
	{
		double Number = 0.0;
		if (!Value.IsValid() || !Value->TryGetNumber(Number))
		{
			return Fail(OutError, FString::Printf(TEXT("%s must be a number"), Context));
		}
		const int32 AsInt = static_cast<int32>(Number);
		if (static_cast<double>(AsInt) != Number || AsInt <= 0)
		{
			return Fail(OutError, FString::Printf(TEXT("%s must be a positive integer"), Context));
		}
		Out = AsInt;
		return true;
	}

	bool ParsePlanVersion(const TSharedRef<FJsonObject>& Json, int32& OutVersion, FString& OutError)
	{
		const TSharedPtr<FJsonValue> VersionValue = Json->TryGetField(TEXT("plan_version"));
		if (!VersionValue.IsValid())
		{
			return Fail(OutError, TEXT("missing required field 'plan_version'"));
		}
		double Number = 0.0;
		if (!VersionValue->TryGetNumber(Number))
		{
			return Fail(OutError, TEXT("plan_version must be an integer"));
		}
		const int32 Version = static_cast<int32>(Number);
		if (static_cast<double>(Version) != Number)
		{
			return Fail(OutError, TEXT("plan_version must be an integer"));
		}
		if (Version != GUERLPlanVersion)
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("unsupported plan_version %d; expected %d"),
					Version,
					GUERLPlanVersion));
		}
		OutVersion = Version;
		return true;
	}

	bool ParseParams(const TSharedPtr<FJsonObject>& Object, FUERLPlanParams& OutParams, FString& OutError)
	{
		OutParams.Reset();
		if (!Object.IsValid())
		{
			return true;
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			const FName Key(*Pair.Key);
			const TSharedPtr<FJsonValue>& Value = Pair.Value;
			if (!Value.IsValid())
			{
				return Fail(OutError, FString::Printf(TEXT("params.%s is null"), *Pair.Key));
			}
			if (Value->Type == EJson::Boolean)
			{
				OutParams.Scalars.Add(Key, Value->AsBool() ? 1.0 : 0.0);
				continue;
			}
			if (Value->Type == EJson::Number)
			{
				OutParams.Scalars.Add(Key, Value->AsNumber());
				continue;
			}
			if (Value->Type == EJson::String)
			{
				OutParams.Strings.Add(Key, Value->AsString());
				continue;
			}
			if (Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& Items = Value->AsArray();
				TArray<double> Vector;
				Vector.Reserve(Items.Num());
				for (int32 Index = 0; Index < Items.Num(); ++Index)
				{
					double Number = 0.0;
					if (!Items[Index].IsValid() || !Items[Index]->TryGetNumber(Number))
					{
						return Fail(
							OutError,
							FString::Printf(TEXT("params.%s[%d] must be a number"), *Pair.Key, Index));
					}
					Vector.Add(Number);
				}
				OutParams.Vectors.Add(Key, MoveTemp(Vector));
				continue;
			}
			return Fail(OutError, FString::Printf(TEXT("unsupported params.%s type"), *Pair.Key));
		}
		return true;
	}

	bool ParseOps(
		const TArray<TSharedPtr<FJsonValue>>* Values,
		TArray<FUERLPlanOp>& OutOps,
		FString& OutError)
	{
		OutOps.Reset();
		if (!Values)
		{
			return Fail(OutError, TEXT("ops must be an array"));
		}
		for (int32 Index = 0; Index < Values->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Values)[Index];
			const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(ObjectPtr) || !ObjectPtr || !ObjectPtr->IsValid())
			{
				return Fail(OutError, FString::Printf(TEXT("ops[%d] must be an object"), Index));
			}
			const TSharedPtr<FJsonObject>& Object = *ObjectPtr;
			FUERLPlanOp Op;
			FString OpName;
			if (!Object->TryGetStringField(TEXT("op"), OpName) || OpName.IsEmpty())
			{
				return Fail(OutError, FString::Printf(TEXT("ops[%d].op must be a non-empty string"), Index));
			}
			Op.Op = FName(*OpName);
			FString OutputName;
			if (!Object->TryGetStringField(TEXT("output"), OutputName) || OutputName.IsEmpty())
			{
				return Fail(
					OutError,
					FString::Printf(TEXT("ops[%d].output must be a non-empty string"), Index));
			}
			Op.OutputName = FName(*OutputName);
			const TSharedPtr<FJsonValue> WidthValue = Object->TryGetField(TEXT("width"));
			if (!ParsePositiveInt(
					WidthValue,
					*FString::Printf(TEXT("ops[%d].width"), Index),
					Op.Width,
					OutError))
			{
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>* Inputs = nullptr;
			if (Object->HasField(TEXT("inputs")))
			{
				if (!Object->TryGetArrayField(TEXT("inputs"), Inputs))
				{
					return Fail(OutError, FString::Printf(TEXT("ops[%d].inputs must be an array"), Index));
				}
			}
			else
			{
				static const TArray<TSharedPtr<FJsonValue>> EmptyInputs;
				Inputs = &EmptyInputs;
			}
			if (!ParseNameArray(
					Inputs,
					*FString::Printf(TEXT("ops[%d].inputs"), Index),
					Op.InputNames,
					OutError))
			{
				return false;
			}
			const TSharedPtr<FJsonObject>* ParamsObject = nullptr;
			if (Object->HasField(TEXT("params")))
			{
				if (!Object->TryGetObjectField(TEXT("params"), ParamsObject) || !ParamsObject)
				{
					return Fail(OutError, FString::Printf(TEXT("ops[%d].params must be an object"), Index));
				}
				if (!ParseParams(*ParamsObject, Op.Params, OutError))
				{
					return false;
				}
			}
			OutOps.Add(MoveTemp(Op));
		}
		return true;
	}

	bool HasCycle(const TArray<TArray<int32>>& Dependents)
	{
		enum class EColor : uint8
		{
			White,
			Gray,
			Black
		};
		TArray<EColor> Color;
		Color.Init(EColor::White, Dependents.Num());
		TArray<int32> Stack;
		TArray<int32> Iterator;
		Stack.Reserve(Dependents.Num());
		Iterator.Reserve(Dependents.Num());

		for (int32 Start = 0; Start < Dependents.Num(); ++Start)
		{
			if (Color[Start] != EColor::White)
			{
				continue;
			}
			Stack.Reset();
			Iterator.Reset();
			Stack.Add(Start);
			Iterator.Add(0);
			Color[Start] = EColor::Gray;
			while (!Stack.IsEmpty())
			{
				const int32 Node = Stack.Last();
				int32& NextIndex = Iterator.Last();
				if (NextIndex < Dependents[Node].Num())
				{
					const int32 Next = Dependents[Node][NextIndex++];
					if (Color[Next] == EColor::Gray)
					{
						return true;
					}
					if (Color[Next] == EColor::White)
					{
						Color[Next] = EColor::Gray;
						Stack.Add(Next);
						Iterator.Add(0);
					}
					continue;
				}
				Color[Node] = EColor::Black;
				Stack.Pop();
				Iterator.Pop();
			}
		}
		return false;
	}

	bool ValidateOps(
		const TArray<FUERLPlanOp>& Ops,
		const TSet<FName>& AvailableSources,
		FString& OutError)
	{
		TMap<FName, int32> Produced;
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			const FUERLPlanOp& Op = Ops[Index];
			if (Op.Op.IsNone())
			{
				return Fail(OutError, FString::Printf(TEXT("ops[%d].op must be non-empty"), Index));
			}
			if (Op.Width <= 0)
			{
				return Fail(OutError, FString::Printf(TEXT("ops[%d].width must be positive"), Index));
			}
			if (Op.OutputName.IsNone())
			{
				return Fail(OutError, FString::Printf(TEXT("ops[%d].output must be non-empty"), Index));
			}
			if (Produced.Contains(Op.OutputName) || AvailableSources.Contains(Op.OutputName))
			{
				return Fail(
					OutError,
					FString::Printf(TEXT("duplicate slot name '%s'"), *Op.OutputName.ToString()));
			}
			Produced.Add(Op.OutputName, Op.Width);
		}

		TSet<FName> AllSlots = AvailableSources;
		for (const TPair<FName, int32>& Pair : Produced)
		{
			AllSlots.Add(Pair.Key);
		}

		TMap<FName, int32> OutputIndex;
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			OutputIndex.Add(Ops[Index].OutputName, Index);
		}

		TArray<TArray<int32>> Dependents;
		Dependents.SetNum(Ops.Num());
		for (int32 Consumer = 0; Consumer < Ops.Num(); ++Consumer)
		{
			const FUERLPlanOp& Op = Ops[Consumer];
			for (int32 InputIndex = 0; InputIndex < Op.InputNames.Num(); ++InputIndex)
			{
				const FName Name = Op.InputNames[InputIndex];
				if (!AllSlots.Contains(Name))
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("ops[%d].inputs[%d] references unknown slot '%s'"),
							Consumer,
							InputIndex,
							*Name.ToString()));
				}
				if (const int32* Producer = OutputIndex.Find(Name))
				{
					Dependents[*Producer].Add(Consumer);
				}
			}
		}

		if (HasCycle(Dependents))
		{
			return Fail(OutError, TEXT("plan ops contain a cyclic dependency"));
		}

		TSet<FName> Available = AvailableSources;
		for (int32 Index = 0; Index < Ops.Num(); ++Index)
		{
			const FUERLPlanOp& Op = Ops[Index];
			for (int32 InputIndex = 0; InputIndex < Op.InputNames.Num(); ++InputIndex)
			{
				const FName Name = Op.InputNames[InputIndex];
				if (!Available.Contains(Name))
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("ops[%d].inputs[%d] is not available in topological order ('%s')"),
							Index,
							InputIndex,
							*Name.ToString()));
				}
			}
			Available.Add(Op.OutputName);
		}
		return true;
	}
}

void FUERLObservationPlan::Reset()
{
	PlanVersion = GUERLPlanVersion;
	StateRequirements.Reset();
	Ops.Reset();
	Groups.Reset();
	GroupWidths.Reset();
}

bool FUERLObservationPlan::ParseJson(const TSharedRef<FJsonObject>& Json, FString& OutError)
{
	Reset();
	if (!ParsePlanVersion(Json, PlanVersion, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* StateRequirementsJson = nullptr;
	if (!Json->TryGetArrayField(TEXT("state_requirements"), StateRequirementsJson))
	{
		return Fail(OutError, TEXT("state_requirements must be an array"));
	}
	if (!ParseNameArray(StateRequirementsJson, TEXT("state_requirements"), StateRequirements, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* OpsJson = nullptr;
	if (!Json->TryGetArrayField(TEXT("ops"), OpsJson))
	{
		return Fail(OutError, TEXT("ops must be an array"));
	}
	if (!ParseOps(OpsJson, Ops, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonObject>* GroupsObject = nullptr;
	if (!Json->TryGetObjectField(TEXT("groups"), GroupsObject) || !RequireObject(*GroupsObject, TEXT("groups"), OutError))
	{
		return Fail(OutError, TEXT("groups must be an object"));
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*GroupsObject)->Values)
	{
		if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Array)
		{
			return Fail(OutError, FString::Printf(TEXT("groups.%s must be an array"), *Pair.Key));
		}
		const TArray<TSharedPtr<FJsonValue>>& Members = Pair.Value->AsArray();
		TArray<FName> Names;
		if (!ParseNameArray(&Members, *FString::Printf(TEXT("groups.%s"), *Pair.Key), Names, OutError))
		{
			return false;
		}
		Groups.Add(FName(*Pair.Key), MoveTemp(Names));
	}

	const TSharedPtr<FJsonObject>* WidthsObject = nullptr;
	if (!Json->TryGetObjectField(TEXT("group_widths"), WidthsObject)
		|| !RequireObject(*WidthsObject, TEXT("group_widths"), OutError))
	{
		return Fail(OutError, TEXT("group_widths must be an object"));
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*WidthsObject)->Values)
	{
		int32 Width = 0;
		if (!ParsePositiveInt(Pair.Value, *FString::Printf(TEXT("group_widths.%s"), *Pair.Key), Width, OutError))
		{
			return false;
		}
		GroupWidths.Add(FName(*Pair.Key), Width);
	}

	return Validate(OutError);
}

bool FUERLObservationPlan::Validate(FString& OutError) const
{
	TSet<FName> Sources;
	for (const FName& Name : StateRequirements)
	{
		if (Name.IsNone())
		{
			return Fail(OutError, TEXT("state_requirements entries must be non-empty"));
		}
		Sources.Add(Name);
	}
	if (!ValidateOps(Ops, Sources, OutError))
	{
		return false;
	}

	TMap<FName, int32> Produced;
	for (const FUERLPlanOp& Op : Ops)
	{
		Produced.Add(Op.OutputName, Op.Width);
	}

	if (Groups.Num() != GroupWidths.Num())
	{
		return Fail(OutError, TEXT("groups and group_widths must declare the same keys"));
	}
	for (const TPair<FName, TArray<FName>>& Pair : Groups)
	{
		const int32* Declared = GroupWidths.Find(Pair.Key);
		if (!Declared)
		{
			return Fail(
				OutError,
				FString::Printf(TEXT("group '%s' missing from group_widths"), *Pair.Key.ToString()));
		}
		int32 Total = 0;
		for (int32 Index = 0; Index < Pair.Value.Num(); ++Index)
		{
			const FName Member = Pair.Value[Index];
			const int32* Width = Produced.Find(Member);
			if (!Width)
			{
				return Fail(
					OutError,
					FString::Printf(
						TEXT("groups.%s[%d] references unknown slot '%s'"),
						*Pair.Key.ToString(),
						Index,
						*Member.ToString()));
			}
			Total += *Width;
		}
		if (Total != *Declared)
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("group '%s' width %d != sum of member widths %d"),
					*Pair.Key.ToString(),
					*Declared,
					Total));
		}
	}
	for (const TPair<FName, int32>& Pair : GroupWidths)
	{
		if (!Groups.Contains(Pair.Key))
		{
			return Fail(
				OutError,
				FString::Printf(TEXT("group_widths.%s has no matching groups entry"), *Pair.Key.ToString()));
		}
	}
	return true;
}

void FUERLActionPlan::Reset()
{
	PlanVersion = GUERLPlanVersion;
	Ops.Reset();
	CommandFields.Reset();
	PolicyWidth = 0;
}

bool FUERLActionPlan::ParseJson(const TSharedRef<FJsonObject>& Json, FString& OutError)
{
	Reset();
	if (!ParsePlanVersion(Json, PlanVersion, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* OpsJson = nullptr;
	if (!Json->TryGetArrayField(TEXT("ops"), OpsJson) || !ParseOps(OpsJson, Ops, OutError))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* CommandFieldsJson = nullptr;
	if (!Json->TryGetArrayField(TEXT("command_fields"), CommandFieldsJson))
	{
		return Fail(OutError, TEXT("command_fields must be an array"));
	}
	if (!ParseNameArray(CommandFieldsJson, TEXT("command_fields"), CommandFields, OutError))
	{
		return false;
	}

	const TSharedPtr<FJsonValue> WidthValue = Json->TryGetField(TEXT("policy_width"));
	if (!ParsePositiveInt(WidthValue, TEXT("policy_width"), PolicyWidth, OutError))
	{
		return false;
	}

	return Validate(OutError);
}

bool FUERLActionPlan::Validate(FString& OutError) const
{
	if (PolicyWidth <= 0)
	{
		return Fail(OutError, TEXT("policy_width must be positive"));
	}
	TSet<FName> EmptySources;
	if (!ValidateOps(Ops, EmptySources, OutError))
	{
		return false;
	}
	TSet<FName> Produced;
	for (const FUERLPlanOp& Op : Ops)
	{
		Produced.Add(Op.OutputName);
	}
	for (int32 Index = 0; Index < CommandFields.Num(); ++Index)
	{
		if (!Produced.Contains(CommandFields[Index]))
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("command_fields[%d] references unknown slot '%s'"),
					Index,
					*CommandFields[Index].ToString()));
		}
	}
	return true;
}
