#include "Misc/AutomationTest.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UERLPlan.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	const TCHAR* GUERLStructurePlanJson = TEXT(R"JSON({
  "plan_version": 1,
  "state_requirements": [
    "robot.body.base_link.body_pose",
    "robot.joint.left_c1.joint_position"
  ],
  "ops": [
    {
      "op": "select",
      "inputs": [],
      "output": "pose",
      "width": 7,
      "params": {"field": "robot.body.base_link.body_pose"}
    },
    {
      "op": "select",
      "inputs": [],
      "output": "joint",
      "width": 1,
      "params": {"field": "robot.joint.left_c1.joint_position"}
    },
    {
      "op": "concat",
      "inputs": ["pose", "joint"],
      "output": "policy_vec",
      "width": 8,
      "params": {}
    }
  ],
  "groups": {
    "policy": ["pose", "joint"],
    "debug": ["policy_vec"]
  },
  "group_widths": {
    "policy": 8,
    "debug": 8
  }
})JSON");

	bool ParseObject(const FString& Text, TSharedPtr<FJsonObject>& OutObject, FString& OutError)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
		{
			OutError = TEXT("failed to deserialize JSON object");
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPlanParseStructureTest,
	"UERL.Unit.Interface.Plan.AC_UE_UNIT_PLAN_001.ParseJsonStructure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPlanParseStructureTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Json;
	FString Error;
	TestTrue(TEXT("structure corpus JSON deserializes"), ParseObject(GUERLStructurePlanJson, Json, Error));
	if (!Json.IsValid())
	{
		AddError(Error);
		return false;
	}

	FUERLObservationPlan Plan;
	TestTrue(TEXT("ParseJson accepts the shared structure corpus"), Plan.ParseJson(Json.ToSharedRef(), Error));
	if (!Error.IsEmpty() && !TestTrue(TEXT("ParseJson leaves no error"), Error.IsEmpty()))
	{
		AddError(Error);
	}

	TestEqual(TEXT("op count"), Plan.Ops.Num(), 3);
	TestEqual(TEXT("op[0] name"), Plan.Ops[0].Op, FName(TEXT("select")));
	TestEqual(TEXT("op[1] name"), Plan.Ops[1].Op, FName(TEXT("select")));
	TestEqual(TEXT("op[2] name"), Plan.Ops[2].Op, FName(TEXT("concat")));
	TestEqual(TEXT("op[0] width"), Plan.Ops[0].Width, 7);
	TestEqual(TEXT("op[1] width"), Plan.Ops[1].Width, 1);
	TestEqual(TEXT("op[2] width"), Plan.Ops[2].Width, 8);

	const TArray<FName>* PolicyGroup = Plan.Groups.Find(TEXT("policy"));
	TestNotNull(TEXT("policy group exists"), PolicyGroup);
	if (PolicyGroup)
	{
		TestEqual(TEXT("policy group size"), PolicyGroup->Num(), 2);
		if (PolicyGroup->Num() == 2)
		{
			TestEqual(TEXT("policy[0]"), (*PolicyGroup)[0], FName(TEXT("pose")));
			TestEqual(TEXT("policy[1]"), (*PolicyGroup)[1], FName(TEXT("joint")));
		}
	}

	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_PLAN_001: ParseJson reads the shared structure JSON"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPlanValidateStructureErrorsTest,
	"UERL.Unit.Interface.Plan.AC_UE_UNIT_PLAN_002.ValidateStructureErrors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPlanValidateStructureErrorsTest::RunTest(const FString& Parameters)
{
	FString Error;

	{
		FUERLObservationPlan Cycle;
		FUERLPlanOp First;
		First.Op = TEXT("scale");
		First.InputNames = { TEXT("b") };
		First.OutputName = TEXT("a");
		First.Width = 1;
		FUERLPlanOp Second;
		Second.Op = TEXT("scale");
		Second.InputNames = { TEXT("a") };
		Second.OutputName = TEXT("b");
		Second.Width = 1;
		Cycle.Ops = { First, Second };
		TestFalse(TEXT("cycle is rejected"), Cycle.Validate(Error));
		TestTrue(TEXT("cycle error mentions cyclic"), Error.Contains(TEXT("cyclic")));
	}

	{
		Error.Reset();
		FUERLObservationPlan NonTopo;
		FUERLPlanOp First;
		First.Op = TEXT("scale");
		First.InputNames = { TEXT("later") };
		First.OutputName = TEXT("early");
		First.Width = 1;
		FUERLPlanOp Second;
		Second.Op = TEXT("select");
		Second.OutputName = TEXT("later");
		Second.Width = 1;
		NonTopo.Ops = { First, Second };
		TestFalse(TEXT("non-topological order is rejected"), NonTopo.Validate(Error));
		TestTrue(TEXT("non-topo error mentions topological"), Error.Contains(TEXT("topological")));
	}

	{
		Error.Reset();
		FUERLObservationPlan Duplicate;
		FUERLPlanOp First;
		First.Op = TEXT("select");
		First.OutputName = TEXT("pose");
		First.Width = 7;
		FUERLPlanOp Second = First;
		Duplicate.Ops = { First, Second };
		TestFalse(TEXT("duplicate slot is rejected"), Duplicate.Validate(Error));
		TestTrue(TEXT("duplicate error mentions duplicate"), Error.Contains(TEXT("duplicate")));
	}

	{
		Error.Reset();
		FUERLObservationPlan Unknown;
		FUERLPlanOp Op;
		Op.Op = TEXT("scale");
		Op.InputNames = { TEXT("missing") };
		Op.OutputName = TEXT("out");
		Op.Width = 1;
		Unknown.Ops = { Op };
		TestFalse(TEXT("unknown slot is rejected"), Unknown.Validate(Error));
		TestTrue(TEXT("unknown error mentions unknown"), Error.Contains(TEXT("unknown")));
	}

	{
		Error.Reset();
		FUERLObservationPlan WidthMismatch;
		FUERLPlanOp Op;
		Op.Op = TEXT("select");
		Op.OutputName = TEXT("pose");
		Op.Width = 7;
		WidthMismatch.Ops = { Op };
		WidthMismatch.Groups.Add(TEXT("policy"), { TEXT("pose") });
		WidthMismatch.GroupWidths.Add(TEXT("policy"), 3);
		TestFalse(TEXT("group width mismatch is rejected"), WidthMismatch.Validate(Error));
		TestTrue(TEXT("group width error mentions width"), Error.Contains(TEXT("width")));
	}

	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_PLAN_002: Validate catches cycle/non-topo/duplicate/unknown/group-width"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPlanVersionMismatchTest,
	"UERL.Unit.Interface.Plan.AC_UE_UNIT_PLAN_003.PlanVersionMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPlanVersionMismatchTest::RunTest(const FString& Parameters)
{
	const FString Text = TEXT(R"JSON({
  "plan_version": 99,
  "state_requirements": ["robot.body.base_link.body_pose"],
  "ops": [{"op": "select", "inputs": [], "output": "pose", "width": 7, "params": {}}],
  "groups": {"policy": ["pose"]},
  "group_widths": {"policy": 7}
})JSON");
	TSharedPtr<FJsonObject> Json;
	FString Error;
	TestTrue(TEXT("version-mismatch JSON deserializes"), ParseObject(Text, Json, Error));
	FUERLObservationPlan Plan;
	TestFalse(TEXT("unsupported plan_version is rejected"), Plan.ParseJson(Json.ToSharedRef(), Error));
	TestTrue(TEXT("version error mentions plan_version"), Error.Contains(TEXT("plan_version")));
	TestEqual(TEXT("reject leaves ops empty"), Plan.Ops.Num(), 0);
	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_PLAN_003: plan_version mismatch fails without accepting ops"));
	return true;
}

#endif
