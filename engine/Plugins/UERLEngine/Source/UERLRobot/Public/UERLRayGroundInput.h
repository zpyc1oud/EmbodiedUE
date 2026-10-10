#pragma once

#include "CoreMinimal.h"
#include "UERLInputProvider.h"

/** Construct the configurable numeric ground-ray factory for explicit registration. */
UERLROBOT_API TSharedRef<IUERLInputFactory> MakeUERLRayGroundInputFactory();
