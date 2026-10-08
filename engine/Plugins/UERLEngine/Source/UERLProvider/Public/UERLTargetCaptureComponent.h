#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Engine/HitResult.h"
#include "UERLTargetCaptureComponent.generated.h"

class UPrimitiveComponent;
class AActor;

/** Latch a blocking target hit from one explicitly bound Robot during an owned control window. */
UCLASS()
class UERLPROVIDER_API UUERLTargetCaptureComponent final : public UActorComponent
{
	GENERATED_BODY()

public:
	void Bind(UPrimitiveComponent& Target, AActor& Robot);
	void Unbind();
	void BeginControlWindow() { bWindowActive = true; }
	void EndControlWindow() { bWindowActive = false; }
	void ResetCapture() { bWindowActive = false; bCaptured = false; }
	bool HasCaptured() const { return bCaptured; }
	bool HasValidBinding() const { return TargetComponent.IsValid() && RobotActor.IsValid(); }
	virtual void BeginDestroy() override;

private:
	UFUNCTION()
	void HandleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);

	TWeakObjectPtr<UPrimitiveComponent> TargetComponent;
	TWeakObjectPtr<AActor> RobotActor;
	bool bWindowActive = false;
	bool bCaptured = false;
};
