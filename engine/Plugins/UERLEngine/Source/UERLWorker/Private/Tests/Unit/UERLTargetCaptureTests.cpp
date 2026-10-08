#include "UERLTargetCaptureComponent.h"

#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTargetCaptureOwnershipTest, "UERL.Unit.Worker.Catch.ContactOwnership",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTargetCaptureOwnershipTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	if (!TestNotNull(TEXT("test World"), World)) { return false; }
	AActor* Target = World->SpawnActor<AActor>();
	AActor* Robot = World->SpawnActor<AActor>();
	AActor* Other = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("target"), Target) || !TestNotNull(TEXT("robot"), Robot)
		|| !TestNotNull(TEXT("other actor"), Other))
	{
		World->DestroyWorld(false);
		return false;
	}
	UBoxComponent* Shape = NewObject<UBoxComponent>(Target);
	UUERLTargetCaptureComponent* Capture = NewObject<UUERLTargetCaptureComponent>(Target);
	Capture->Bind(*Shape, *Robot);
	FHitResult Hit;
	Hit.bBlockingHit = true;
	Shape->OnComponentHit.Broadcast(Shape, Robot, nullptr, FVector::ZeroVector, Hit);
	TestFalse(TEXT("idle hits do not capture"), Capture->HasCaptured());
	Capture->BeginControlWindow();
	Shape->OnComponentHit.Broadcast(Shape, Other, nullptr, FVector::ZeroVector, Hit);
	TestFalse(TEXT("unrelated actor cannot capture"), Capture->HasCaptured());
	Hit.bBlockingHit = false;
	Shape->OnComponentHit.Broadcast(Shape, Robot, nullptr, FVector::ZeroVector, Hit);
	TestFalse(TEXT("nonblocking event cannot capture"), Capture->HasCaptured());
	Hit.bBlockingHit = true;
	Shape->OnComponentHit.Broadcast(Shape, Robot, nullptr, FVector::ZeroVector, Hit);
	TestTrue(TEXT("bound robot blocking hit captures"), Capture->HasCaptured());
	Capture->EndControlWindow();
	TestTrue(TEXT("capture remains latched until reset"), Capture->HasCaptured());
	Capture->ResetCapture();
	Shape->OnComponentHit.Broadcast(Shape, Robot, nullptr, FVector::ZeroVector, Hit);
	TestFalse(TEXT("reset clears and disarms capture"), Capture->HasCaptured());
	Capture->Unbind();
	Capture->BeginControlWindow();
	Shape->OnComponentHit.Broadcast(Shape, Robot, nullptr, FVector::ZeroVector, Hit);
	TestFalse(TEXT("unbound listener cannot capture"), Capture->HasCaptured());
	World->DestroyWorld(false);
	return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "PreviewScene.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLTargetCapturePhysicsTest, "UERL.Integration.Worker.Catch.BlockingContact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLTargetCapturePhysicsTest::RunTest(const FString& Parameters)
{
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues().SetCreateDefaultLighting(false)
		.SetCreatePhysicsScene(true).ShouldSimulatePhysics(true).SetTransactional(false).SetEditor(false));
	UWorld* World = Scene->GetWorld();
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("physics World"), World) || !TestNotNull(TEXT("cube collision asset"), Cube)) { return false; }
	AActor* Target = World->SpawnActor<AActor>();
	AActor* Robot = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("target"), Target) || !TestNotNull(TEXT("robot"), Robot)) { return false; }
	const auto MakeBody = [Cube](AActor& Owner, const FVector& Position, bool bSimulate)
	{
		UStaticMeshComponent* Body = NewObject<UStaticMeshComponent>(&Owner);
		Owner.SetRootComponent(Body);
		Owner.AddInstanceComponent(Body);
		Body->SetMobility(EComponentMobility::Movable);
		Body->SetStaticMesh(Cube);
		Body->SetWorldLocation(Position);
		Body->SetWorldScale3D(FVector(0.1));
		Body->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Body->SetCollisionObjectType(ECC_PhysicsBody);
		Body->SetCollisionResponseToAllChannels(ECR_Block);
		Body->SetNotifyRigidBodyCollision(true);
		Body->RegisterComponent();
		Body->SetEnableGravity(false);
		Body->SetSimulatePhysics(bSimulate);
		return Body;
	};
	UStaticMeshComponent* TargetBody = MakeBody(*Target, FVector(100.0, 0.0, 100.0), false);
	UStaticMeshComponent* RobotBody = MakeBody(*Robot, FVector(0.0, 0.0, 100.0), true);
	UUERLTargetCaptureComponent* Capture = NewObject<UUERLTargetCaptureComponent>(Target);
	Target->AddInstanceComponent(Capture);
	Capture->RegisterComponent();
	Capture->Bind(*TargetBody, *Robot);
	Capture->BeginControlWindow();
	for (int32 Frame = 0; Frame < 10; ++Frame) { World->Tick(ELevelTick::LEVELTICK_All, 0.005f); ++GFrameCounter; }
	TestFalse(TEXT("separated stationary bodies do not capture"), Capture->HasCaptured());
	RobotBody->SetPhysicsLinearVelocity(FVector(300.0, 0.0, 0.0));
	RobotBody->WakeAllRigidBodies();
	for (int32 Frame = 0; Frame < 100 && !Capture->HasCaptured(); ++Frame)
	{
		World->Tick(ELevelTick::LEVELTICK_All, 0.005f);
		++GFrameCounter;
	}
	TestTrue(TEXT("real Chaos blocking collision captures"), Capture->HasCaptured());
	Capture->ResetCapture();
	for (int32 Frame = 0; Frame < 10; ++Frame) { World->Tick(ELevelTick::LEVELTICK_All, 0.005f); ++GFrameCounter; }
	TestFalse(TEXT("physical contact outside an owned window does not recapture"), Capture->HasCaptured());
	Capture->Unbind();
	return true;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
#include "UERLCatchEnvironment.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLCatchDescriptorTest, "UERL.Unit.Worker.Catch.DescriptorAndConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLCatchDescriptorTest::RunTest(const FString& Parameters)
{
	TSharedRef<IUERLEnvironmentFactory> Factory = UERLCatch::MakeFactory();
	const FUERLEnvironmentDescriptor& Descriptor = Factory->Describe();
	if (!TestEqual(TEXT("two target state fields"), Descriptor.StateFields.Num(), 2)) { return false; }
	TestEqual(TEXT("position field"), Descriptor.StateFields[0].Name, UERLCatch::TargetPositionField);
	TestEqual(TEXT("position width"), Descriptor.StateFields[0].Width, 3);
	TestEqual(TEXT("capture field"), Descriptor.StateFields[1].Name, UERLCatch::TargetCaptureField);
	TestEqual(TEXT("capture width"), Descriptor.StateFields[1].Width, 1);
	FUERLProviderConfig Input;
	FUERLProviderConfig Effective;
	FString Error;
	if (!TestTrue(TEXT("default arena config"), Factory->ValidateConfig(Input, Effective, Error))) { return false; }
	TestEqual(TEXT("target starts 1.5m ahead"), Effective.Scalars[FName(TEXT("environment.target_x_m"))], 1.5);
	Input.Scalars.Add(FName(TEXT("environment.target_path_radius_m")), 0.0);
	TestFalse(TEXT("zero path radius rejected"), Factory->ValidateConfig(Input, Effective, Error));
	Input.Scalars.Reset();
	Input.Scalars.Add(FName(TEXT("environment.use_player_target")), 2.0);
	TestFalse(TEXT("invalid player mode rejected"), Factory->ValidateConfig(Input, Effective, Error));
	return true;
}
#endif
