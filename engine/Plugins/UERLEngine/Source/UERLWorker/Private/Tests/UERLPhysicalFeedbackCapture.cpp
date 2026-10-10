#include "UERLPhysicalFeedbackCapture.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "UERLBatchBinding.h"
#include "UERLBridgeTypes.h"
#include "UERLPhysicsSnapshot.h"
#include "UERLProvider.h"

bool CaptureUERLPhysicalFeedback(UWorld* World, const FUERLBridgeRequest& Request,
    const FUERLBatchBinding& Binding, const FUERLSlotContext* Slot, FString& OutError)
{
    FString Path;
    if (!FParse::Value(FCommandLine::Get(), TEXT("uerltestphysicalfeedback="), Path)) { return true; }
    if (!World || !Slot || Request.States.NumRows != 1 || Request.Sequence > 10000)
    {
        OutError = TEXT("physical feedback capture requires one Slot and a bounded session");
        return false;
    }
    USkeletalMeshComponent* Mesh = nullptr;
    for (TActorIterator<AActor> It(World); It; ++It)
    {
        auto* Candidate = It->FindComponentByClass<USkeletalMeshComponent>();
        if (Candidate && Candidate->GetFName() == FName(TEXT("RobotSkeletalMesh")))
        {
            if (Mesh) { OutError = TEXT("ambiguous physical feedback robot"); return false; }
            Mesh = Candidate;
        }
    }
    FUERLSolverClockSnapshot Clock;
    if (!Mesh || !ReadUERLSolverClock(*World, Clock, OutError))
    {
        if (OutError.IsEmpty()) { OutError = TEXT("missing physical feedback robot"); }
        return false;
    }
    FString Rows;
    int32 Captured = 0;
    for (FConstraintInstance* Joint : Mesh->Constraints)
    {
        if (!Joint) { continue; }
        FBodyInstance* Child = Mesh->GetBodyInstance(Joint->ConstraintBone1);
        FBodyInstance* Parent = Mesh->GetBodyInstance(Joint->ConstraintBone2);
        if (!Child || !Parent) { OutError = TEXT("missing physical feedback joint body"); return false; }
        const bool Twist = Joint->GetAngularTwistMotion() != EAngularConstraintMotion::ACM_Locked;
        const bool Swing1 = Joint->GetAngularSwing1Motion() != EAngularConstraintMotion::ACM_Locked;
        const bool Swing2 = Joint->GetAngularSwing2Motion() != EAngularConstraintMotion::ACM_Locked;
        if (int32(Twist) + int32(Swing1) + int32(Swing2) != 1)
        {
            OutError = TEXT("physical feedback oracle requires one free angular coordinate per joint");
            return false;
        }
        // Literal UE constraint axes. Do not call the product observation reader.
        const FVector Axis = Twist ? FVector::XAxisVector : Swing1 ? FVector::ZAxisVector : FVector::YAxisVector;
        const FTransform C = Joint->GetRefFrame(EConstraintFrame::Frame1) * Child->GetUnrealWorldTransform();
        const FTransform P = Joint->GetRefFrame(EConstraintFrame::Frame2) * Parent->GetUnrealWorldTransform();
        const FQuat Q = (P.GetRotation().Inverse() * C.GetRotation()).GetNormalized();
        const double Angle = FMath::UnwindRadians(2.0 * FMath::Atan2(
            FVector::DotProduct(FVector(Q.X, Q.Y, Q.Z), Axis), Q.W));
        const double Speed = FVector::DotProduct(C.GetRotation().RotateVector(Axis),
            Child->GetUnrealWorldAngularVelocityInRadians() - Parent->GetUnrealWorldAngularVelocityInRadians());
        for (int32 Quantity = 0; Quantity < 2; ++Quantity)
        {
            const FString Name = FString::Printf(TEXT("robot.joint.%s.%s"), *Joint->JointName.ToString(),
                Quantity == 0 ? TEXT("joint_position") : TEXT("joint_velocity"));
            const FUERLBatchFieldBinding* Field = Binding.FindState(FName(*Name));
            if (!Field || Field->Field.Width != 1)
            {
                OutError = TEXT("physical feedback capture requires every named joint position and velocity");
                return false;
            }
            const double Native = Quantity == 0 ? Angle : Speed;
            const float Staged = Request.States.At(0, Field->Column);
            if (!FMath::IsFinite(Native) || !FMath::IsFinite(Staged))
            {
                OutError = TEXT("nonfinite physical feedback capture"); return false;
            }
            Rows += FString::Printf(TEXT("%llu,%d,%.17g,%.17g,%s,%.17g,%.17g\n"),
                static_cast<unsigned long long>(Request.Sequence), Clock.Frame, Clock.SolverTime,
                Clock.LastDt, *Name, Native, double(Staged));
            ++Captured;
        }
    }
    if (Captured != 36) { OutError = TEXT("incomplete native joint capture"); return false; }
    // This fixture uses a horizontal Slot frame. A rotated robot reset is
    // separate from the Slot frame. Reject a different scene precondition.
    const FTransform SlotFrame(FQuat::Identity, Slot->Origin);
    if (!Slot->GroundNormal.Equals(FVector::UpVector, 1.0e-6))
    {
        OutError = TEXT("physical feedback fixture requires a world-aligned Slot frame"); return false;
    }
    for (FConstraintInstance* Joint : Mesh->Constraints)
    {
        FBodyInstance* Body = Mesh->GetBodyInstance(Joint->ConstraintBone1);
        const FTransform Pose = Body->GetUnrealWorldTransform().GetRelativeTransform(SlotFrame);
        const FVector Position = Pose.GetLocation() / 100.0;
        const FQuat Rotation = Pose.GetRotation().GetNormalized();
        const FVector V = Body->GetUnrealWorldVelocity() / 100.0;
        const FVector W = Body->GetUnrealWorldAngularVelocityInRadians();
        const double Values[] = { Position.X, Position.Y, Position.Z, Rotation.X, Rotation.Y,
            Rotation.Z, Rotation.W, V.X, V.Y, V.Z, W.X, W.Y, W.Z };
        int32 Offset = 0;
        for (const TCHAR* Quantity : { TEXT("body_pose"), TEXT("body_linear_velocity"), TEXT("body_angular_velocity") })
        {
            const FString Name = FString::Printf(TEXT("robot.body.%s.%s"), *Joint->ConstraintBone1.ToString(), Quantity);
            const FUERLBatchFieldBinding* Field = Binding.FindState(FName(*Name));
            const int32 Width = Offset == 0 ? 7 : 3;
            if (!Field || Field->Field.Width != Width)
            {
                OutError = TEXT("physical feedback capture requires articulated pose and velocity fields"); return false;
            }
            for (int32 Component = 0; Component < Width; ++Component)
            {
                Rows += FString::Printf(TEXT("%llu,%d,%.17g,%.17g,%s[%d],%.17g,%.17g\n"),
                    static_cast<unsigned long long>(Request.Sequence), Clock.Frame, Clock.SolverTime,
                    Clock.LastDt, *Name, Component, Values[Offset + Component],
                    double(Request.States.At(0, Field->Column + Component)));
                ++Captured;
            }
            Offset += Width;
        }
    }
    if (Captured != 270 || !FFileHelper::SaveStringToFile(Rows, *Path,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append))
    {
        OutError = TEXT("incomplete or unwritable physical feedback capture"); return false;
    }
    return true;
}
#endif
