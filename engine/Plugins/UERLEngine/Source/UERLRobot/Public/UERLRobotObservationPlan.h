#pragma once

#include "CoreMinimal.h"
#include "UERLInterfaceTypes.h"

/** Bind one selected robot State field to reflected topology indices. */
struct FUERLRobotObservationPlanEntry
{
	/** Identify the selected State field written through the Worker seam. */
	FName FieldName;
	/** Select the closed generic observation quantity. */
	EUERLObservationType Type = EUERLObservationType::None;
	/** Identify the reflected body index read by the provider. */
	int32 BodyIndex = INDEX_NONE;
	/** Identify the reflected joint index for scalar joint observations. */
	int32 JointIndex = INDEX_NONE;
	/** Store the selected field width in the State layout. */
	int32 Width = 0;
};

/**
 * Select fields owned by this Robot while preserving the selected schema order.
 * Environment-owned fields are omitted from the output.
 */
UERLROBOT_API void SelectRobotObservationFields(
	const TArray<FUERLFieldDescriptor>& AvailableFields,
	const TArray<FUERLFieldDescriptor>& SelectedFields,
	TArray<FUERLFieldDescriptor>& OutFields);

/**
 * Compile selected generic Robot observation descriptors against reflected topology.
 *
 * The returned entries preserve selected field order. A generic provider can use
 * this plan in its hot path without parsing names or JSON metadata per frame.
 * The input array must contain only fields belonging to the Robot provider.
 */
UERLROBOT_API bool CompileRobotObservationPlan(
	const FUERLRobotTopology& Topology,
	const TArray<FUERLFieldDescriptor>& SelectedFields,
	TArray<FUERLRobotObservationPlanEntry>& OutPlan,
	FString& OutError);
