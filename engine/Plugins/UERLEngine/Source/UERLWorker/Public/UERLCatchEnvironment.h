#pragma once

#include "CoreMinimal.h"
#include "UERLProvider.h"

namespace UERLCatch
{
	UERLWORKER_API extern const FName EnvironmentId;
	UERLWORKER_API extern const FName TargetPositionField;
	UERLWORKER_API extern const FName TargetCaptureField;
	UERLWORKER_API TSharedRef<IUERLEnvironmentFactory> MakeFactory();
}
