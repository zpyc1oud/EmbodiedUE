#include "UERLPhysicsResponseTestSupport.h"

#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "Misc/ScopeExit.h"
#include "UERLBatchBinding.h"
#include "UERLGenericRobotProvider.h"
#include "UERLSlotCollisionPlan.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLPhysicalSparseResetTest,
    "UERL.Integration.PhysicsResponse.ProductEffort.SparseResetClearsForceAndPreservesOtherSlot",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPhysicalSparseResetTest::RunTest(const FString& Parameters)
{
    using namespace UERLPhysicsResponseTests;
    FLockstepSettings Settings;
    const auto Capture = [&](bool ResetSelected, TArray<TArray<float>>& Trace)
    {
        auto Scene = MakeScene();
        UWorld* World = Scene->GetWorld();
        if (!World) { AddError(TEXT("missing two-Slot physical World")); return false; }
        auto Factory = UERLGenericRobot::MakeFactory();
        FUERLProviderConfig Input, Effective;
        Input.AssetPath = TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole");
        Input.Scalars.Add(UERLGenericRobot::SolverStepCommands, 1.0);
        FString Error;
        if (!Factory->ValidateConfig(Input, Effective, Error)) { AddError(Error); return false; }
        const auto& Topology = Factory->Describe().Topology;
        const int32 CartIndex = Topology.Joints.IndexOfByPredicate(
            [](const FUERLJointTopology& J) { return J.Name == FName(TEXT("cart")); });
        if (CartIndex == INDEX_NONE) { AddError(TEXT("missing cart joint")); return false; }
        Input.Actuators.Add(FUERLActuatorConfig{
            0, CartIndex, FName(TEXT("cart")), TEXT("prismatic"), TEXT("linear_x"), TEXT("N"), TEXT("effort"),
            0.0, 0.0, 10.0, 0.0 });
        if (!Factory->ValidateConfig(Input, Effective, Error)) { AddError(Error); return false; }
        const auto& Descriptor = Factory->Describe();
        FUERLBatchSchema Schema;
        Schema.ActionFields.Add({ Descriptor.ActionFields[0], 0 }); Schema.ActionWidth = 1;
        TArray<FUERLFieldDescriptor> Fields;
        for (const TCHAR* Name : { TEXT("cart"), TEXT("pole") })
        for (const TCHAR* Quantity : { TEXT("joint_position"), TEXT("joint_velocity") })
        {
            const FName FieldName(*FString::Printf(TEXT("robot.joint.%s.%s"), Name, Quantity));
            const auto* Field = Descriptor.StateFields.FindByPredicate(
                [FieldName](const FUERLFieldDescriptor& F) { return F.Name == FieldName; });
            if (!Field || Field->Width != 1) { AddError(TEXT("missing scalar physical field")); return false; }
            Fields.Add(*Field); Schema.StateFields.Add({ *Field, Schema.StateWidth++ });
        }
        FUERLBatchBinding Binding;
        if (!Binding.Compile(Schema, Descriptor.ActionFields, Descriptor.StateFields, Error)) { AddError(Error); return false; }
        auto Robot = Factory->Create(Effective);
        ON_SCOPE_EXIT { Robot->DestroySlots(); };
        FUERLSlotCollisionPlan CollisionPlan;
        if (!CollisionPlan.Compile(2, EUERLEnvironmentCollisionScope::SlotIsolated, Error)
            || !Robot->PrepareState(Fields, Error)) { AddError(Error); return false; }
        TArray<FUERLSlotContext> Slots;
        FUERLResetBatch Initial;
        for (int32 Slot = 0; Slot < 2; ++Slot)
        {
            const FVector Origin(Slot * 1000.0, 0.0, 300.0);
            Slots.Add(FUERLSlotContext{ Slot, Origin, 0.0, FVector::UpVector, {} });
            Slots.Last().CollisionProfile = CollisionPlan.Profile(Slot);
            Initial.Rows.Add(FUERLResetRow{ Slot, 0, Origin, 0.0, FVector::UpVector, {} });
        }
        if (!Robot->SpawnIntoSlots(*World, Slots, Error) || !Robot->ResetSlots(Initial, Error)) { AddError(Error); return false; }
        USkeletalMeshComponent* Meshes[2] = { nullptr, nullptr };
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            auto* Mesh = It->FindComponentByClass<USkeletalMeshComponent>();
            auto* Cart = Mesh ? Mesh->GetBodyInstance(FName(TEXT("cart"))) : nullptr;
            if (Cart)
            {
                const int32 Slot = Cart->GetCOMPosition().X < 500.0 ? 0 : 1;
                if (Meshes[Slot]) { AddError(TEXT("ambiguous physical Slot identity")); return false; }
                Meshes[Slot] = Mesh;
            }
        }
        for (auto* Mesh : Meshes)
        {
            if (!Mesh) { AddError(TEXT("missing physical Slot mesh")); return false; }
            Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
            Mesh->SetMassOverrideInKg(FName(TEXT("cart")), 1.0, true);
            Mesh->SetMassOverrideInKg(FName(TEXT("pole")), 0.2, true);
            for (auto* Body : Mesh->Bodies)
            {
                if (!Body) { continue; }
                Body->SetEnableGravity(false); Body->LinearDamping = 0.0f; Body->AngularDamping = 0.0f;
                Body->UpdateDampingProperties(); Body->SetInertiaConditioningEnabled(false);
            }
            for (auto* C : Mesh->Constraints) { if (C) { C->DisableProjection(); C->DisableMassConditioning(); } }
        }
        constexpr double Dt = 0.005;
        if (!Tick(*this, *World, Dt)) { return false; }
        const float Commands[2] = { 0.4f, -0.7f };
        FUERLNamedActionReader Reader(Binding, { Commands, 2, 1 });
        if (!Robot->ApplyCommands(Reader, Error)) { AddError(Error); return false; }
        Meshes[0]->AddImpulse(FVector(2.0, 0.0, 0.0), FName(TEXT("cart")), false); // +0.02 N s
        Meshes[1]->AddImpulse(FVector(-4.0, 0.0, 0.0), FName(TEXT("cart")), false); // -0.04 N s
        for (int32 Step = 0; Step < 200; ++Step)
        {
            if (Step == 100 && ResetSelected)
            {
                // Queue a one-step force immediately before reset. Both this
                // accumulator and the already held command must be cleared.
                Meshes[0]->AddForce(FVector(30.0, 0.0, 0.0), FName(TEXT("cart")), false);
                FUERLResetBatch Reset;
                Reset.Rows.Add(Initial.Rows[0]);
                if (!Robot->ResetSlots(Reset, Error)) { AddError(Error); return false; }
            }
            if (!Tick(*this, *World, Dt)) { return false; }
            TArray<float> State; State.SetNumZeroed(8);
            FUERLNamedStateWriter Writer(Binding, { State.GetData(), 2, 4 });
            if (!Robot->TryCollectState({ 0, 1 }, Writer, Error)) { AddError(Error); return false; }
            for (int32 Slot = 0; Slot < 2; ++Slot)
            {
                const double P = Meshes[Slot]->GetBodyInstance(FName(TEXT("cart")))->GetUnrealWorldVelocity().X / 100.0
                    + 0.2 * Meshes[Slot]->GetBodyInstance(FName(TEXT("pole")))->GetUnrealWorldVelocity().X / 100.0;
                const double Expected = Slot == 0 && ResetSelected && Step >= 100 ? 0.0
                    : (Slot == 0 ? 0.02 : -0.04) + Commands[Slot] * (Step + 1) * Dt;
                TestTrue(TEXT("selected physical force and impulse have the declared whole-system momentum"),
                    FMath::Abs(P - Expected) <= 0.001 + FMath::Abs(Expected) * 0.02);
                if (Slot == 0 && ResetSelected && Step >= 100)
                {
                    TestTrue(TEXT("selected reset leaves zero joint velocities with no stale effort"),
                        FMath::Abs(State[1]) < 0.001 && FMath::Abs(State[3]) < 0.001);
                }
            }
            Trace.Add(MoveTemp(State));
        }
        return true;
    };
    TArray<TArray<float>> Reference, ResetRun;
    if (!Capture(false, Reference) || !Capture(true, ResetRun)) { return false; }
    for (int32 Step = 0; Step < Reference.Num(); ++Step)
    for (int32 Field = 4; Field < 8; ++Field)
    {
        TestTrue(TEXT("resetting Slot zero preserves Slot one's independently repeated trajectory"),
            FMath::Abs(Reference[Step][Field] - ResetRun[Step][Field]) < (Field % 2 == 0 ? 0.0001 : 0.001));
    }
    AddInfo(TEXT("[PHYSICS_ORACLE] two real Slots; distinct forces and impulses; selected reset; 200 completed steps per run"));
    return true;
}

#endif
