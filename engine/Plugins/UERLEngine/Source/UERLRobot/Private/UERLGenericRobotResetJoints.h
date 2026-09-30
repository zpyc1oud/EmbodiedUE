#pragma once

#include "CoreMinimal.h"
#include "UERLGenericRobotResetApplier.h"
#include "UERLInterfaceTypes.h"

bool SetGenericRobotJointReset(
	FUERLRobotResetSlotRef& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<int32> ParentBodyIndices,
	const FUERLResetBinding& Binding,
	double Value,
	FString& OutError,
	bool bApply);

bool ApplyGenericRobotJointResetBindings(
	FUERLRobotResetSlotRef& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<int32> ParentBodyIndices,
	TConstArrayView<FUERLResetBinding> ResetBindings,
	const FUERLResetRow& Row,
	const FString& TargetType,
	FString& OutError);
