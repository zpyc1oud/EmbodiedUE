#include "UERLTargetCaptureComponent.h"

#include "Components/PrimitiveComponent.h"
#include "GameFramework/Actor.h"

void UUERLTargetCaptureComponent::Bind(UPrimitiveComponent& Target, AActor& Robot)
{
	Unbind();
	TargetComponent = &Target;
	RobotActor = &Robot;
	Target.OnComponentHit.AddDynamic(this, &UUERLTargetCaptureComponent::HandleHit);
}

void UUERLTargetCaptureComponent::Unbind()
{
	if (UPrimitiveComponent* Target = TargetComponent.Get())
	{
		Target->OnComponentHit.RemoveDynamic(this, &UUERLTargetCaptureComponent::HandleHit);
	}
	TargetComponent.Reset();
	RobotActor.Reset();
	ResetCapture();
}

void UUERLTargetCaptureComponent::BeginDestroy()
{
	Unbind();
	Super::BeginDestroy();
}

void UUERLTargetCaptureComponent::HandleHit(
	UPrimitiveComponent* HitComponent, AActor* OtherActor,
	UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit)
{
	if (bWindowActive && HasValidBinding() && HitComponent == TargetComponent.Get()
		&& OtherActor == RobotActor.Get() && Hit.bBlockingHit)
	{
		bCaptured = true;
	}
}
