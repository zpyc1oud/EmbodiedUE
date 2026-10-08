#include "Misc/AutomationTest.h"

#include "UERLInterfaceTypes.h"
#include "UERLPlan.h"
#include "UERLPolicyOperators.h"
#include "UERLPolicyPlanRuntime.h"

#include <limits>

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	FUERLFieldDescriptor MakeStateField(const TCHAR* Name, int32 Width)
	{
		FUERLFieldDescriptor Field;
		Field.Name = FName(Name);
		Field.DType = TEXT("float32");
		Field.Shape = {Width};
		Field.Unit = TEXT("1");
		Field.CoordinateFrame = TEXT("world");
		Field.Semantic = TEXT("test");
		Field.Source = TEXT("robot");
		Field.Width = Width;
		return Field;
	}

	FUERLPlanOp MakeSelect(const TCHAR* FieldName, const TCHAR* Output, int32 Width)
	{
		FUERLPlanOp Op;
		Op.Op = TEXT("select");
		Op.OutputName = FName(Output);
		Op.Width = Width;
		Op.Params.Strings.Add(TEXT("field"), FieldName);
		return Op;
	}

	FUERLPlanOp MakeConcat(const TArray<FName>& Inputs, const TCHAR* Output, int32 Width)
	{
		FUERLPlanOp Op;
		Op.Op = TEXT("concat");
		Op.InputNames = Inputs;
		Op.OutputName = FName(Output);
		Op.Width = Width;
		return Op;
	}

	FUERLObservationPlan MakeSelectConcatPlan()
	{
		FUERLObservationPlan Plan;
		Plan.PlanVersion = 1;
		Plan.StateRequirements = {
			TEXT("robot.body.base_link.body_pose"),
			TEXT("robot.joint.left_c1.joint_position"),
		};
		Plan.Ops = {
			MakeSelect(TEXT("robot.body.base_link.body_pose"), TEXT("pose"), 7),
			MakeSelect(TEXT("robot.joint.left_c1.joint_position"), TEXT("joint"), 1),
			MakeConcat({TEXT("pose"), TEXT("joint")}, TEXT("policy_vec"), 8),
		};
		Plan.Groups.Add(TEXT("policy"), {TEXT("policy_vec")});
		Plan.GroupWidths.Add(TEXT("policy"), 8);
		return Plan;
	}

	FUERLObservationPlan MakeControlFrameDtPlan(double Scale, int32 Width = 1)
	{
		FUERLObservationPlan Plan;
		Plan.PlanVersion = 1;
		FUERLPlanOp Dt;
		Dt.Op = TEXT("control_frame_dt");
		Dt.OutputName = TEXT("dt");
		Dt.Width = Width;
		Dt.Params.Scalars.Add(TEXT("scale"), Scale);
		Plan.Ops = {Dt};
		Plan.Groups.Add(TEXT("policy"), {TEXT("dt")});
		Plan.GroupWidths.Add(TEXT("policy"), Width);
		return Plan;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimeOutputWidthTest,
	"UERL.Unit.Policy.PlanRuntime.AC_UE_UNIT_POLICY_001.CompileOutputWidth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimeOutputWidthTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();

	const FUERLObservationPlan Plan = MakeSelectConcatPlan();
	const TArray<FUERLFieldDescriptor> Available = {
		MakeStateField(TEXT("robot.body.base_link.body_pose"), 7),
		MakeStateField(TEXT("robot.joint.left_c1.joint_position"), 1),
	};

	FUERLPlanRuntime Runtime;
	FString Error;
	TestTrue(TEXT("Compile succeeds"), Runtime.Compile(Plan, Available, Error));
	if (!Error.IsEmpty())
	{
		AddError(Error);
	}
	TestEqual(TEXT("OutputWidth matches policy group"), Runtime.OutputWidth(), 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimeUnregisteredOpTest,
	"UERL.Unit.Policy.PlanRuntime.AC_UE_UNIT_POLICY_002.UnregisteredOperator",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimeUnregisteredOpTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();

	FUERLObservationPlan Plan;
	Plan.PlanVersion = 1;
	Plan.StateRequirements = {TEXT("robot.body.base_link.body_pose")};
	FUERLPlanOp Bogus;
	Bogus.Op = TEXT("not_a_real_op");
	Bogus.OutputName = TEXT("out");
	Bogus.Width = 7;
	Plan.Ops = {Bogus};
	Plan.Groups.Add(TEXT("policy"), {TEXT("out")});
	Plan.GroupWidths.Add(TEXT("policy"), 7);

	const TArray<FUERLFieldDescriptor> Available = {
		MakeStateField(TEXT("robot.body.base_link.body_pose"), 7),
	};

	FUERLPlanRuntime Runtime;
	FString Error;
	TestFalse(TEXT("Compile rejects unregistered op"), Runtime.Compile(Plan, Available, Error));
	TestTrue(TEXT("error names the operator"), Error.Contains(TEXT("not_a_real_op")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimeCompileErrorsTest,
	"UERL.Unit.Policy.PlanRuntime.AC_UE_UNIT_POLICY_003.CompileErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimeCompileErrorsTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();
	const TArray<FUERLFieldDescriptor> Available = {
		MakeStateField(TEXT("robot.body.base_link.body_pose"), 7),
		MakeStateField(TEXT("robot.joint.left_c1.joint_position"), 1),
	};

	{
		// Missing slot reference: concat input names a slot that no op produces.
		FUERLObservationPlan Plan;
		Plan.PlanVersion = 1;
		Plan.StateRequirements = {TEXT("robot.body.base_link.body_pose")};
		Plan.Ops = {
			MakeSelect(TEXT("robot.body.base_link.body_pose"), TEXT("pose"), 7),
			MakeConcat({TEXT("pose"), TEXT("missing_slot")}, TEXT("out"), 8),
		};
		Plan.Groups.Add(TEXT("policy"), {TEXT("out")});
		Plan.GroupWidths.Add(TEXT("policy"), 8);

		FUERLPlanRuntime Runtime;
		FString Error;
		// Structural Validate fails first on unknown slot — still a compile error.
		TestFalse(TEXT("missing slot fails Compile"), Runtime.Compile(Plan, Available, Error));
		TestTrue(TEXT("missing slot mentioned"), Error.Contains(TEXT("missing_slot")));
	}

	{
		// Width mismatch: concat declares wrong width.
		FUERLObservationPlan Plan = MakeSelectConcatPlan();
		Plan.Ops[2].Width = 9;
		Plan.GroupWidths[TEXT("policy")] = 9;

		FUERLPlanRuntime Runtime;
		FString Error;
		TestFalse(TEXT("width mismatch fails Compile"), Runtime.Compile(Plan, Available, Error));
		TestTrue(TEXT("width mismatch mentions derived/declared"), Error.Contains(TEXT("width")));
	}

	{
		// Field not in AvailableState.
		FUERLObservationPlan Plan;
		Plan.PlanVersion = 1;
		Plan.StateRequirements = {TEXT("robot.body.base_link.body_pose")};
		Plan.Ops = {MakeSelect(TEXT("robot.body.base_link.body_pose"), TEXT("pose"), 7)};
		Plan.Groups.Add(TEXT("policy"), {TEXT("pose")});
		Plan.GroupWidths.Add(TEXT("policy"), 7);

		const TArray<FUERLFieldDescriptor> EmptyAvailable;
		FUERLPlanRuntime Runtime;
		FString Error;
		TestFalse(TEXT("missing field fails Compile"), Runtime.Compile(Plan, EmptyAvailable, Error));
		TestTrue(
			TEXT("error mentions field or requirement"),
			Error.Contains(TEXT("robot.body.base_link.body_pose")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimeNoPartialCompileTest,
	"UERL.Unit.Policy.PlanRuntime.AC_UE_UNIT_POLICY_004.NoPartialCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimeNoPartialCompileTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();

	FUERLObservationPlan Bad;
	Bad.PlanVersion = 1;
	Bad.StateRequirements = {TEXT("robot.body.base_link.body_pose")};
	FUERLPlanOp Bogus;
	Bogus.Op = TEXT("not_a_real_op");
	Bogus.OutputName = TEXT("out");
	Bogus.Width = 1;
	Bad.Ops = {Bogus};
	Bad.Groups.Add(TEXT("policy"), {TEXT("out")});
	Bad.GroupWidths.Add(TEXT("policy"), 1);

	const TArray<FUERLFieldDescriptor> Available = {
		MakeStateField(TEXT("robot.body.base_link.body_pose"), 7),
	};

	FUERLPlanRuntime Runtime;
	FString Error;
	TestFalse(TEXT("bad Compile fails"), Runtime.Compile(Bad, Available, Error));
	TestFalse(TEXT("not compiled after failure"), Runtime.IsCompiled());

	FUERLPlanInputs Inputs;
	TArray<float> Raw = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f};
	Inputs.RawState = Raw;
	TArray<float> Out;
	FString ExecError;
	TestFalse(TEXT("Execute after failed Compile still errors"), Runtime.Execute(Inputs, Out, ExecError));
	TestTrue(TEXT("Execute error mentions not compiled"), ExecError.Contains(TEXT("not compiled")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimePreviousActionFirstFrameTest,
	"UERL.Unit.Policy.PlanRuntime.PreviousAction.FirstFrameZeros",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimePreviousActionFirstFrameTest::RunTest(const FString& Parameters)
{
	// Episode reset is the controller's duty: pass zero PreviousAction on the
	// first Execute after Reset. PlanRuntime does not cache previous_action.
	RegisterBuiltinPlanOperators();

	FUERLObservationPlan Plan;
	Plan.PlanVersion = 1;
	FUERLPlanOp Prev;
	Prev.Op = TEXT("previous_action");
	Prev.OutputName = TEXT("prev");
	Prev.Width = 3;
	Prev.Params.Scalars.Add(TEXT("width"), 3.0);
	Plan.Ops = {Prev};
	Plan.Groups.Add(TEXT("policy"), {TEXT("prev")});
	Plan.GroupWidths.Add(TEXT("policy"), 3);

	FUERLPlanRuntime Runtime;
	FString Error;
	TestTrue(TEXT("Compile succeeds"), Runtime.Compile(Plan, {}, Error));

	TArray<float> Zeros = {0.f, 0.f, 0.f};
	FUERLPlanInputs Inputs;
	Inputs.PreviousAction = Zeros;
	TArray<float> Out;
	TestTrue(TEXT("first Execute after reset succeeds"), Runtime.Execute(Inputs, Out, Error));
	if (!TestEqual(TEXT("width"), Out.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("zero[0]"), Out[0], 0.f);
	TestEqual(TEXT("zero[1]"), Out[1], 0.f);
	TestEqual(TEXT("zero[2]"), Out[2], 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimePreviousActionResetClearsResidueTest,
	"UERL.Unit.Policy.PlanRuntime.PreviousAction.ResetClearsResidue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimePreviousActionResetClearsResidueTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();

	FUERLObservationPlan Plan;
	Plan.PlanVersion = 1;
	FUERLPlanOp Prev;
	Prev.Op = TEXT("previous_action");
	Prev.OutputName = TEXT("prev");
	Prev.Width = 3;
	Prev.Params.Scalars.Add(TEXT("width"), 3.0);
	Plan.Ops = {Prev};
	Plan.Groups.Add(TEXT("policy"), {TEXT("prev")});
	Plan.GroupWidths.Add(TEXT("policy"), 3);

	FUERLPlanRuntime Runtime;
	FString Error;
	TestTrue(TEXT("Compile succeeds"), Runtime.Compile(Plan, {}, Error));

	TArray<float> Nonzero = {0.5f, -0.25f, 1.0f};
	FUERLPlanInputs NonzeroInputs;
	NonzeroInputs.PreviousAction = Nonzero;
	TArray<float> Mid;
	TestTrue(TEXT("nonzero Execute"), Runtime.Execute(NonzeroInputs, Mid, Error));
	if (!TestEqual(TEXT("nonzero Execute output width"), Mid.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("mid[0]"), Mid[0], 0.5f);

	// Controller Reset(): re-supply zeros — operator must not retain prior values.
	TArray<float> Zeros = {0.f, 0.f, 0.f};
	FUERLPlanInputs ResetInputs;
	ResetInputs.PreviousAction = Zeros;
	TArray<float> AfterReset;
	TestTrue(TEXT("Execute after reset"), Runtime.Execute(ResetInputs, AfterReset, Error));
	if (!TestEqual(TEXT("Execute after reset output width"), AfterReset.Num(), 3))
	{
		return false;
	}
	TestEqual(TEXT("cleared[0]"), AfterReset[0], 0.f);
	TestEqual(TEXT("cleared[1]"), AfterReset[1], 0.f);
	TestEqual(TEXT("cleared[2]"), AfterReset[2], 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimeCommandWidthMismatchCompileTest,
	"UERL.Unit.Policy.PlanRuntime.Command.WidthMismatchAtCompile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimeCommandWidthMismatchCompileTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();

	FUERLObservationPlan Plan;
	Plan.PlanVersion = 1;
	FUERLPlanOp Cmd;
	Cmd.Op = TEXT("command");
	Cmd.OutputName = TEXT("vel");
	Cmd.Width = 3;
	Cmd.Params.Strings.Add(TEXT("channel"), TEXT("velocity"));
	Cmd.Params.Scalars.Add(TEXT("width"), 3.0);
	Plan.Ops = {Cmd};
	Plan.Groups.Add(TEXT("policy"), {TEXT("vel")});
	Plan.GroupWidths.Add(TEXT("policy"), 3);

	TMap<FName, int32> Channels;
	Channels.Add(TEXT("velocity"), 2);

	FUERLPlanRuntime Runtime;
	FString Error;
	TestFalse(TEXT("width mismatch fails Compile"), Runtime.Compile(Plan, {}, Channels, Error));
	TestTrue(TEXT("error names channel"), Error.Contains(TEXT("velocity")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimeCommandMissingAtExecuteTest,
	"UERL.Unit.Policy.PlanRuntime.Command.MissingChannelAtExecute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimeCommandMissingAtExecuteTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();

	FUERLObservationPlan Plan;
	Plan.PlanVersion = 1;
	FUERLPlanOp Cmd;
	Cmd.Op = TEXT("command");
	Cmd.OutputName = TEXT("vel");
	Cmd.Width = 3;
	Cmd.Params.Strings.Add(TEXT("channel"), TEXT("velocity"));
	Cmd.Params.Scalars.Add(TEXT("width"), 3.0);
	Plan.Ops = {Cmd};
	Plan.Groups.Add(TEXT("policy"), {TEXT("vel")});
	Plan.GroupWidths.Add(TEXT("policy"), 3);

	TMap<FName, int32> Channels;
	Channels.Add(TEXT("velocity"), 3);

	FUERLPlanRuntime Runtime;
	FString Error;
	TestTrue(TEXT("Compile succeeds with declared channel"), Runtime.Compile(Plan, {}, Channels, Error));

	FUERLPlanInputs Inputs;
	TArray<float> Out;
	TestFalse(TEXT("missing channel fails Execute"), Runtime.Execute(Inputs, Out, Error));
	TestTrue(TEXT("error names channel"), Error.Contains(TEXT("velocity")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimeControlFrameDtSourceTest,
	"UERL.Unit.Policy.PlanRuntime.AC_UE_UNIT_POLICY_019.ControlFrameDtSource",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimeControlFrameDtSourceTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();
	const FUERLObservationPlan Plan = MakeControlFrameDtPlan(100.0);
	FUERLPlanRuntime Runtime;
	FString Error;
	TestTrue(TEXT("Compile succeeds"), Runtime.Compile(Plan, {}, Error));

	FUERLPlanInputs Inputs;
	Inputs.ControlFrameDtSeconds = 0.005f;
	TArray<float> Out;
	TestTrue(TEXT("first interval executes"), Runtime.Execute(Inputs, Out, Error));
	if (!TestEqual(TEXT("one output"), Out.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("first scaled value"), FMath::IsNearlyEqual(Out[0], 0.5f));

	Inputs.ControlFrameDtSeconds = 0.035f;
	TestTrue(TEXT("second interval executes"), Runtime.Execute(Inputs, Out, Error));
	if (!TestEqual(TEXT("second interval executes output width"), Out.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("source is stateless"), FMath::IsNearlyEqual(Out[0], 3.5f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyPlanRuntimeControlFrameDtValidationTest,
	"UERL.Unit.Policy.PlanRuntime.AC_UE_UNIT_POLICY_020_021.ControlFrameDtValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyPlanRuntimeControlFrameDtValidationTest::RunTest(const FString& Parameters)
{
	RegisterBuiltinPlanOperators();

	for (const double Scale : {0.0, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
	{
		FUERLPlanRuntime Runtime;
		FString Error;
		TestFalse(TEXT("invalid scale fails Compile"), Runtime.Compile(MakeControlFrameDtPlan(Scale), {}, Error));
		TestTrue(TEXT("Compile error names scale"), Error.Contains(TEXT("scale")));
	}

	FUERLPlanRuntime WidthRuntime;
	FString WidthError;
	TestFalse(TEXT("wrong width fails Compile"), WidthRuntime.Compile(MakeControlFrameDtPlan(100.0, 2), {}, WidthError));
	TestTrue(TEXT("width error names control_frame_dt"), WidthError.Contains(TEXT("control_frame_dt")));

	FUERLPlanRuntime Runtime;
	FString Error;
	TestTrue(TEXT("valid plan compiles"), Runtime.Compile(MakeControlFrameDtPlan(100.0), {}, Error));
	FUERLPlanInputs Inputs;
	TArray<float> Out;
	for (const float Dt : {0.0f, -0.005f, std::numeric_limits<float>::quiet_NaN()})
	{
		Inputs.ControlFrameDtSeconds = Dt;
		TestFalse(TEXT("invalid runtime dt fails Execute"), Runtime.Execute(Inputs, Out, Error));
		TestTrue(TEXT("Execute error names control_frame_dt"), Error.Contains(TEXT("control_frame_dt")));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
