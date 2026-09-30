#include "UERLPolicyPlanRuntime.h"

namespace
{
	bool Fail(FString& OutError, const FString& Message)
	{
		OutError = Message;
		return false;
	}

	bool CheckArity(const FUERLPlanOperator& Op, int32 InputCount, int32 OpIndex, FString& OutError)
	{
		if (Op.Arity == -1)
		{
			return true;
		}
		if (InputCount != Op.Arity)
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("ops[%d] operator '%s' expects arity %d, got %d"),
					OpIndex,
					*Op.Name.ToString(),
					Op.Arity,
					InputCount));
		}
		return true;
	}

	bool ParamWidthMatches(const FUERLPlanOp& PlanOp, int32 OpIndex, FString& OutError)
	{
		const double* WidthParam = PlanOp.Params.Scalars.Find(TEXT("width"));
		if (WidthParam == nullptr)
		{
			return Fail(
				OutError,
				FString::Printf(TEXT("ops[%d] %s missing params.width"), OpIndex, *PlanOp.Op.ToString()));
		}
		const int32 Derived = static_cast<int32>(*WidthParam);
		if (Derived != PlanOp.Width)
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("ops[%d] operator '%s' derived width %d != declared %d"),
					OpIndex,
					*PlanOp.Op.ToString(),
					Derived,
					PlanOp.Width));
		}
		return true;
	}

	bool ControlFrameDtScaleValid(const FUERLPlanOp& PlanOp, int32 OpIndex, FString& OutError)
	{
		const double* Scale = PlanOp.Params.Scalars.Find(TEXT("scale"));
		if (Scale == nullptr || !FMath::IsFinite(*Scale) || *Scale == 0.0)
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("ops[%d] control_frame_dt scale must be finite and non-zero"),
					OpIndex));
		}
		if (PlanOp.Width != 1)
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("ops[%d] control_frame_dt width %d != declared 1"),
					OpIndex,
					PlanOp.Width));
		}
		return true;
	}
}

void FUERLPlanRuntime::Reset()
{
	bCompiled = false;
	Ops.Reset();
	SlotBuffer.Reset();
	SlotOffsets.Reset();
	SlotWidths.Reset();
	OutputSlots.Reset();
	CachedOutputWidth = 0;
	ExpectedRawStateWidth = 0;
}

int32 FUERLPlanRuntime::OutputWidth() const
{
	return CachedOutputWidth;
}

