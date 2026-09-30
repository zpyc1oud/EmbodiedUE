#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"

#include "UERLPolicyImportCommandlet.generated.h"

/** Import and validate one .uerlpol2 asset without opening the UE Editor UI. */
UCLASS()
class UERLPOLICYEDITOR_API UUERLPolicyImportCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UUERLPolicyImportCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;
};
