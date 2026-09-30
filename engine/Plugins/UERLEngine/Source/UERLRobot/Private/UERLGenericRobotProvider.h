#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

#include "UERLGenericRobotKinematics.h"
#include "UERLGenericRobotStateFields.h"
#include "UERLProvider.h"

namespace UERLGenericRobot
{
	/** Stable id for the first generic SkeletalMesh Robot provider. */
	UERLROBOT_API extern const FName RobotId;
	/** Opt into claiming one matching authored SkeletalMesh Actor instead of spawning a copy. */
	UERLROBOT_API extern const FName ClaimAuthoredActor;
	/**
	 * Re-apply effort actuators before every Chaos solver step from the physics
	 * thread, for hosts that run several synchronous substeps per game frame.
	 */
	UERLROBOT_API extern const FName SolverStepCommands;

	/** Create the generic SkeletalMesh Robot factory for module registration. */
	UERLROBOT_API TSharedRef<IUERLRobotFactory> MakeFactory();
}
