#pragma once

#include "CoreMinimal.h"
#include "UERLInterfaceTypes.h"

class USkeletalMesh;

/**
 * Reflect a PhysicsAsset / Skeletal Mesh into a name-carrying robot topology.
 *
 * The reflector reads bodies, joints (constraints), motion types, the unique
 * unlocked coordinate, SI position limits, and parent/child wiring without
 * interpreting robot semantics. Python owns the mapping from these names to
 * actuators, observations, and control modes.
 *
 * Structurally incomplete assets fail loudly: a missing PhysicsAsset, an empty
 * body set, a non-tree body graph, an invalid base, a multi-DOF constraint, or
 * a constraint that references an unknown body is a hard error, never a silent
 * downgrade.
 */
class UERLROBOT_API FUERLTopologyReflector
{
public:
	/** Reflect topology and canonical joint positions from one SkeletalMesh asset. */
	static bool ReflectSkeletalMesh(
		const USkeletalMesh* SkeletalMesh,
		FUERLRobotTopology& OutTopology,
		FString& OutError);
};
