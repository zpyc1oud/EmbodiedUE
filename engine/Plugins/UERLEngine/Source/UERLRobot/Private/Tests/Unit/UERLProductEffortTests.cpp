#include "UERLPhysicsResponseTestSupport.h"

#include "Components/SkeletalMeshComponent.h"
#include "EngineUtils.h"
#include "UERLSkeletalMeshRobotRuntime.h"

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
			Actuator.Damping = 0.0;
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
			double DeclaredImpulse = 0.0;
			for (float Force : { 1.0f, 0.0f, -1.0f })
			{
				const float Targets[] = { Force };
				// Publish once. The production solver-step command owner must
				// retain this force and replace it with zero on the next phase.
				if (!Runtime.ApplyActuatorTargets(Targets, Error)) { AddError(Error); return false; }
				for (int32 Step = 0; Step < 40; ++Step)
				{
					if (!Tick(*this, *World, Dt)) { return false; }
					DeclaredImpulse += Force * Dt;
					const double Measured = MomentumAlongRail() - InitialMomentum;
					// The rail supplies no force along its free axis. All cart/pole
					// joint forces cancel in total momentum, even while the pole rotates.
					TestTrue(TEXT("generic held effort produces the independently declared whole-system impulse"),
						FMath::Abs(Measured - DeclaredImpulse) <= 0.001 + FMath::Abs(DeclaredImpulse) * 0.02);
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

#endif
