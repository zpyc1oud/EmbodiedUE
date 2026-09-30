#include "UERLPolicyOperators.h"

namespace
{
	TMap<FName, FUERLPlanOperator>& OperatorMap()
	{
		static TMap<FName, FUERLPlanOperator> Registry;
		return Registry;
	}

	bool bBuiltinsRegistered = false;

	int32 SelectOutputWidth(TConstArrayView<int32> /*InputWidths*/, const FUERLPlanParams& /*Params*/)
	{
		// Width is declared on the plan op (ticket 02); source derivation is skipped.
		return 0;
	}

	int32 ConcatOutputWidth(TConstArrayView<int32> InputWidths, const FUERLPlanParams& /*Params*/)
	{
		int32 Total = 0;
		for (const int32 Width : InputWidths)
		{
			Total += Width;
		}
		return Total;
	}

	void ConcatEvaluate(const FUERLPlanOpContext& Context)
	{
		int32 Offset = 0;
		for (const TConstArrayView<float>& Input : Context.Inputs)
		{
			for (int32 Index = 0; Index < Input.Num(); ++Index)
			{
				Context.Output[Offset + Index] = Input[Index];
			}
			Offset += Input.Num();
		}
	}

	int32 Fixed3OutputWidth(TConstArrayView<int32> /*InputWidths*/, const FUERLPlanParams& /*Params*/)
	{
		return 3;
	}

	int32 Fixed1OutputWidth(TConstArrayView<int32> /*InputWidths*/, const FUERLPlanParams& /*Params*/)
	{
		return 1;
	}

	/** Plan quaternions are xyzw — FQuat(X,Y,Z,W) matches that layout. */
	FQuat QuatFromPlanXyzw(TConstArrayView<float> Q)
	{
		check(Q.Num() == 4);
		return FQuat(Q[0], Q[1], Q[2], Q[3]);
	}

	void WriteVector3(TArrayView<float> Output, const FVector& Value)
	{
		check(Output.Num() == 3);
		Output[0] = static_cast<float>(Value.X);
		Output[1] = static_cast<float>(Value.Y);
		Output[2] = static_cast<float>(Value.Z);
	}

	void EvaluateRotateInverse(const FUERLPlanOpContext& Context)
	{
		check(Context.Inputs.Num() == 2);
		const TConstArrayView<float> Q = Context.Inputs[0];
		const TConstArrayView<float> V = Context.Inputs[1];
		check(V.Num() == 3);
		const FQuat Rotation = QuatFromPlanXyzw(Q);
		WriteVector3(Context.Output, Rotation.UnrotateVector(FVector(V[0], V[1], V[2])));
	}

	void EvaluateProjectedGravity(const FUERLPlanOpContext& Context)
	{
		check(Context.Inputs.Num() == 1);
		check(Context.Params != nullptr);
		const TArray<double>* Gravity = Context.Params->Vectors.Find(TEXT("gravity"));
		checkf(
			Gravity != nullptr && Gravity->Num() == 3,
			TEXT("projected_gravity requires params.gravity as a length-3 vector"));
		const FQuat Rotation = QuatFromPlanXyzw(Context.Inputs[0]);
		const FVector WorldGravity((*Gravity)[0], (*Gravity)[1], (*Gravity)[2]);
		WriteVector3(Context.Output, Rotation.UnrotateVector(WorldGravity));
	}

	int32 IdentityOutputWidth(TConstArrayView<int32> InputWidths, const FUERLPlanParams& /*Params*/)
	{
		check(InputWidths.Num() == 1);
		return InputWidths[0];
	}

	void EvaluateJointPosRel(const FUERLPlanOpContext& Context)
	{
		check(Context.Inputs.Num() == 1);
		check(Context.Params != nullptr);
		const TArray<double>* Default = Context.Params->Vectors.Find(TEXT("default"));
		checkf(Default != nullptr, TEXT("joint_pos_rel requires params.default"));
		const TConstArrayView<float> Positions = Context.Inputs[0];
		check(Default->Num() == Positions.Num());
		check(Context.Output.Num() == Positions.Num());
		for (int32 Index = 0; Index < Positions.Num(); ++Index)
		{
			Context.Output[Index] = Positions[Index] - static_cast<float>((*Default)[Index]);
		}
	}

