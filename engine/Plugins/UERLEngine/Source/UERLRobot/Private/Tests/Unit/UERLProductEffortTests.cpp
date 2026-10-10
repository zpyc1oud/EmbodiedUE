#include "UERLPhysicsResponseTestSupport.h"

#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "UERLSkeletalMeshRobotRuntime.h"
#include "UERLGenericRobotProvider.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLProductForceMomentumTest,
	"UERL.Integration.PhysicsResponse.ProductEffort.CartPoleForceMomentumLifetimeAndReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLProductForceMomentumTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	FLockstepSettings Settings;
	for (double CartMass : { 1.0, 2.0 })
	{
		for (double Yaw : { 0.0, 35.0 })
		{
			auto Scene = MakeScene();
			UWorld* World = Scene->GetWorld();
			if (!World) { AddError(TEXT("missing product effort World")); return false; }
			FUERLSkeletalMeshRobotRuntimeConfig Config;
			Config.AssetPath = TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole");
			Config.bClaimAuthoredActor = false;
			Config.bHasPlacementTransform = true;
			Config.PlacementTransform = FTransform(FRotator(0.0, Yaw, 0.0), FVector(0.0, 0.0, 300.0));
			FUERLSkeletalMeshRuntimeActuator& Actuator = Config.Actuators.AddDefaulted_GetRef();
			Actuator.JointName = TEXT("cart");
			Actuator.Stiffness = 0.0; // Select the actual generic effort path.
			Actuator.Damping = 0.1; // The supported effort law includes viscous damping.
			Actuator.EffortLimit = 10.0;
			Config.Observations.Add({ EUERLObservationType::JointPosition, FName(TEXT("cart")) });
			Config.Observations.Add({ EUERLObservationType::JointVelocity, FName(TEXT("cart")) });
			FUERLSkeletalMeshRobotRuntime Runtime;
			FString Error;
			if (!Runtime.Initialize(*World, Config, Error)) { AddError(Error); return false; }
			USkeletalMeshComponent* Mesh = nullptr;
			for (TActorIterator<AActor> It(World); It; ++It)
			{
				auto* Candidate = It->FindComponentByClass<USkeletalMeshComponent>();
				if (Candidate && Candidate->GetFName() == FName(TEXT("RobotSkeletalMesh"))) { Mesh = Candidate; break; }
			}
			FBodyInstance* Cart = Mesh ? Mesh->GetBodyInstance(FName(TEXT("cart"))) : nullptr;
			FBodyInstance* Pole = Mesh ? Mesh->GetBodyInstance(FName(TEXT("pole"))) : nullptr;
			FConstraintInstance* Rail = Mesh ? Mesh->FindConstraintInstance(FName(TEXT("cart"))) : nullptr;
			if (!Cart || !Pole || !Rail) { AddError(TEXT("actual CartPole bodies and rail are required")); return false; }
			Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
			Mesh->SetMassOverrideInKg(FName(TEXT("cart")), CartMass, true);
			Mesh->SetMassOverrideInKg(FName(TEXT("pole")), 0.2, true);
			for (FBodyInstance* Body : { Cart, Pole })
			{
				Body->SetEnableGravity(false);
				Body->LinearDamping = 0.0f;
				Body->AngularDamping = 0.0f;
				Body->UpdateDampingProperties();
				Body->SetInertiaConditioningEnabled(false);
			}
			for (FConstraintInstance* C : Mesh->Constraints)
			{
				if (C) { C->DisableProjection(); C->DisableMassConditioning(); }
			}
			// A fixed pole gives one independent translational coordinate. This
			// lets the supported target-minus-damping law have a closed solution.
			FConstraintInstance* PoleJoint = Mesh->FindConstraintInstance(FName(TEXT("pole")));
			if (!PoleJoint) { AddError(TEXT("missing pole fixture constraint")); return false; }
			PoleJoint->SetAngularTwistMotion(EAngularConstraintMotion::ACM_Locked);
			PoleJoint->SetAngularSwing1Motion(EAngularConstraintMotion::ACM_Locked);
			PoleJoint->SetAngularSwing2Motion(EAngularConstraintMotion::ACM_Locked);
			constexpr double Dt = 0.005;
			if (!Tick(*this, *World, Dt)) { return false; }
			TestTrue(TEXT("cart mass matches its declared physical fixture"), FMath::Abs(Cart->GetBodyMass() - CartMass) < 1.0e-5);
			TestTrue(TEXT("pole mass matches its declared physical fixture"), FMath::Abs(Pole->GetBodyMass() - 0.2) < 1.0e-5);
			FBodyInstance* Base = Mesh->GetBodyInstance(FName(TEXT("base")));
			if (!Base) { AddError(TEXT("missing fixed rail anchor")); return false; }
			const FTransform ParentFrame = Rail->GetRefFrame(EConstraintFrame::Frame2) * Base->GetUnrealWorldTransform();
			const FVector Axis = FRotator(0.0, Yaw, 0.0).Quaternion().RotateVector(FVector::XAxisVector);
			TestTrue(TEXT("declared root rotation reaches the physical prismatic rail"),
				Matches(ParentFrame.GetRotation().RotateVector(FVector::XAxisVector), Axis, 1.0e-5));
			const auto MomentumAlongRail = [&]()
			{
				return FVector::DotProduct(Axis,
					Cart->GetUnrealWorldVelocity() * (CartMass / 100.0)
					+ Pole->GetUnrealWorldVelocity() * (0.2 / 100.0));
			};
			const double InitialMomentum = MomentumAlongRail();
			double ExpectedMomentum = InitialMomentum;
			const double TotalMass = CartMass + 0.2;
			constexpr double Damping = 0.1;
			for (float Force : { 1.0f, 0.0f, -1.0f })
			{
				const float Targets[] = { Force };
				// Publish once. The production solver-step command owner must
				// retain this force and replace it with zero on the next phase.
				if (!Runtime.ApplyActuatorTargets(Targets, Error)) { AddError(Error); return false; }
				for (int32 Step = 0; Step < 40; ++Step)
				{
					if (!Tick(*this, *World, Dt)) { return false; }
					const double EquilibriumMomentum = Force * TotalMass / Damping;
					ExpectedMomentum = EquilibriumMomentum + (ExpectedMomentum - EquilibriumMomentum)
						* FMath::Exp(-Damping * Dt / TotalMass);
					const double Measured = MomentumAlongRail();
					TestTrue(TEXT("generic held effort follows the independent damped whole-system response"),
						FMath::Abs(Measured - ExpectedMomentum) <= 0.001 + FMath::Abs(ExpectedMomentum) * 0.02);
				}
			}
			const float ForceBeforeReset[] = { 1.0f };
			if (!Runtime.ApplyActuatorTargets(ForceBeforeReset, Error)) { AddError(Error); return false; }
			for (int32 Step = 0; Step < 20; ++Step) { if (!Tick(*this, *World, Dt)) { return false; } }
			if (!Runtime.ResetToReferencePose(Error)) { AddError(Error); return false; }
			for (int32 Step = 0; Step < 40; ++Step)
			{
				if (!Tick(*this, *World, Dt)) { return false; }
				TestTrue(TEXT("reset clears the retained product effort without another action publication"),
					FMath::Abs(MomentumAlongRail()) < 0.001);
			}
			AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] generic_effort cart_mass=%.3f pole_mass=0.2 yaw_deg=%.1f final_p=%.9f"),
				CartMass, Yaw, MomentumAlongRail()));
		}
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLProductTorqueResponseTest,
    "UERL.Integration.PhysicsResponse.ProductEffort.CartPoleRevoluteTorqueResponse",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLProductTorqueResponseTest::RunTest(const FString& Parameters)
{
    using namespace UERLPhysicsResponseTests;
    FLockstepSettings Settings;
    for (double Mass : { 0.2, 0.4 })
    for (double Yaw : { 0.0, 35.0 })
    for (double Dt : { 0.005, 0.0025 })
    for (double Sign : { -1.0, 1.0 })
    for (bool SolverCallback : { false, true })
    {
        auto Scene = MakeScene();
        UWorld* World = Scene->GetWorld();
        if (!World) { AddError(TEXT("missing torque fixture World")); return false; }
        FUERLSkeletalMeshRobotRuntimeConfig Config;
        Config.AssetPath = TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole");
        Config.bClaimAuthoredActor = false;
        Config.bHasPlacementTransform = true;
        Config.PlacementTransform = FTransform(FRotator(0.0, Yaw, 0.0), FVector(0.0, 0.0, 300.0));
        auto& A = Config.Actuators.AddDefaulted_GetRef();
        A.JointName = TEXT("pole"); A.Stiffness = SolverCallback ? 0.0 : 1.0;
        A.Damping = 0.01; A.EffortLimit = 10.0;
        Config.Observations.Add({ EUERLObservationType::JointPosition, FName(TEXT("pole")) });
        Config.Observations.Add({ EUERLObservationType::JointVelocity, FName(TEXT("pole")) });
        FUERLSkeletalMeshRobotRuntime Runtime;
        FString Error;
        if (!Runtime.Initialize(*World, Config, Error)) { AddError(Error); return false; }
        USkeletalMeshComponent* Mesh = nullptr;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            auto* Candidate = It->FindComponentByClass<USkeletalMeshComponent>();
            if (Candidate && Candidate->GetFName() == FName(TEXT("RobotSkeletalMesh"))) { Mesh = Candidate; break; }
        }
        FBodyInstance* Pole = Mesh ? Mesh->GetBodyInstance(FName(TEXT("pole"))) : nullptr;
        FBodyInstance* Cart = Mesh ? Mesh->GetBodyInstance(FName(TEXT("cart"))) : nullptr;
        FConstraintInstance* Joint = Mesh ? Mesh->FindConstraintInstance(FName(TEXT("pole"))) : nullptr;
        FConstraintInstance* Rail = Mesh ? Mesh->FindConstraintInstance(FName(TEXT("cart"))) : nullptr;
        if (!Pole || !Cart || !Joint || !Rail) { AddError(TEXT("missing actual pole/rail fixture")); return false; }
        Joint->SetOrientationDriveTwistAndSwing(false, false);
        Joint->SetAngularVelocityDriveTwistAndSwing(false, false);
        // Turn the actual rail into a fixed anchor. Keep the production effort
        // publication, solver callback, joint mapping and unit conversion intact.
        Rail->SetLinearXMotion(ELinearConstraintMotion::LCM_Locked);
        Rail->SetLinearYMotion(ELinearConstraintMotion::LCM_Locked);
        Rail->SetLinearZMotion(ELinearConstraintMotion::LCM_Locked);
        Mesh->SetCollisionResponseToAllChannels(ECR_Ignore);
        Mesh->SetMassOverrideInKg(FName(TEXT("pole")), Mass, true);
        for (FBodyInstance* Body : { Cart, Pole })
        {
            Body->SetEnableGravity(false);
            Body->LinearDamping = 0.0f; Body->AngularDamping = 0.0f;
            Body->UpdateDampingProperties(); Body->SetInertiaConditioningEnabled(false);
        }
        for (FConstraintInstance* C : Mesh->Constraints)
        {
            if (C) { C->DisableProjection(); C->DisableMassConditioning(); }
        }
        if (!Tick(*this, *World, Dt)) { return false; }
        const bool Twist = Joint->GetAngularTwistMotion() != EAngularConstraintMotion::ACM_Locked;
        const bool Swing1 = Joint->GetAngularSwing1Motion() != EAngularConstraintMotion::ACM_Locked;
        const bool Swing2 = Joint->GetAngularSwing2Motion() != EAngularConstraintMotion::ACM_Locked;
        if (!TestTrue(TEXT("pole has exactly one angular coordinate"),
            int32(Twist) + int32(Swing1) + int32(Swing2) == 1)) { return false; }
        const FVector LocalAxis = Twist ? FVector::XAxisVector : Swing1 ? FVector::ZAxisVector : FVector::YAxisVector;
        // Independently exercise the per-frame training applier as well as
        // the held solver callback used by direct runtime control.
        auto Factory = UERLGenericRobot::MakeFactory();
        FUERLProviderConfig ReflectionInput, Reflection;
        ReflectionInput.AssetPath = Config.AssetPath;
        if (!Factory->ValidateConfig(ReflectionInput, Reflection, Error)) { AddError(Error); return false; }
        const FUERLRobotTopology Topology = Factory->Describe().Topology;
        const int32 PoleIndex = Topology.Joints.IndexOfByPredicate(
            [](const FUERLJointTopology& J) { return J.Name == FName(TEXT("pole")); });
        if (PoleIndex == INDEX_NONE) { AddError(TEXT("missing reflected torque joint")); return false; }
        TArray<int32> Indices;
        for (const auto& J : Topology.Joints)
        {
            Indices.Add(Mesh->Constraints.IndexOfByPredicate(
                [&J](const FConstraintInstance* C) { return C && C->JointName == J.Name; }));
        }
        TArray<FUERLActuatorConfig> EffortActuators;
        EffortActuators.Add(FUERLActuatorConfig{
            0, PoleIndex, FName(TEXT("pole")), TEXT("revolute"),
            Twist ? TEXT("twist") : Swing1 ? TEXT("swing1") : TEXT("swing2"), TEXT("N*m"), TEXT("effort"),
            0.0, 0.01, 10.0, 0.0 });
        TArray<float> DirectTargets = { 0.0f };
        FUERLRobotCommandSlotView DirectSlot{ Mesh, &Indices, &DirectTargets };
        const FTransform ParentFrame = Joint->GetRefFrame(EConstraintFrame::Frame2) * Cart->GetUnrealWorldTransform();
        const FVector Axis = ParentFrame.GetRotation().RotateVector(LocalAxis);
        const FQuat MassRotation = Pole->GetMassSpaceToWorldSpace().GetRotation();
        const FVector PrincipalI = Pole->GetBodyInertiaTensor() / 10000.0;
        const FVector AxisInMass = MassRotation.UnrotateVector(Axis);
        const FVector Offset = (Pole->GetCOMPosition() - ParentFrame.GetLocation()) / 100.0;
        // Fixed-axis inertia includes the COM-to-anchor parallel-axis term.
        // Inventory it before applying commands; never derive it from motion.
        const double Inertia = FVector::DotProduct(PrincipalI * AxisInMass, AxisInMass)
            + Mass * FVector::CrossProduct(Offset, Axis).SizeSquared();
        if (!TestTrue(TEXT("positive finite fixed-axis inertia"), FMath::IsFinite(Inertia) && Inertia > 1.0e-6)
            || !TestTrue(TEXT("declared pole mass reaches solver"), FMath::Abs(Pole->GetBodyMass() - Mass) < 1.0e-5)) { return false; }
        const double Torque = Inertia * 0.5; // Declared acceleration 0.5 rad/s^2.
        if (!TestTrue(TEXT("probe remains below the declared torque cap"), Torque < 10.0)) { return false; }
        const auto ReadQ = [&]()
        {
            const FTransform Child = Joint->GetRefFrame(EConstraintFrame::Frame1) * Pole->GetUnrealWorldTransform();
            const FQuat Relative = (ParentFrame.GetRotation().Inverse() * Child.GetRotation()).GetNormalized();
            return 2.0 * FMath::Atan2(FVector::DotProduct(FVector(Relative.X, Relative.Y, Relative.Z), LocalAxis), Relative.W);
        };
        const double Q0 = ReadQ();
        const double W0 = FVector::DotProduct(Pole->GetUnrealWorldAngularVelocityInRadians(), Axis);
        double ExpectedW = W0, ExpectedQ = Q0, MaximumWError = 0.0, Elapsed = 0.0;
        const int32 Steps = FMath::RoundToInt(0.1 / Dt);
        for (double Multiplier : { Sign, 0.0, -Sign })
        {
            const float Targets[] = { static_cast<float>(Multiplier * Torque) };
            DirectTargets[0] = Targets[0];
            if (SolverCallback && !Runtime.ApplyActuatorTargets(Targets, Error)) { AddError(Error); return false; }
            for (int32 Step = 0; Step < Steps; ++Step)
            {
                constexpr double Damping = 0.01;
                const double Rate = Damping / Inertia;
                const double EquilibriumW = Multiplier * Torque / Damping;
                const double Decay = FMath::Exp(-Rate * Dt);
                ExpectedQ += EquilibriumW * Dt + (ExpectedW - EquilibriumW) * (1.0 - Decay) / Rate;
                ExpectedW = EquilibriumW + (ExpectedW - EquilibriumW) * Decay;
                Elapsed += Dt;
                if (!SolverCallback && !ApplyGenericRobotActuatorForces(DirectSlot, Topology, EffortActuators, Error))
                { AddError(Error); return false; }
                if (!Tick(*this, *World, Dt)) { return false; }
                const double W = FVector::DotProduct(Pole->GetUnrealWorldAngularVelocityInRadians(), Axis);
                MaximumWError = FMath::Max(MaximumWError, FMath::Abs(W - ExpectedW));
                TestTrue(TEXT("actual revolute effort has the predicted signed angular velocity"),
                    FMath::Abs(W - ExpectedW) <= 0.001 + FMath::Abs(ExpectedW) * 0.02);
                TestTrue(TEXT("actual revolute effort has the predicted angle including the lever-arm inertia"),
                    FMath::Abs(ReadQ() - ExpectedQ) <= 0.0001 + 0.5 * Elapsed * Dt);
                TestTrue(TEXT("the reaction parent remains fixed"),
                    Cart->GetUnrealWorldVelocity().Size() / 100.0 < 0.001
                    && Cart->GetUnrealWorldAngularVelocityInRadians().Size() < 0.001);
                TArray<float> State;
                if (!Runtime.CollectState(State, Error) || State.Num() != 2) { AddError(Error); return false; }
                TestTrue(TEXT("product torque feedback matches independent geometry and body velocity"),
                    FMath::Abs(State[0] - ReadQ()) < 0.0001 && FMath::Abs(State[1] - W) < 0.0001);
            }
        }
        AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] product_torque callback=%d mass=%.6f yaw=%.1f dt=%.6f inertia=%.9f torque_nm=%.9f max_w_error=%.9f"),
            SolverCallback, Mass, Yaw, Dt, Inertia, Torque, MaximumWError));
    }
    return true;
}

#endif