bool FUERLPlanRuntime::CompileOps(
	const TArray<FUERLPlanOp>& PlanOps,
	const TArray<FUERLFieldDescriptor>* AvailableState,
	const TMap<FName, int32>* AvailableCommandsForCommandOps,
	TMap<FName, int32>& OutSlotByName,
	FString& OutError)
{
	TMap<FName, int32> FieldOffsets;
	TMap<FName, int32> FieldWidths;
	int32 RawOffset = 0;
	if (AvailableState != nullptr)
	{
		for (const FUERLFieldDescriptor& Field : *AvailableState)
		{
			if (Field.Name.IsNone() || Field.Width <= 0)
			{
				return Fail(
					OutError,
					FString::Printf(
						TEXT("AvailableState entry '%s' has invalid name/width"),
						*Field.Name.ToString()));
			}
			if (FieldOffsets.Contains(Field.Name))
			{
				return Fail(
					OutError,
					FString::Printf(TEXT("duplicate AvailableState field '%s'"), *Field.Name.ToString()));
			}
			FieldOffsets.Add(Field.Name, RawOffset);
			FieldWidths.Add(Field.Name, Field.Width);
			RawOffset += Field.Width;
		}
	}
	ExpectedRawStateWidth = RawOffset;

	OutSlotByName.Reset();
	Ops.Reserve(PlanOps.Num());
	SlotOffsets.Reserve(PlanOps.Num());
	SlotWidths.Reserve(PlanOps.Num());

	int32 NextSlotOffset = 0;
	for (int32 OpIndex = 0; OpIndex < PlanOps.Num(); ++OpIndex)
	{
		const FUERLPlanOp& PlanOp = PlanOps[OpIndex];
		const FUERLPlanOperator* Operator = FindPlanOperator(PlanOp.Op);
		if (Operator == nullptr)
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("unregistered plan operator '%s'"),
					*PlanOp.Op.ToString()));
		}
		if (!CheckArity(*Operator, PlanOp.InputNames.Num(), OpIndex, OutError))
		{
			return false;
		}

		FCompiledOp Compiled;
		Compiled.Operator = Operator;
		Compiled.Width = PlanOp.Width;
		Compiled.Params = PlanOp.Params;
		Compiled.bIsSource = (Operator->Evaluate == nullptr);

		TArray<int32> InputWidths;
		InputWidths.Reserve(PlanOp.InputNames.Num());
		for (int32 InputIndex = 0; InputIndex < PlanOp.InputNames.Num(); ++InputIndex)
		{
			const FName InputName = PlanOp.InputNames[InputIndex];
			const int32* Slot = OutSlotByName.Find(InputName);
			if (Slot == nullptr)
			{
				return Fail(
					OutError,
					FString::Printf(
						TEXT("ops[%d].inputs[%d] references missing slot '%s'"),
						OpIndex,
						InputIndex,
						*InputName.ToString()));
			}
			Compiled.InputSlots.Add(*Slot);
			InputWidths.Add(SlotWidths[*Slot]);
		}

		if (Compiled.bIsSource)
		{
			if (PlanOp.Op == FName(TEXT("select")))
			{
				Compiled.SourceKind = ESourceKind::Select;
				const FString* FieldName = PlanOp.Params.Strings.Find(TEXT("field"));
				if (FieldName == nullptr || FieldName->IsEmpty())
				{
					return Fail(
						OutError,
						FString::Printf(TEXT("ops[%d] select missing params.field"), OpIndex));
				}
				const FName Field(*FieldName);
				const int32* Offset = FieldOffsets.Find(Field);
				if (Offset == nullptr)
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("ops[%d] select field '%s' is not in AvailableState"),
							OpIndex,
							**FieldName));
				}
				const int32 AvailableWidth = FieldWidths.FindChecked(Field);
				if (AvailableWidth != PlanOp.Width)
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("ops[%d] select field '%s' width %d != declared %d"),
							OpIndex,
							**FieldName,
							AvailableWidth,
							PlanOp.Width));
				}
				Compiled.StateOffset = *Offset;
			}
			else if (PlanOp.Op == FName(TEXT("command")))
			{
				Compiled.SourceKind = ESourceKind::Command;
				if (!ParamWidthMatches(PlanOp, OpIndex, OutError))
				{
					return false;
				}
				const FString* ChannelName = PlanOp.Params.Strings.Find(TEXT("channel"));
				if (ChannelName == nullptr || ChannelName->IsEmpty())
				{
					return Fail(
						OutError,
						FString::Printf(TEXT("ops[%d] command missing params.channel"), OpIndex));
				}
				const FName Channel(*ChannelName);
				Compiled.CommandChannel = Channel;
				if (AvailableCommandsForCommandOps != nullptr && AvailableCommandsForCommandOps->Num() > 0)
				{
					const int32* DeclaredWidth = AvailableCommandsForCommandOps->Find(Channel);
					if (DeclaredWidth == nullptr)
					{
						return Fail(
							OutError,
							FString::Printf(
								TEXT("ops[%d] command channel '%s' is not declared in AvailableCommands"),
								OpIndex,
								**ChannelName));
					}
					if (*DeclaredWidth != PlanOp.Width)
					{
						return Fail(
							OutError,
							FString::Printf(
								TEXT("ops[%d] command channel '%s' width %d != declared %d"),
								OpIndex,
								**ChannelName,
								*DeclaredWidth,
								PlanOp.Width));
					}
				}
			}
			else if (PlanOp.Op == FName(TEXT("control_frame_dt")))
			{
				Compiled.SourceKind = ESourceKind::ControlFrameDt;
				if (!ControlFrameDtScaleValid(PlanOp, OpIndex, OutError))
				{
					return false;
				}
			}
			else if (PlanOp.Op == FName(TEXT("previous_action")))
			{
				Compiled.SourceKind = ESourceKind::PreviousAction;
				if (!ParamWidthMatches(PlanOp, OpIndex, OutError))
				{
					return false;
				}
			}
			else if (PlanOp.Op == FName(TEXT("policy_action")))
			{
				Compiled.SourceKind = ESourceKind::PolicyAction;
				if (!ParamWidthMatches(PlanOp, OpIndex, OutError))
				{
					return false;
				}
			}
			else
			{
				return Fail(
					OutError,
					FString::Printf(
						TEXT("ops[%d] unhandled source operator '%s'"),
						OpIndex,
						*PlanOp.Op.ToString()));
			}
		}
		else
		{
			if (PlanOp.Op == FName(TEXT("joint_pos_rel")))
			{
				const TArray<double>* Default = PlanOp.Params.Vectors.Find(TEXT("default"));
				if (Default == nullptr)
				{
					return Fail(
						OutError,
						FString::Printf(TEXT("ops[%d] joint_pos_rel missing params.default"), OpIndex));
				}
				// Arity already enforced by CheckArity; only compare default vs input width.
				if (Default->Num() != InputWidths[0])
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("joint_pos_rel default has %d entries but its input is %d wide"),
							Default->Num(),
							InputWidths[0]));
				}
			}
			else if (PlanOp.Op == FName(TEXT("scale")))
			{
				if (const TArray<double>* Factor = PlanOp.Params.Vectors.Find(TEXT("factor")))
				{
					if (Factor->Num() != InputWidths[0])
					{
						return Fail(
							OutError,
							FString::Printf(
								TEXT("scale factor has %d entries but its input is %d wide"),
								Factor->Num(),
								InputWidths[0]));
					}
				}
				else if (!PlanOp.Params.Scalars.Contains(TEXT("factor")))
				{
					return Fail(
						OutError,
						FString::Printf(TEXT("ops[%d] scale missing params.factor"), OpIndex));
				}
			}
			else if (PlanOp.Op == FName(TEXT("offset")))
			{
				if (const TArray<double>* Bias = PlanOp.Params.Vectors.Find(TEXT("bias")))
				{
					if (Bias->Num() != InputWidths[0])
					{
						return Fail(
							OutError,
							FString::Printf(
								TEXT("offset bias has %d entries but its input is %d wide"),
								Bias->Num(),
								InputWidths[0]));
					}
				}
				else if (!PlanOp.Params.Scalars.Contains(TEXT("bias")))
				{
					return Fail(
						OutError,
						FString::Printf(TEXT("ops[%d] offset missing params.bias"), OpIndex));
				}
			}
			else if (PlanOp.Op == FName(TEXT("clip")))
			{
				const double* Low = PlanOp.Params.Scalars.Find(TEXT("low"));
				const double* High = PlanOp.Params.Scalars.Find(TEXT("high"));
				if (Low == nullptr || High == nullptr)
				{
					return Fail(
						OutError,
						FString::Printf(TEXT("ops[%d] clip missing params.low or params.high"), OpIndex));
				}
				if (*Low > *High)
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("clip low (%g) > high (%g)"),
							*Low,
							*High));
				}
			}
			else if (PlanOp.Op == FName(TEXT("slice")))
			{
				const double* StartParam = PlanOp.Params.Scalars.Find(TEXT("start"));
				const double* WidthParam = PlanOp.Params.Scalars.Find(TEXT("width"));
				if (StartParam == nullptr || WidthParam == nullptr)
				{
					return Fail(
						OutError,
						FString::Printf(TEXT("ops[%d] slice missing params.start or params.width"), OpIndex));
				}
				const int32 Start = static_cast<int32>(*StartParam);
				const int32 Width = static_cast<int32>(*WidthParam);
				if (*StartParam != Start || *WidthParam != Width || Start < 0 || Width <= 0)
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("ops[%d] slice start/width must be non-negative integers with width > 0"),
							OpIndex));
				}
				if (Start + Width > InputWidths[0])
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("slice start %d + width %d exceeds input width %d"),
							Start,
							Width,
							InputWidths[0]));
				}
			}
			const int32 Derived = Operator->OutputWidth(InputWidths, PlanOp.Params);
			if (Derived != PlanOp.Width)
			{
				return Fail(
					OutError,
					FString::Printf(
						TEXT("ops[%d] operator '%s' derived width %d != declared %d"),
						OpIndex,
						*PlanOp.Op.ToString(),
						Derived,
						PlanOp.Width));
			}
		}

		Compiled.OutputSlot = Ops.Num();
		OutSlotByName.Add(PlanOp.OutputName, Compiled.OutputSlot);
		SlotOffsets.Add(NextSlotOffset);
		SlotWidths.Add(PlanOp.Width);
		NextSlotOffset += PlanOp.Width;
		Ops.Add(MoveTemp(Compiled));
	}

	SlotBuffer.SetNumZeroed(NextSlotOffset);
	return true;
}

