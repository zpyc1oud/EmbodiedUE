#pragma once

#include "CoreMinimal.h"
#include "UERLProvider.h"

namespace UERLAuthoredPursuit
{
	UERLWORKER_API extern const FName EnvironmentId;
	UERLWORKER_API extern const FName OriginX;
	UERLWORKER_API extern const FName OriginY;
	UERLWORKER_API extern const FName TraceStartZ;
	UERLWORKER_API extern const FName TraceDepth;
	UERLWORKER_API extern const FName TargetPositionField;

	/** Create the single-Slot authored-map Environment used by interactive pursuit demos. */
	UERLWORKER_API TSharedRef<IUERLEnvironmentFactory> MakeFactory();
}
