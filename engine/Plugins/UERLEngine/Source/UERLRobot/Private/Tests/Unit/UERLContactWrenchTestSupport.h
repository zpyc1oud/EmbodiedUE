#pragma once

#include "Misc/AutomationTest.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/Collision/ContactPoint.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/PBDRigidsEvolutionGBF.h"
#include "Chaos/Collision/CollisionConstraintAllocator.h"
#include "PBDRigidsSolver.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "HAL/IConsoleManager.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace UERLPhysicsResponseTests
{
    inline Chaos::FPBDCollisionConstraints* CompletedContactContainer(FAutomationTestBase& Test, UWorld& World)
    {
        auto* Scene = World.GetPhysicsScene();
        auto* Solver = Scene ? Scene->GetSolver() : nullptr;
        if (!Solver || !Solver->GetEvolution()) { Test.AddError(TEXT("missing completed collision solver")); return nullptr; }
        auto& Collisions = Solver->GetEvolution()->GetCollisionConstraints();
        const auto* Capacity = IConsoleManager::Get().FindConsoleVariable(TEXT("p.Chaos.PBDCollisionSolver.MaxManifoldPoints"));
        if (!Capacity || Capacity->GetInt() >= 0 || Collisions.GetSolverSettings().bUseSplitImpulse)
        {
            Test.AddError(TEXT("momentum oracle requires unlimited manifold capacity and split impulse disabled"));
            return nullptr;
        }
        return &Collisions;
    }

    // Independent completed-step contact oracle. Call at the synchronous
    // post-scatter boundary with split impulse disabled and no solver overflow.
    // The fixture rejects CCD contributions that have no manifold-point result.
    inline bool AccumulateContactWrench(FAutomationTestBase& Test,
        const Chaos::FPBDCollisionConstraint& Constraint, int32 CurrentEpoch,
        const Chaos::FGeometryParticleHandle* Body, double Dt, const FVector& OriginMeters,
        FVector& OutImpulseNs, FVector& OutAngularImpulseNmS)
    {
        if (!Constraint.IsCurrent() || Constraint.GetContainerCookie().LastUsedEpoch != CurrentEpoch) { return true; }
        if (Dt <= 0.0 || Constraint.GetCCDEnabled())
        {
            Test.AddError(TEXT("per-point momentum oracle requires positive dt and CCD disabled")); return false;
        }
        const auto* P0 = Constraint.GetParticle0();
        const auto* P1 = Constraint.GetParticle1();
        if (Body != P0 && Body != P1) { Test.AddError(TEXT("contact does not reference measured body")); return false; }
        const auto* R0 = P0 ? P0->CastToRigidParticle() : nullptr;
        const auto* R1 = P1 ? P1->CastToRigidParticle() : nullptr;
        const double InvM0 = R0 ? double(R0->InvM()) : 0.0;
        const double InvM1 = R1 ? double(R1->InvM()) : 0.0;
        if (InvM0 + InvM1 <= 0.0) { Test.AddError(TEXT("contact oracle needs at least one dynamic body")); return false; }
        const double Sign = Body == P0 ? 1.0 : -1.0;
        FVector UnsignedTotal = FVector::ZeroVector;
        for (int32 Index = 0; Index < Constraint.NumManifoldPoints(); ++Index)
        {
            if (!Constraint.IsManifoldPointActive(Index)) { continue; }
            const auto& Result = Constraint.GetManifoldPointResult(Index);
            if (!Result.bIsValid) { Test.AddError(TEXT("active contact point lacks a completed solver result")); return false; }
            const auto& Point = Constraint.GetManifoldPoint(Index).ContactPoint;
            const auto Position0 = Constraint.GetShapeWorldTransform(0).TransformPositionNoScale(
                Chaos::FVec3(Point.ShapeContactPoints[0]));
            const auto Position1 = Constraint.GetShapeWorldTransform(1).TransformPositionNoScale(
                Chaos::FVec3(Point.ShapeContactPoints[1]));
            // UE 5.8 PBDCollisionContainerSolver:96-102 uses this application
            // point, not the midpoint returned by the closest-point helper.
            const FVector C0(Position0.X, Position0.Y, Position0.Z);
            const FVector C1(Position1.X, Position1.Y, Position1.Z);
            const FVector ApplicationMeters = (InvM0 * C0 + InvM1 * C1) / ((InvM0 + InvM1) * 100.0);
            const FVector VelocityImpulse(Result.NetImpulse.X, Result.NetImpulse.Y, Result.NetImpulse.Z);
            const FVector PositionImpulse(Result.NetPushOut.X, Result.NetPushOut.Y, Result.NetPushOut.Z);
            // NetPushOut is position impulse, not displacement. The sum equals
            // the reported momentum impulse only without split impulse.
            const FVector ImpulseNs = (VelocityImpulse + PositionImpulse / Dt) / 100.0;
            if (ImpulseNs.ContainsNaN() || ApplicationMeters.ContainsNaN())
            {
                Test.AddError(TEXT("nonfinite contact wrench")); return false;
            }
            UnsignedTotal += ImpulseNs;
            OutImpulseNs += Sign * ImpulseNs;
            OutAngularImpulseNmS += FVector::CrossProduct(ApplicationMeters - OriginMeters, Sign * ImpulseNs);
        }
        const auto& Accumulated = Constraint.AccumulatedImpulse;
        const FVector ReportedNs = FVector(Accumulated.X, Accumulated.Y, Accumulated.Z) / 100.0;
        return Test.TestTrue(TEXT("all reported contact impulse is explained by current manifold points; no unaccounted CCD"),
            (UnsignedTotal - ReportedNs).Size() <= 1.0e-5 + 1.0e-4 * ReportedNs.Size());
    }
}
#endif