	void EvaluateScale(const FUERLPlanOpContext& Context)
	{
		check(Context.Inputs.Num() == 1);
		check(Context.Params != nullptr);
		const TConstArrayView<float> In = Context.Inputs[0];
		check(Context.Output.Num() == In.Num());
		if (const TArray<double>* Vector = Context.Params->Vectors.Find(TEXT("factor")))
		{
			check(Vector->Num() == In.Num());
			for (int32 Index = 0; Index < In.Num(); ++Index)
			{
				Context.Output[Index] = In[Index] * static_cast<float>((*Vector)[Index]);
			}
			return;
		}
		const double* Factor = Context.Params->Scalars.Find(TEXT("factor"));
		checkf(Factor != nullptr, TEXT("scale requires params.factor"));
		const float Scalar = static_cast<float>(*Factor);
		for (int32 Index = 0; Index < In.Num(); ++Index)
		{
			Context.Output[Index] = In[Index] * Scalar;
		}
	}

	void EvaluateOffset(const FUERLPlanOpContext& Context)
	{
		check(Context.Inputs.Num() == 1);
		check(Context.Params != nullptr);
		const TConstArrayView<float> In = Context.Inputs[0];
		check(Context.Output.Num() == In.Num());
		if (const TArray<double>* Vector = Context.Params->Vectors.Find(TEXT("bias")))
		{
			check(Vector->Num() == In.Num());
			for (int32 Index = 0; Index < In.Num(); ++Index)
			{
				Context.Output[Index] = In[Index] + static_cast<float>((*Vector)[Index]);
			}
			return;
		}
		const double* Bias = Context.Params->Scalars.Find(TEXT("bias"));
		checkf(Bias != nullptr, TEXT("offset requires params.bias"));
		const float Scalar = static_cast<float>(*Bias);
		for (int32 Index = 0; Index < In.Num(); ++Index)
		{
			Context.Output[Index] = In[Index] + Scalar;
		}
	}

	void EvaluateClip(const FUERLPlanOpContext& Context)
	{
		check(Context.Inputs.Num() == 1);
		check(Context.Params != nullptr);
		const double* Low = Context.Params->Scalars.Find(TEXT("low"));
		const double* High = Context.Params->Scalars.Find(TEXT("high"));
		checkf(Low != nullptr && High != nullptr, TEXT("clip requires params.low and params.high"));
		const float LowF = static_cast<float>(*Low);
		const float HighF = static_cast<float>(*High);
		const TConstArrayView<float> In = Context.Inputs[0];
		check(Context.Output.Num() == In.Num());
		for (int32 Index = 0; Index < In.Num(); ++Index)
		{
			const float Value = In[Index];
			// NaN passthrough: finite clip only; finiteness is an upper-layer concern.
			if (FMath::IsNaN(Value))
			{
				Context.Output[Index] = Value;
			}
			else
			{
				Context.Output[Index] = FMath::Clamp(Value, LowF, HighF);
			}
		}
	}

	int32 SliceOutputWidth(TConstArrayView<int32> /*InputWidths*/, const FUERLPlanParams& Params)
	{
		const double* Width = Params.Scalars.Find(TEXT("width"));
		checkf(Width != nullptr, TEXT("slice requires params.width"));
		return static_cast<int32>(*Width);
	}

	void EvaluateSlice(const FUERLPlanOpContext& Context)
	{
		check(Context.Inputs.Num() == 1);
		check(Context.Params != nullptr);
		const double* StartParam = Context.Params->Scalars.Find(TEXT("start"));
		const double* WidthParam = Context.Params->Scalars.Find(TEXT("width"));
		checkf(StartParam != nullptr && WidthParam != nullptr, TEXT("slice requires params.start and params.width"));
		const int32 Start = static_cast<int32>(*StartParam);
		const int32 Width = static_cast<int32>(*WidthParam);
		const TConstArrayView<float> In = Context.Inputs[0];
		check(Start >= 0 && Width > 0 && Start + Width <= In.Num());
		check(Context.Output.Num() == Width);
		for (int32 Index = 0; Index < Width; ++Index)
		{
			Context.Output[Index] = In[Start + Index];
		}
	}
}

void RegisterPlanOperator(const FUERLPlanOperator& Operator)
{
	check(!Operator.Name.IsNone());
	check(Operator.OutputWidth != nullptr);
	TMap<FName, FUERLPlanOperator>& Registry = OperatorMap();
	checkf(!Registry.Contains(Operator.Name), TEXT("duplicate plan operator '%s'"), *Operator.Name.ToString());
	Registry.Add(Operator.Name, Operator);
}

