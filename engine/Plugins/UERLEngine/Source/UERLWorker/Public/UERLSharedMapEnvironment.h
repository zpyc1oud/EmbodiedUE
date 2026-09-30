#pragma once

#include "CoreMinimal.h"
#include "UERLProvider.h"

namespace UERLSharedMap
{
	UERLWORKER_API extern const FName EnvironmentId;
	UERLWORKER_API extern const FName SpacingX;
	UERLWORKER_API extern const FName SpacingY;
	UERLWORKER_API extern const FName Columns;
	UERLWORKER_API extern const FName OriginX;
	UERLWORKER_API extern const FName OriginY;
	UERLWORKER_API extern const FName TraceStartZ;
	UERLWORKER_API extern const FName TraceDepth;

	/** Create the built-in Environment that places Slots on one loaded World map. */
	UERLWORKER_API TSharedRef<IUERLEnvironmentFactory> MakeFactory();
}
