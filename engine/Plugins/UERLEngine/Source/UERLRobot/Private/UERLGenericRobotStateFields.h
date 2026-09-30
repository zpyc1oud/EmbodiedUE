#pragma once

#include "CoreMinimal.h"

#include "UERLProvider.h"

namespace UERLGenericRobot
{
	/** Build the dynamic State descriptors from reflected bodies and scalar joints. */
	UERLROBOT_API bool BuildGenericRobotStateFields(
		const FUERLRobotTopology& Topology,
		TArray<FUERLFieldDescriptor>& OutFields,
		FString& OutError);

	/** Build the packed actuator-target Action field for the negotiated schema. */
	UERLROBOT_API FUERLFieldDescriptor BuildGenericRobotActuatorActionField(
		const TArray<FUERLActuatorConfig>& Actuators);

	/**
	 * Validate provider config, reflect topology, and publish descriptor fields.
	 * On success OutEffective mirrors Input and InOutDescriptor is filled.
	 */
	UERLROBOT_API bool ValidateGenericRobotProviderConfig(
		const FUERLProviderConfig& Input,
		FUERLRobotDescriptor& InOutDescriptor,
		FUERLProviderConfig& OutEffective,
		FString& OutError);
}
