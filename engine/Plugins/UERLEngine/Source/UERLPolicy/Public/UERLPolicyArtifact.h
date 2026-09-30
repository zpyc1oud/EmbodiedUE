#pragma once

#include "CoreMinimal.h"
#include "UERLPlan.h"
#include "UERLSkeletalMeshRobotRuntime.h"

/** Actuator drive parameters carried in the artifact ``robot_runtime`` segment. */
struct UERLPOLICY_API FUERLPolicyRobotRuntimeActuator
{
	FName JointName;
	double Stiffness = 0.0;
	double Damping = 0.0;
	double EffortLimit = 0.0;
	double DefaultPosition = 0.0;
};

/** Deployment robot-runtime segment: actuator parameters only (no topology map). */
struct UERLPOLICY_API FUERLPolicyRobotRuntime
{
	TArray<FUERLPolicyRobotRuntimeActuator> Actuators;

	void Reset() { Actuators.Reset(); }
};

/** Physics timing contract carried in the artifact ``timing`` segment. */
struct UERLPOLICY_API FUERLPolicyArtifactTiming
{
	double PhysicsDt = 0.0;
	int32 DecimationMin = 0;
	int32 DecimationMax = 0;

	double DtMin() const { return PhysicsDt * static_cast<double>(DecimationMin); }
	double DtMax() const { return PhysicsDt * static_cast<double>(DecimationMax); }
};

/**
 * Load a UERLPOL2 policy artifact: our observation/action plans plus raw ONNX bytes.
 *
 * This type only splits the container. It does not parse the ONNX graph — that
 * is NNE's job (ticket 14).
 */
class UERLPOLICY_API FUERLPolicyArtifact
{
public:
	bool Load(const FString& Path, FString& OutError);
	bool LoadFromBytes(TConstArrayView<uint8> Bytes, FString& OutError);

	const FUERLObservationPlan& ObservationPlan() const { return Observation; }
	const FUERLActionPlan& ActionPlan() const { return Action; }
	const FUERLPolicyRobotRuntime& RobotRuntime() const { return Runtime; }
	const FUERLPolicyArtifactTiming& Timing() const { return ArtifactTiming; }
	TConstArrayView<uint8> OnnxBytes() const { return Onnx; }
	FName TaskId() const { return Task; }
	FName RobotId() const { return Robot; }
	uint32 FormatVersion() const { return Version; }
	bool IsLoaded() const { return bLoaded; }

	void Reset();

private:
	bool bLoaded = false;
	uint32 Version = 0;
	FName Task;
	FName Robot;
	FUERLObservationPlan Observation;
	FUERLActionPlan Action;
	FUERLPolicyRobotRuntime Runtime;
	FUERLPolicyArtifactTiming ArtifactTiming;
	TArray<uint8> Onnx;
};
