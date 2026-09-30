#pragma once

#include "CoreMinimal.h"
#include "UERLInterfaceTypes.h"

class UPhysicsAsset;

/** Reflect PhysicsAsset-only structure for the SkeletalMesh reflector and its unit tests. */
bool ReflectPhysicsAssetTopology(
	const UPhysicsAsset* PhysicsAsset,
	FUERLRobotTopology& OutTopology,
	FString& OutError);
