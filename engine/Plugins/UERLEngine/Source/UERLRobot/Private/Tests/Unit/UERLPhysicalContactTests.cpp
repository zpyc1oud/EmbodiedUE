#include "UERLPhysicsResponseTestSupport.h"
#include "UERLContactWrenchTestSupport.h"

#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/Collision/ParticleCollisions.h"
#include "Chaos/ParticleHandle.h"
#include "Engine/StaticMeshActor.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "Physics/Experimental/PhysInterface_Chaos.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace UERLPhysicalContactTests
{
	UPhysicalMaterial* Material(UObject* Owner, float Friction, float Restitution)
	{
		UPhysicalMaterial* Result = NewObject<UPhysicalMaterial>(Owner);
		Result->Friction = Friction;
		Result->StaticFriction = Friction;
		Result->Restitution = Restitution;
		Result->bOverrideFrictionCombineMode = true;
		Result->FrictionCombineMode = EFrictionCombineMode::Average;
		Result->bOverrideRestitutionCombineMode = true;
		Result->RestitutionCombineMode = EFrictionCombineMode::Average;
		return Result;
	}

	UStaticMeshComponent* Ground(UWorld& World, float Friction, float Restitution)
	{
		AStaticMeshActor* Actor = World.SpawnActor<AStaticMeshActor>();
		if (!Actor) { return nullptr; }
		UStaticMeshComponent* Mesh = Actor->GetStaticMeshComponent();
		Mesh->SetMobility(EComponentMobility::Movable);
		Mesh->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Mesh->SetWorldScale3D(FVector(20.0, 20.0, 0.2));
		Mesh->SetWorldLocation(FVector(0.0, 0.0, -10.0)); // top face is z=0
		Mesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Mesh->SetCollisionObjectType(ECC_WorldStatic);
		Mesh->SetCollisionResponseToAllChannels(ECR_Block);
		Mesh->SetPhysMaterialOverride(Material(Actor, Friction, Restitution));
		// Register the final static body only after configuring its transform.
		Mesh->SetMobility(EComponentMobility::Static);
		return Mesh;
	}

	UStaticMeshComponent* Box(UWorld& World, double Mass, double HeightM, float Friction, float Restitution)
	{
		UStaticMeshComponent* Result = UERLPhysicsResponseTests::MakeCube(World, Mass);
		if (!Result) { return nullptr; }
		Result->SetWorldLocationAndRotation(FVector(0.0, 0.0, HeightM * 100.0), FQuat::Identity,
			false, nullptr, ETeleportType::TeleportPhysics);
		Result->SetCollisionObjectType(ECC_PhysicsBody);
		Result->SetCollisionResponseToAllChannels(ECR_Block);
		Result->SetEnableGravity(true);
		Result->SetPhysMaterialOverride(Material(Result->GetOwner(), Friction, Restitution));
		return Result;
	}

	// Read the completed constraint impulses. The expected impulse comes from
	// momentum balance, not from the production contact-force conversion.
	FVector ContactImpulse(FAutomationTestBase& Test, UPrimitiveComponent& Component, double Dt)
	{
		FVector Sum = FVector::ZeroVector;
		FBodyInstance* Body = Component.GetBodyInstance();
		FPhysicsActorHandle Handle = Body ? Body->GetPhysicsActorHandle() : nullptr;
		if (!Handle) { Test.AddError(TEXT("missing contact actor handle")); return Sum; }
		FPhysicsCommand::ExecuteRead(Handle, [&](const FPhysicsActorHandle& ReadHandle)
		{
			auto* Particle = ReadHandle && ReadHandle->GetHandle_LowLevel()
				? ReadHandle->GetHandle_LowLevel()->CastToRigidParticle() : nullptr;
			if (!Particle) { Test.AddError(TEXT("missing measured contact particle")); return; }
			auto* Collisions = UERLPhysicsResponseTests::CompletedContactContainer(Test, *Component.GetWorld());
			if (!Collisions) { return; }
			auto& Allocator = Collisions->GetConstraintAllocator();
			FVector AngularImpulse = FVector::ZeroVector;
			for (const auto* C : Collisions->GetConstraints())
			{
				if (!C || (C->GetParticle0() != Particle && C->GetParticle1() != Particle)) { continue; }
				if (!UERLPhysicsResponseTests::AccumulateContactWrench(Test, *C, Allocator, Particle, Dt,
					FVector::ZeroVector, Sum, AngularImpulse)) { return; }
			}
		});
		return Sum;
	}

	bool Settle(FAutomationTestBase& Test, UWorld& World, UStaticMeshComponent& Body)
	{
		for (int32 Step = 0; Step < 400; ++Step)
		{
			if (!UERLPhysicsResponseTests::Tick(Test, World, 0.005)) { return false; }
		}
		Test.AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] contact_settle com_m=%s velocity_m_s=%s"),
			*(Body.GetBodyInstance()->GetCOMPosition() / 100.0).ToString(),
			*(Body.GetBodyInstance()->GetUnrealWorldVelocity() / 100.0).ToString()));
		return Test.TestTrue(TEXT("contact fixture settles before the measured phase"),
			Body.GetBodyInstance()->GetUnrealWorldVelocity().Size() / 100.0 < 0.002
			&& FMath::Abs(Body.GetBodyInstance()->GetCOMPosition().Z / 100.0 - 0.1) < 0.003);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLStaticContactBalanceTest,
	"UERL.Integration.PhysicsResponse.Contact.StaticWeightAndImpulseBalance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLStaticContactBalanceTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	using namespace UERLPhysicalContactTests;
	FLockstepSettings Settings;
	for (double Mass : { 1.0, 2.0 })
	{
		auto Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		if (!World || !Ground(*World, 0.0f, 0.0f)) { AddError(TEXT("missing weight plane")); return false; }
		UStaticMeshComponent* Body = Box(*World, Mass, 0.12, 0.0f, 0.0f);
		if (!Body || !Settle(*this, *World, *Body)) { return false; }
		constexpr double Dt = 0.005;
		FVector Impulse = FVector::ZeroVector;
		const FVector StartV = Body->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0;
		for (int32 Step = 0; Step < 200; ++Step)
		{
			if (!Tick(*this, *World, Dt)) { return false; }
			Impulse += ContactImpulse(*this, *Body, Dt);
		}
		const FVector Gravity(0.0, 0.0, World->GetGravityZ() / 100.0);
		const FVector Expected = -Gravity * Mass; // one second of weight support
		const FVector MomentumChange = (Body->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0 - StartV) * Mass;
		const double Tolerance = 0.02 + Expected.Size() * 0.02;
		TestTrue(TEXT("signed support impulse balances declared weight"), Matches(Impulse, Expected, Tolerance));
		TestTrue(TEXT("contact and gravity impulses explain observed momentum change"),
			Matches(Impulse + Gravity * Mass, MomentumChange, Tolerance));
		AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] static_contact mass=%.3f expected_impulse=%s measured_impulse=%s tolerance=%.6f"),
			Mass, *Expected.ToString(), *Impulse.ToString(), Tolerance));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLZeroFrictionTest,
	"UERL.Integration.PhysicsResponse.Contact.ZeroFrictionPreservesMomentum",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLZeroFrictionTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	using namespace UERLPhysicalContactTests;
	FLockstepSettings Settings;
	auto Scene = MakeScene();
	UWorld* World = Scene->GetWorld();
	if (!World || !Ground(*World, 0.0f, 0.0f)) { return false; }
	UStaticMeshComponent* Body = Box(*World, 1.0, 0.12, 0.0f, 0.0f);
	if (!Body || !Settle(*this, *World, *Body)) { return false; }
	Body->GetBodyInstance()->SetLinearVelocity(FVector(50.0, -20.0, 0.0), false);
	const FVector Start = Body->GetBodyInstance()->GetCOMPosition() / 100.0;
	for (int32 Step = 0; Step < 100; ++Step)
	{
		if (!Tick(*this, *World, 0.005)) { return false; }
	}
	const FVector V = Body->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0;
	const FVector Delta = Body->GetBodyInstance()->GetCOMPosition() / 100.0 - Start;
	TestTrue(TEXT("zero-friction ground preserves tangential velocity"),
		FMath::Abs(V.X - 0.5) < 0.002 && FMath::Abs(V.Y + 0.2) < 0.002);
	TestTrue(TEXT("zero-friction displacement agrees with uniform sliding"),
		FMath::Abs(Delta.X - 0.25) < 0.002 && FMath::Abs(Delta.Y + 0.1) < 0.002);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLCoulombFrictionTest,
	"UERL.Integration.PhysicsResponse.Contact.StaticAndSlidingFriction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLCoulombFrictionTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	using namespace UERLPhysicalContactTests;
	FLockstepSettings Settings;
	// Keep force away from the stick/slip boundary. Equal materials make the
	// explicitly selected average combine rule unambiguous.
	for (bool Sliding : { false, true })
	{
		auto Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		if (!World || !Ground(*World, 0.4f, 0.0f)) { return false; }
		UStaticMeshComponent* Body = Box(*World, 1.0, 0.12, 0.4f, 0.0f);
		if (!Body || !Settle(*this, *World, *Body)) { return false; }
		const double G = -World->GetGravityZ() / 100.0;
		const double Force = (Sliding ? 1.5 : 0.5) * 0.4 * G;
		const FVector Start = Body->GetBodyInstance()->GetCOMPosition() / 100.0;
		// Initial sliding avoids ambiguity about exactly when static friction breaks.
		if (Sliding) { Body->GetBodyInstance()->SetLinearVelocity(FVector(20.0, 0.0, 0.0), false); }
		for (int32 Step = 0; Step < 40; ++Step)
		{
			Body->AddForce(FVector(Force * 100.0, 0.0, 0.0), NAME_None, false);
			if (!Tick(*this, *World, 0.005)) { return false; }
		}
		const double V = Body->GetBodyInstance()->GetUnrealWorldVelocity().X / 100.0;
		const double X = Body->GetBodyInstance()->GetCOMPosition().X / 100.0 - Start.X;
		if (!Sliding)
		{
			TestTrue(TEXT("subthreshold force stays inside static-friction displacement and speed bounds"),
				FMath::Abs(X) < 0.002 && FMath::Abs(V) < 0.005);
		}
		else
		{
			const double ExpectedV = 0.2 + (Force - 0.4 * G) * 0.2;
			TestTrue(TEXT("sliding acceleration agrees with the effective friction coefficient"),
				FMath::Abs(V - ExpectedV) < 0.01 + 0.03 * FMath::Abs(ExpectedV));
			AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] friction force_n=%.6f expected_v=%.6f actual_v=%.6f"), Force, ExpectedV, V));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FUERLRestitutionImpulseTest,
	"UERL.Integration.PhysicsResponse.Contact.RestitutionAndCollisionImpulse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLRestitutionImpulseTest::RunTest(const FString& Parameters)
{
	using namespace UERLPhysicsResponseTests;
	using namespace UERLPhysicalContactTests;
	FLockstepSettings Settings;
	for (double Restitution : { 0.0, 0.5 })
	{
		auto Scene = MakeScene();
		UWorld* World = Scene->GetWorld();
		if (!World || !Ground(*World, 0.0f, Restitution)) { return false; }
		UStaticMeshComponent* Body = Box(*World, 1.0, 0.6, 0.0f, Restitution);
		if (!Body || !Body->GetBodyInstance()) { return false; }
		constexpr double Dt = 0.0025;
		const double G = World->GetGravityZ() / 100.0;
		bool ImpactSeen = false;
		double ImpactV = 0.0;
		double ReboundV = 0.0;
		FVector CollisionImpulse = FVector::ZeroVector;
		FVector TotalContactImpulse = FVector::ZeroVector;
		const FVector InitialVelocity = Body->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0;
		int32 CompletedSteps = 0;
		TestTrue(TEXT("drop fixture has the declared one-kilogram mass"),
			FMath::Abs(Body->GetBodyInstance()->GetBodyMass() - 1.0) < 1.0e-5);
		for (int32 Step = 0; Step < 300; ++Step)
		{
			const double BeforeV = Body->GetBodyInstance()->GetUnrealWorldVelocity().Z / 100.0;
			if (!Tick(*this, *World, Dt)) { return false; }
			const FVector J = ContactImpulse(*this, *Body, Dt);
			TotalContactImpulse += J;
			++CompletedSteps;
			const double AfterV = Body->GetBodyInstance()->GetUnrealWorldVelocity().Z / 100.0;
			if (J.Size() > 0.001)
			{
				ImpactSeen = true;
				ImpactV = BeforeV + G * Dt;
				ReboundV = AfterV;
				CollisionImpulse = J;
				break;
			}
		}
		if (!TestTrue(TEXT("drop trial reaches a measured collision"), ImpactSeen)) { return false; }
		TestTrue(TEXT("collision impulse accounts for the velocity jump"),
			FMath::Abs(CollisionImpulse.Z - (ReboundV - ImpactV)) < 0.03);
		TestTrue(TEXT("rebound velocity agrees with the declared effective restitution"),
			FMath::Abs(ReboundV + Restitution * ImpactV) < 0.05 + 0.03 * FMath::Abs(ImpactV));
		TestTrue(TEXT("normal drop has no unexplained lateral collision impulse"),
			FVector(CollisionImpulse.X, CollisionImpulse.Y, 0.0).Size() < 0.02);
		// Follow the first rebound to its apex. A single velocity sample must
		// not replace a trajectory-level restitution check.
		double PeakHeight = Body->GetBodyInstance()->GetCOMPosition().Z / 100.0;
		double PreviousV = ReboundV;
		bool ApexSeen = Restitution == 0.0;
		for (int32 Step = 0; Step < 400; ++Step)
		{
			if (!Tick(*this, *World, Dt)) { return false; }
			TotalContactImpulse += ContactImpulse(*this, *Body, Dt);
			++CompletedSteps;
			PeakHeight = FMath::Max(PeakHeight, Body->GetBodyInstance()->GetCOMPosition().Z / 100.0);
			const double V = Body->GetBodyInstance()->GetUnrealWorldVelocity().Z / 100.0;
			if (Restitution > 0.0 && PreviousV > 0.0 && V <= 0.0) { ApexSeen = true; break; }
			PreviousV = V;
		}
		const double ExpectedPeak = 0.1 + Restitution * Restitution * (0.6 - 0.1);
		const FVector FinalVelocity = Body->GetBodyInstance()->GetUnrealWorldVelocity() / 100.0;
		const FVector ExpectedContactImpulse = FinalVelocity - InitialVelocity
			- FVector(0.0, 0.0, G * Dt * CompletedSteps); // declared mass is 1 kg
		TestTrue(TEXT("all collision and support impulses account for the complete drop and rebound momentum"),
			Matches(TotalContactImpulse, ExpectedContactImpulse, 0.03));
		AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] drop_balance steps=%d impulse=%s expected=%s error_ns=%.9f"),
			CompletedSteps, *TotalContactImpulse.ToString(), *ExpectedContactImpulse.ToString(),
			(TotalContactImpulse - ExpectedContactImpulse).Size()));
		const double HeightBudget = 0.005 + FMath::Sqrt(2.0 * FMath::Abs(G) * 0.5) * Dt;
		TestTrue(TEXT("the nonzero restitution trial reaches a rebound apex"), ApexSeen);
		TestTrue(TEXT("rebound height agrees with restitution-squared energy recovery"),
			FMath::Abs(PeakHeight - ExpectedPeak) <= HeightBudget);
		AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] rebound e=%.3f expected_height_m=%.9f height_m=%.9f budget_m=%.9f"),
			Restitution, ExpectedPeak, PeakHeight, HeightBudget));
		AddInfo(FString::Printf(TEXT("[PHYSICS_ORACLE] restitution e=%.3f impact_v=%.6f rebound_v=%.6f impulse=%s"),
			Restitution, ImpactV, ReboundV, *CollisionImpulse.ToString()));
	}
	return true;
}

#endif