bool FUERLPlanRuntime::Compile(
	const FUERLObservationPlan& Plan,
	const TArray<FUERLFieldDescriptor>& AvailableState,
	FString& OutError)
{
	static const TMap<FName, int32> EmptyCommands;
	return Compile(Plan, AvailableState, EmptyCommands, OutError);
}

bool FUERLPlanRuntime::Compile(
	const FUERLObservationPlan& Plan,
	const TArray<FUERLFieldDescriptor>& AvailableState,
	const TMap<FName, int32>& AvailableCommands,
	FString& OutError)
{
	Reset();
	RegisterBuiltinPlanOperators();

	if (!Plan.Validate(OutError))
	{
		return false;
	}

	for (const FName Requirement : Plan.StateRequirements)
	{
		bool bFound = false;
		for (const FUERLFieldDescriptor& Field : AvailableState)
		{
			if (Field.Name == Requirement)
			{
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			return Fail(
				OutError,
				FString::Printf(
					TEXT("state requirement '%s' is not in AvailableState"),
					*Requirement.ToString()));
		}
	}

	TMap<FName, int32> SlotByName;
	if (!CompileOps(Plan.Ops, &AvailableState, &AvailableCommands, SlotByName, OutError))
	{
		Reset();
		return false;
	}

	const TArray<FName>* PolicyMembers = Plan.Groups.Find(TEXT("policy"));
	if (PolicyMembers == nullptr)
	{
		Reset();
		return Fail(OutError, TEXT("observation plan is missing required 'policy' group"));
	}
	const int32* DeclaredPolicyWidth = Plan.GroupWidths.Find(TEXT("policy"));
	if (DeclaredPolicyWidth == nullptr)
	{
		Reset();
		return Fail(OutError, TEXT("observation plan is missing group_widths.policy"));
	}

	OutputSlots.Reset();
	int32 PolicyWidth = 0;
	for (int32 MemberIndex = 0; MemberIndex < PolicyMembers->Num(); ++MemberIndex)
	{
		const FName Member = (*PolicyMembers)[MemberIndex];
		const int32* Slot = SlotByName.Find(Member);
		if (Slot == nullptr)
		{
			Reset();
			return Fail(
				OutError,
				FString::Printf(
					TEXT("policy group member '%s' is not produced by any op"),
					*Member.ToString()));
		}
		OutputSlots.Add(*Slot);
		PolicyWidth += SlotWidths[*Slot];
	}
	if (PolicyWidth != *DeclaredPolicyWidth)
	{
		Reset();
		return Fail(
			OutError,
			FString::Printf(
				TEXT("policy group width %d != declared %d"),
				PolicyWidth,
				*DeclaredPolicyWidth));
	}

	CachedOutputWidth = PolicyWidth;
	bCompiled = true;
	OutError.Reset();
	return true;
}

bool FUERLPlanRuntime::Compile(
	const FUERLActionPlan& Plan,
	const TMap<FName, int32>& AvailableCommands,
	FString& OutError)
{
	Reset();
	RegisterBuiltinPlanOperators();

	if (!Plan.Validate(OutError))
	{
		return false;
	}

	TMap<FName, int32> SlotByName;
	if (!CompileOps(Plan.Ops, nullptr, nullptr, SlotByName, OutError))
	{
		Reset();
		return false;
	}

	for (int32 OpIndex = 0; OpIndex < Plan.Ops.Num(); ++OpIndex)
	{
		const FUERLPlanOp& PlanOp = Plan.Ops[OpIndex];
		if (PlanOp.Op == FName(TEXT("policy_action")) && PlanOp.Width != Plan.PolicyWidth)
		{
			Reset();
			return Fail(
				OutError,
				FString::Printf(
					TEXT("ops[%d] policy_action width %d != plan policy_width %d"),
					OpIndex,
					PlanOp.Width,
					Plan.PolicyWidth));
		}
	}

	OutputSlots.Reset();
	int32 TotalWidth = 0;
	for (int32 FieldIndex = 0; FieldIndex < Plan.CommandFields.Num(); ++FieldIndex)
	{
		const FName FieldName = Plan.CommandFields[FieldIndex];
		const int32* Slot = SlotByName.Find(FieldName);
		if (Slot == nullptr)
		{
			Reset();
			return Fail(
				OutError,
				FString::Printf(
					TEXT("command_fields[%d] references unknown slot '%s'"),
					FieldIndex,
					*FieldName.ToString()));
		}
		const int32 ActualWidth = SlotWidths[*Slot];
		if (AvailableCommands.Num() > 0)
		{
			const int32* DeclaredWidth = AvailableCommands.Find(FieldName);
			if (DeclaredWidth == nullptr)
			{
				Reset();
				return Fail(
					OutError,
					FString::Printf(
						TEXT("command field '%s' is not declared in AvailableCommands"),
						*FieldName.ToString()));
			}
			if (*DeclaredWidth != ActualWidth)
			{
				Reset();
				return Fail(
					OutError,
					FString::Printf(
						TEXT("command field '%s' width %d != declared %d"),
						*FieldName.ToString(),
						ActualWidth,
						*DeclaredWidth));
			}
		}
		OutputSlots.Add(*Slot);
		TotalWidth += ActualWidth;
	}

	CachedOutputWidth = TotalWidth;
	bCompiled = true;
	OutError.Reset();
	return true;
}

bool FUERLPlanRuntime::Execute(const FUERLPlanInputs& Inputs, TArray<float>& OutValues, FString& OutError)
{
	if (!bCompiled)
	{
		return Fail(OutError, TEXT("plan runtime is not compiled"));
	}
	if (Inputs.RawState.Num() != ExpectedRawStateWidth)
	{
		return Fail(
			OutError,
			FString::Printf(
				TEXT("RawState width %d != compiled AvailableState width %d"),
				Inputs.RawState.Num(),
				ExpectedRawStateWidth));
	}

	for (FCompiledOp& Op : Ops)
	{
		const int32 OutOffset = SlotOffsets[Op.OutputSlot];
		TArrayView<float> OutputView(SlotBuffer.GetData() + OutOffset, Op.Width);

		if (Op.bIsSource)
		{
			if (Op.SourceKind == ESourceKind::Select)
			{
				for (int32 Index = 0; Index < Op.Width; ++Index)
				{
					OutputView[Index] = Inputs.RawState[Op.StateOffset + Index];
				}
			}
			else if (Op.SourceKind == ESourceKind::Command)
			{
				const TArray<float>* Channel = Inputs.Commands.Find(Op.CommandChannel);
				if (Channel == nullptr)
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("command channel '%s' was not supplied"),
							*Op.CommandChannel.ToString()));
				}
				if (Channel->Num() != Op.Width)
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("command channel '%s' width %d != declared %d"),
							*Op.CommandChannel.ToString(),
							Channel->Num(),
							Op.Width));
				}
				for (int32 Index = 0; Index < Op.Width; ++Index)
				{
					OutputView[Index] = (*Channel)[Index];
				}
			}
			else if (Op.SourceKind == ESourceKind::ControlFrameDt)
			{
				if (!FMath::IsFinite(Inputs.ControlFrameDtSeconds) || Inputs.ControlFrameDtSeconds <= 0.0f)
				{
					return Fail(
						OutError,
						TEXT("control_frame_dt must be finite and positive"));
				}
				const double* Scale = Op.Params.Scalars.Find(TEXT("scale"));
				if (Scale == nullptr || !FMath::IsFinite(*Scale) || *Scale == 0.0)
				{
					return Fail(
						OutError,
						TEXT("control_frame_dt scale must be finite and non-zero"));
				}
				OutputView[0] = Inputs.ControlFrameDtSeconds * static_cast<float>(*Scale);
			}
			else if (Op.SourceKind == ESourceKind::PreviousAction)
			{
				if (Inputs.PreviousAction.Num() == 0)
				{
					return Fail(OutError, TEXT("plan requires previous_action but none was supplied"));
				}
				if (Inputs.PreviousAction.Num() != Op.Width)
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("previous_action width %d != declared %d"),
							Inputs.PreviousAction.Num(),
							Op.Width));
				}
				for (int32 Index = 0; Index < Op.Width; ++Index)
				{
					OutputView[Index] = Inputs.PreviousAction[Index];
				}
			}
			else if (Op.SourceKind == ESourceKind::PolicyAction)
			{
				if (Inputs.PolicyAction.Num() == 0)
				{
					return Fail(OutError, TEXT("plan requires policy_action but none was supplied"));
				}
				if (Inputs.PolicyAction.Num() != Op.Width)
				{
					return Fail(
						OutError,
						FString::Printf(
							TEXT("policy_action width %d != declared %d"),
							Inputs.PolicyAction.Num(),
							Op.Width));
				}
				for (int32 Index = 0; Index < Op.Width; ++Index)
				{
					OutputView[Index] = Inputs.PolicyAction[Index];
				}
			}
			else
			{
				return Fail(OutError, TEXT("compiled source op has unknown SourceKind"));
			}
			continue;
		}

		FUERLPlanOpContext Context;
		Context.Params = &Op.Params;
		Context.Output = OutputView;
		Context.Inputs.Reserve(Op.InputSlots.Num());
		for (const int32 InputSlot : Op.InputSlots)
		{
			const int32 InOffset = SlotOffsets[InputSlot];
			Context.Inputs.Add(TConstArrayView<float>(SlotBuffer.GetData() + InOffset, SlotWidths[InputSlot]));
		}
		check(Op.Operator != nullptr && Op.Operator->Evaluate != nullptr);
		Op.Operator->Evaluate(Context);
	}

	OutValues.SetNumUninitialized(CachedOutputWidth);
	int32 Write = 0;
	for (const int32 Slot : OutputSlots)
	{
		const int32 Offset = SlotOffsets[Slot];
		const int32 Width = SlotWidths[Slot];
		for (int32 Index = 0; Index < Width; ++Index)
		{
			OutValues[Write++] = SlotBuffer[Offset + Index];
		}
	}
	OutError.Reset();
	return true;
}
