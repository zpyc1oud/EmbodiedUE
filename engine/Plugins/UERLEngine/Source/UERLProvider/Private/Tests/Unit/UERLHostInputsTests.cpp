#include "UERLHostInputs.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLHostInputsTest, "UERL.Unit.InputProvider.Host.RoutingResetFault",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FUERLHostInputsTest::RunTest(const FString&)
{
	FString Text, Error;
	if (!TestTrue(TEXT("read paired contract"), FFileHelper::LoadFileToString(Text,
		*FPaths::Combine(FPaths::ProjectDir(), TEXT("../tests/parity/cases/inputs/contract.json"))))) { return false; }
	TSharedPtr<FJsonObject> Json;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json);
	TArray<FUERLResolvedInput> Inputs;
	if (!TestTrue(TEXT("parse paired contract"), ParseUERLInputContract(Json, Inputs, true, Error))) { return false; }
	TestEqual(TEXT("scalar contract"), Inputs[0].Fields[0].Shape.Num(), 0);
	TestEqual(TEXT("scalar width"), Inputs[0].Fields[0].Width, 1);
	TArray<FUERLResolvedInput> RoundTrip;
	TestTrue(TEXT("serialize paired contract"), ParseUERLInputContract(WriteUERLInputContract(Inputs), RoundTrip, true, Error));
	TestEqual(TEXT("ray output frame"), RoundTrip[1].Fields[0].CoordinateFrame, FString(TEXT("world/z-relative")));
	Inputs.SetNum(1);
	FUERLFieldDescriptor Joint = Inputs[0].Fields[0];
	FUERLFieldDescriptor Velocity = Joint; Velocity.Name = TEXT("legacy.velocity"); Velocity.Unit = TEXT("rad/s");
	Velocity.Semantic = TEXT("joint_velocity");
	FUERLHostInputs Host;
	if (!TestTrue(TEXT("configure mixed routing"), Host.Configure(Inputs, {Joint, Velocity}, {Velocity, Joint}, Error))) { return false; }
	FUERLInputSlotBinding A, B; A.Slot.SlotId = 2; B.Slot.SlotId = 7;
	if (!TestTrue(TEXT("bind sparse slots"), Host.Bind({A, B}, Error))) { return false; }
	TArray<float> Values;
	TestTrue(TEXT("publish"), Host.Publish(2, 10, 0.2, TArray<float>{0.5f, 3.0f}, Values, Error));
	TestTrue(TEXT("selected order, not source order"), Values == TArray<float>({3.0f, 0.5f}));
	TestTrue(TEXT("duplicate publication"), Host.Publish(2, 10, 0.2, TArray<float>{9, 9}, Values, Error));
	TestTrue(TEXT("entire publication immutable"), Values == TArray<float>({3.0f, 0.5f}));
	TestTrue(TEXT("other slot"), Host.Publish(7, 10, 0.2, TArray<float>{-0.5f, -3.0f}, Values, Error));
	TestTrue(TEXT("sparse reset"), Host.Reset(TArray<int32>{2}, Error));
	TestTrue(TEXT("same clock after physical reset"), Host.Publish(2, 10, 0.2, TArray<float>{0, 0}, Values, Error));
	TestTrue(TEXT("reset publishes zero"), Values == TArray<float>({0, 0}));
	TestTrue(TEXT("other slot publication retained"), Host.Publish(7, 10, 0.2, TArray<float>{9, 9}, Values, Error));
	TestTrue(TEXT("unaffected values"), Values == TArray<float>({-3.0f, -0.5f}));
	TestFalse(TEXT("required nonfinite input faults"), Host.Publish(2, 11, 0.22,
		TArray<float>{std::numeric_limits<float>::quiet_NaN(), 0}, Values, Error));
	TestEqual(TEXT("no partial publication"), Values.Num(), 0);
	TestTrue(TEXT("other slot continues"), Host.Publish(7, 11, 0.22, TArray<float>{1, 2}, Values, Error));
	TestFalse(TEXT("fault remains until reset"), Host.Publish(2, 12, 0.24, TArray<float>{0, 0}, Values, Error));
	TestTrue(TEXT("recover"), Host.Reset(TArray<int32>{2}, Error));
	TestTrue(TEXT("recovered publication"), Host.Publish(2, 12, 0.24, TArray<float>{0, 0}, Values, Error));
	Host.Release();
	Inputs[0].Fields[0].Unit = TEXT("degree");
	TestFalse(TEXT("persisted unit mismatch"), Host.Configure(Inputs, {Joint}, {Joint}, Error));
	Inputs[0].Fields[0] = Joint;
	FUERLFieldDescriptor Wrong = Joint; Wrong.Shape = {1};
	TestFalse(TEXT("scalar and vector are distinct"), Host.Configure(Inputs, {Joint}, {Wrong}, Error));
	return true;
}
#endif