const FUERLPlanOperator* FindPlanOperator(FName Name)
{
	return OperatorMap().Find(Name);
}

void RegisterBuiltinPlanOperators()
{
	if (bBuiltinsRegistered)
	{
		return;
	}
	bBuiltinsRegistered = true;

	FUERLPlanOperator Select;
	Select.Name = TEXT("select");
	Select.Arity = 0;
	Select.OutputWidth = &SelectOutputWidth;
	Select.Evaluate = nullptr;
	RegisterPlanOperator(Select);

	FUERLPlanOperator Command;
	Command.Name = TEXT("command");
	Command.Arity = 0;
	Command.OutputWidth = &SelectOutputWidth;
	Command.Evaluate = nullptr;
	RegisterPlanOperator(Command);

	FUERLPlanOperator ControlFrameDt;
	ControlFrameDt.Name = TEXT("control_frame_dt");
	ControlFrameDt.Arity = 0;
	ControlFrameDt.OutputWidth = &Fixed1OutputWidth;
	ControlFrameDt.Evaluate = nullptr;
	RegisterPlanOperator(ControlFrameDt);

	FUERLPlanOperator PreviousAction;
	PreviousAction.Name = TEXT("previous_action");
	PreviousAction.Arity = 0;
	PreviousAction.OutputWidth = &SelectOutputWidth;
	PreviousAction.Evaluate = nullptr;
	RegisterPlanOperator(PreviousAction);

	FUERLPlanOperator PolicyAction;
	PolicyAction.Name = TEXT("policy_action");
	PolicyAction.Arity = 0;
	PolicyAction.OutputWidth = &SelectOutputWidth;
	PolicyAction.Evaluate = nullptr;
	RegisterPlanOperator(PolicyAction);

	FUERLPlanOperator Concat;
	Concat.Name = TEXT("concat");
	Concat.Arity = -1;
	Concat.OutputWidth = &ConcatOutputWidth;
	Concat.Evaluate = &ConcatEvaluate;
	RegisterPlanOperator(Concat);

	FUERLPlanOperator Slice;
	Slice.Name = TEXT("slice");
	Slice.Arity = 1;
	Slice.OutputWidth = &SliceOutputWidth;
	Slice.Evaluate = &EvaluateSlice;
	RegisterPlanOperator(Slice);

	FUERLPlanOperator RotateInverse;
	RotateInverse.Name = TEXT("rotate_inverse");
	RotateInverse.Arity = 2;
	RotateInverse.OutputWidth = &Fixed3OutputWidth;
	RotateInverse.Evaluate = &EvaluateRotateInverse;
	RegisterPlanOperator(RotateInverse);

	FUERLPlanOperator ProjectedGravity;
	ProjectedGravity.Name = TEXT("projected_gravity");
	ProjectedGravity.Arity = 1;
	ProjectedGravity.OutputWidth = &Fixed3OutputWidth;
	ProjectedGravity.Evaluate = &EvaluateProjectedGravity;
	RegisterPlanOperator(ProjectedGravity);

	FUERLPlanOperator JointPosRel;
	JointPosRel.Name = TEXT("joint_pos_rel");
	JointPosRel.Arity = 1;
	JointPosRel.OutputWidth = &IdentityOutputWidth;
	JointPosRel.Evaluate = &EvaluateJointPosRel;
	RegisterPlanOperator(JointPosRel);

	FUERLPlanOperator Scale;
	Scale.Name = TEXT("scale");
	Scale.Arity = 1;
	Scale.OutputWidth = &IdentityOutputWidth;
	Scale.Evaluate = &EvaluateScale;
	RegisterPlanOperator(Scale);

	FUERLPlanOperator Offset;
	Offset.Name = TEXT("offset");
	Offset.Arity = 1;
	Offset.OutputWidth = &IdentityOutputWidth;
	Offset.Evaluate = &EvaluateOffset;
	RegisterPlanOperator(Offset);

	FUERLPlanOperator Clip;
	Clip.Name = TEXT("clip");
	Clip.Arity = 1;
	Clip.OutputWidth = &IdentityOutputWidth;
	Clip.Evaluate = &EvaluateClip;
	RegisterPlanOperator(Clip);
}
