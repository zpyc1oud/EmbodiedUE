#pragma once

#include "CoreMinimal.h"
#include "UERLInterfaceTypes.h"

class USkeletalMeshComponent;
class UWorld;
struct FConstraintInstance;

/**
 * Slot-local inputs for actuator command application. The provider fills this
 * view from its Slot storage; the applier never owns component lifetime.
 */
struct FUERLRobotCommandSlotView
{
	USkeletalMeshComponent* Component = nullptr;
	const TArray<int32>* ConstraintIndices = nullptr;
	const TArray<float>* Targets = nullptr;
};

/** Return whether an actuator is held by a Chaos joint drive instead of an applied effort. */
UERLROBOT_API bool IsGenericRobotDriveActuator(const FUERLActuatorConfig& Actuator);

/**
 * Configure the Chaos angular drive of one revolute position actuator on its
 * reflected twist, swing1, or swing2 coordinate.
 * SI stiffness/damping/effort are converted to Chaos units only here.
 */
UERLROBOT_API void ConfigureGenericRobotRevoluteAngularDrive(
	FConstraintInstance& Constraint,
	const FUERLActuatorConfig& Actuator);

/** Configure the Chaos angular drives of every revolute position actuator. */
UERLROBOT_API void ConfigureGenericRobotRevoluteAngularDrives(
	USkeletalMeshComponent& Component,
	const TArray<int32>& ConstraintIndices,
	TConstArrayView<FUERLActuatorConfig> Actuators);

/**
 * Apply one Slot's cached actuator targets through unified effort law.
 * Prismatic effort conversion to Chaos force units lives only here.
 */
UERLROBOT_API bool ApplyGenericRobotActuatorForces(
	const FUERLRobotCommandSlotView& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FUERLActuatorConfig> Actuators,
	FString& OutError);

/**
 * Re-apply one Slot's effort actuators from physics-thread state before every
 * Chaos solver step. A host that runs several synchronous substeps per game
 * frame uses this where the training Worker re-applies once per lockstep frame.
 */
class UERLROBOT_API FUERLGenericRobotSolverStepCommands
{
public:
	FUERLGenericRobotSolverStepCommands();
	~FUERLGenericRobotSolverStepCommands();

	FUERLGenericRobotSolverStepCommands(const FUERLGenericRobotSolverStepCommands&) = delete;
	FUERLGenericRobotSolverStepCommands& operator=(const FUERLGenericRobotSolverStepCommands&) = delete;

	/** Register the solver callback for the non-drive actuators in Actuators. */
	bool Register(
		UWorld& World,
		const FUERLRobotCommandSlotView& Slot,
		const FUERLRobotTopology& Topology,
		TConstArrayView<FUERLActuatorConfig> Actuators,
		FString& OutError);
	/** Hold Targets from the next solver step until the next publication. */
	void PublishTargets(TConstArrayView<float> Targets);
	void Unregister();

private:
	class FImpl;
	TUniquePtr<FImpl> Impl;
};
