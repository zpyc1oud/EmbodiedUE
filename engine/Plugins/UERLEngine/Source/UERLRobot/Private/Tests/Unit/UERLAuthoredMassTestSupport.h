#pragma once

#include "Misc/AutomationTest.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Physics/PhysicsInterfaceTypes.h"
#include "Physics/Experimental/ChaosInterfaceUtils.h"
#include "Chaos/MassProperties.h"
#include "Chaos/Utilities.h"
#include "PhysicsInterfaceTypesCore.h"
#include "HAL/IConsoleManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace UERLPhysicsResponseTests
{
    // Rebuild mass geometry from the authored aggregate, without reading the
    // solver's shapes or using live mass/inertia as the expected result.
    inline bool CheckAuthoredMassProperties(FAutomationTestBase& Test,
        USkeletalBodySetup& Setup, FBodyInstance& Body)
    {
        UPhysicalMaterial* Material = Body.GetSimplePhysicalMaterial();
        if (!Material) { Test.AddError(TEXT("authored mass oracle needs a resolved material")); return false; }
        FGeometryAddParams Params{};
        Params.bDoubleSided = Setup.bDoubleSidedGeometry;
        Params.CollisionTraceType = Setup.GetCollisionTraceFlag();
        Params.Scale = Body.Scale3D;
        Params.SimpleMaterial = Material;
        Params.LocalTransform = FTransform::Identity;
        Params.WorldTransform = Body.GetUnrealWorldTransform();
        Params.Geometry = &Setup.AggGeom;
        Params.TriMeshGeometries = MakeArrayView(Setup.TriMeshGeometries);
        TArray<Chaos::FImplicitObjectPtr> Geometry;
        Chaos::FShapesArray Shapes;
        ChaosInterface::CreateGeometry(Params, Geometry, Shapes);
        TArray<bool> Contributions;
        for (const auto& Shape : Shapes)
        {
            const FKShapeElem* Element = FChaosUserData::Get<FKShapeElem>(Shape->GetUserData());
            Contributions.Add(Element && Element->GetContributeToMass());
        }
        if (Shapes.IsEmpty() || !Contributions.Contains(true))
        { Test.AddError(TEXT("authored mass oracle found no mass-contributing geometry")); return false; }
        const double DensityKgPerCm3 = FMath::Max(0.09 / 1000000.0, double(Material->Density) / 1000.0);
        Chaos::FMassProperties Expected;
        ChaosInterface::CalculateMassPropertiesFromShapeCollection(Expected, Shapes, Contributions, DensityKgPerCm3);
        if (!FMath::IsFinite(double(Expected.Mass)) || Expected.Mass <= 0.0)
        { Test.AddError(TEXT("authored geometry has invalid computed mass")); return false; }
        Chaos::TransformToLocalSpace(Expected);
        const FBodyInstance& Declaration = Setup.DefaultInstance;
        const double Power = FMath::Clamp(double(Material->RaiseMassToPower), double(UE_KINDA_SMALL_NUMBER), 1.0);
        const double ExpectedMass = Declaration.bOverrideMass
            ? FMath::Max(double(Declaration.GetMassOverride()), 0.001)
            : FMath::Max(double(Declaration.MassScale) * FMath::Pow(double(Expected.Mass), Power), 0.001);
        Expected.InertiaTensor *= ExpectedMass / Expected.Mass;
        auto ActorInertia = Chaos::Utilities::ComputeWorldSpaceInertia(Expected.RotationOfMass, Expected.InertiaTensor);
        if (!(Declaration.InertiaTensorScale - FVector::OneVector).IsNearlyZero(1.0e-3f))
        {
            ActorInertia = Chaos::Utilities::ScaleInertia(ActorInertia, Declaration.InertiaTensorScale, false);
        }
        const FVector Nudge = Params.Scale * Declaration.COMNudge;
        const FVector ExpectedCOM = FVector(Expected.CenterOfMass.X, Expected.CenterOfMass.Y, Expected.CenterOfMass.Z) + Nudge;
        const auto* NudgeAffectsInertia = IConsoleManager::Get().FindConsoleVariable(TEXT("p.ComNudgeAffectsInertia"));
        if (!Nudge.IsNearlyZero())
        {
            if (!NudgeAffectsInertia)
            { Test.AddError(TEXT("missing COM nudge inertia setting")); return false; }
            if (NudgeAffectsInertia->GetInt() != 0)
            {
                // The installed engine's authored COM correction is applied
                // in actor coordinates before principal-axis decomposition.
                // This checks that contract, not a hardware inertia model.
                for (int32 Axis = 0; Axis < 3; ++Axis)
                {
                    ActorInertia.SetAt(Axis, Axis, ActorInertia.GetAt(Axis, Axis)
                        + ExpectedMass * Nudge[Axis] * Nudge[Axis]);
                }
            }
        }
        const FTransform MassFrame = Body.GetMassSpaceLocal();
        const FVector LivePrincipal = Body.GetBodyInertiaTensor() / 10000.0;
        const FQuat LiveRotation = MassFrame.GetRotation();
        bool Passed = Test.TestTrue(TEXT("authored geometry and resolved density predict live mass"),
            FMath::Abs(Body.GetBodyMass() - ExpectedMass) <= 1.0e-5 + 0.005 * ExpectedMass);
        Passed &= Test.TestTrue(TEXT("authored geometry predicts local COM including declared nudge"),
            (MassFrame.GetLocation() - ExpectedCOM).Size() / 100.0 <= 1.0e-5);
        double MaximumTensorError = 0.0;
        for (const FVector& Axis : { FVector::XAxisVector, FVector::YAxisVector, FVector::ZAxisVector })
        {
            const auto ExpectedColumn = ActorInertia * Chaos::FVec3(Axis);
            const FVector ExpectedSI = FVector(ExpectedColumn.X, ExpectedColumn.Y, ExpectedColumn.Z) / 10000.0;
            const FVector ActualSI = LiveRotation.RotateVector(LivePrincipal * LiveRotation.UnrotateVector(Axis));
            const double Error = (ActualSI - ExpectedSI).Size();
            MaximumTensorError = FMath::Max(MaximumTensorError, Error);
            Passed &= Test.TestTrue(TEXT("authored full inertia tensor and mass-frame orientation reach the solver"),
                Error <= 1.0e-8 + 0.005 * ExpectedSI.Size());
        }
        Test.AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] authored_mass body=%s material=%s scale=%s expected_kg=%.9f measured_kg=%.9f expected_com_cm=%s measured_com_cm=%s max_tensor_error_kg_m2=%.12f"),
            *Setup.BoneName.ToString(), *Material->GetPathName(), *Params.Scale.ToString(), ExpectedMass,
            Body.GetBodyMass(), *ExpectedCOM.ToString(), *MassFrame.GetLocation().ToString(), MaximumTensorError));
        return Passed;
    }
}
#endif
