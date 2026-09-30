#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "UERLEgyptChaseGameMode.generated.h"

class UUERLPolicyComponent;

/** Host demo: PhantomX chases Player 0 on Stylized_Egypt_Demo. */
UCLASS()
class AUERLEgyptChaseGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AUERLEgyptChaseGameMode();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	void TrySpawnRobot();

	UPROPERTY()
	TObjectPtr<UUERLPolicyComponent> PolicyComponent;

	bool bEgyptMap = false;
};
