#pragma once

#include "CoreMinimal.h"
#include "UERLProvider.h"

namespace UERLIsolatedGrid
{
	UERLWORKER_API extern const FName EnvironmentId;
	UERLWORKER_API extern const FName SpacingX;
	UERLWORKER_API extern const FName SpacingY;
	UERLWORKER_API extern const FName Columns;
	UERLWORKER_API extern const FName OriginX;
	UERLWORKER_API extern const FName OriginY;

	/** Create Slot-local grid anchors for procedural terrain and isolated collision. */
	UERLWORKER_API TSharedRef<IUERLEnvironmentFactory> MakeFactory();
}
