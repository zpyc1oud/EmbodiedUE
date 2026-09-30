#pragma once

#include "CoreMinimal.h"

class USkeletalMeshComponent;

class FJsonObject;

/** Identify the reset distribution supported at the Worker boundary. */
enum class EUERLDistributionType : uint8
{
	Constant,
	Uniform,
};

/** Describe one transport-neutral reset distribution carried by worker_config. */
struct FUERLDistributionConfig
{
	/** Select whether the bounds represent a constant or uniform sample. */
	EUERLDistributionType Type = EUERLDistributionType::Constant;
	/** Store the constant value or lower uniform bound. */
	double Lower = 0.0;
	/** Store the constant value or upper uniform bound. */
	double Upper = 0.0;
	/** Identify the deterministic random stream used for uniform sampling. */
	FName StreamId;

	/** Build a valid constant distribution. */
	static FUERLDistributionConfig Constant(double Value, FName Stream = NAME_None)
	{
		FUERLDistributionConfig Result;
		Result.Lower = Value;
		Result.Upper = Value;
		Result.StreamId = Stream;
		return Result;
	}

	/** Build a uniform distribution bound to a named deterministic stream. */
	static FUERLDistributionConfig Uniform(double Min, double Max, FName Stream)
	{
		FUERLDistributionConfig Result;
		Result.Type = EUERLDistributionType::Uniform;
		Result.Lower = Min;
		Result.Upper = Max;
		Result.StreamId = Stream;
		return Result;
	}

	/** Validate finite bounds, ordering, and the stream requirement for uniform sampling. */
	bool IsValid() const
	{
		return FMath::IsFinite(Lower) && FMath::IsFinite(Upper) && Lower <= Upper
			&& (Type == EUERLDistributionType::Constant || !StreamId.IsNone());
	}
};

/** Carry one indexed actuator binding and its UE-side control coefficients. */
struct FUERLActuatorConfig
{
	/** Preserve the declaration-order column index. */
	int32 Index = INDEX_NONE;
	/** Identify the reflected scalar joint driven by this column. */
	int32 JointIndex = INDEX_NONE;
	/** Preserve the joint name for initialization diagnostics. */
	FName JointName;
	/** Identify the reflected coordinate type and coordinate name. */
	FString CoordinateType;
	FString Coordinate;
	/** Identify the target unit and derived control mode. */
	FString Unit;
	FString TargetMode;
	/** Store the unified actuator law coefficients in SI units. */
	double Stiffness = 0.0;
	double Damping = 0.0;
	double EffortLimit = 0.0;
	/** Store the neutral position target used before the first Action frame. */
	double DefaultPosition = 0.0;

	bool IsValid() const
	{
		return Index >= 0 && JointIndex >= 0 && !JointName.IsNone()
			&& !CoordinateType.IsEmpty() && !Coordinate.IsEmpty() && !Unit.IsEmpty()
			&& (TargetMode == TEXT("position") || TargetMode == TEXT("effort"))
			&& FMath::IsFinite(Stiffness) && FMath::IsFinite(Damping)
			&& FMath::IsFinite(EffortLimit) && FMath::IsFinite(DefaultPosition)
			&& Stiffness >= 0.0 && Damping >= 0.0 && EffortLimit > 0.0;
	}
};

/** Carry one indexed explicit reset target without distribution semantics. */
struct FUERLResetBinding
{
	/** Preserve the fixed reset vector column and stable row key. */
	int32 Index = INDEX_NONE;
	FName Name;
	/** Identify the explicit target kind and bound topology indices. */
	FString TargetType;
	int32 JointIndex = INDEX_NONE;
	int32 BodyIndex = INDEX_NONE;
	/** Identify the scalar component inside a vector root target. */
	int32 ComponentIndex = 0;
	FString Coordinate;
	FString Unit;

	bool IsValid() const
	{
		const bool bJoint = TargetType == TEXT("joint_position") || TargetType == TEXT("joint_velocity");
		const bool bRoot = TargetType == TEXT("root_pose") || TargetType == TEXT("root_velocity");
		const int32 ComponentCount = TargetType == TEXT("root_pose") ? 7 : 6;
		return Index >= 0 && !Name.IsNone() && (bJoint || bRoot)
			&& ((bJoint && JointIndex >= 0 && ComponentIndex == 0)
				|| (bRoot && BodyIndex >= 0 && ComponentIndex >= 0 && ComponentIndex < ComponentCount))
			&& !Unit.IsEmpty();
	}
};

/** Carry one indexed action column descriptor through schema negotiation. */
struct FUERLActuatorColumnDescriptor
{
	int32 Index = INDEX_NONE;
	FName JointName;
	int32 JointIndex = INDEX_NONE;
	FString CoordinateType;
	FString Unit;
	FString TargetMode;

	bool IsValid() const
	{
		return Index >= 0 && !JointName.IsNone() && JointIndex >= 0
			&& !CoordinateType.IsEmpty() && !Unit.IsEmpty()
			&& (TargetMode == TEXT("position") || TargetMode == TEXT("effort"));
	}

	bool operator==(const FUERLActuatorColumnDescriptor& Other) const
	{
		return Index == Other.Index && JointName == Other.JointName && JointIndex == Other.JointIndex
			&& CoordinateType == Other.CoordinateType && Unit == Other.Unit && TargetMode == Other.TargetMode;
	}
};

/** Carry fully consumed provider configuration without leaking provider types into Transport. */
struct FUERLProviderConfig
{
	/** Store provider scalar values keyed by their stable names. */
	TMap<FName, double> Scalars;
	/** Store reset distributions keyed by their stable parameter names. */
	TMap<FName, FUERLDistributionConfig> ResetDistributions;
	/** Store the optional UE object/package path for a generic Robot provider. */
	FString AssetPath;
	/** Store the indexed actuator projection consumed by a generic Robot. */
	TArray<struct FUERLActuatorConfig> Actuators;
	/** Store the fixed reset projection consumed by a generic Robot. */
	TArray<struct FUERLResetBinding> ResetBindings;
	/** Direct-runtime claim target; transport/provider configs leave this null. */
	USkeletalMeshComponent* ClaimedMesh = nullptr;
	/** Direct-runtime spawn transform; provider sessions leave identity. */
	FTransform PlacementTransform = FTransform::Identity;
	/** Whether PlacementTransform was intentionally supplied by a direct caller. */
	bool bHasPlacementTransform = false;

	/** Return whether the optional asset reference has UE object-path syntax. */
	static bool IsValidAssetPath(const FString& Path)
	{
		return Path.IsEmpty() || (Path.StartsWith(TEXT("/")) && !Path.Contains(TEXT("\\")));
	}
};

/** Identify one Session-mediated physical or scene event. */
enum class EUERLEventKind : uint8
{
	GroundFriction,
	TerrainTierParams,
	RootPush,
};

/** Return the stable wire spelling of one event kind. */
inline const TCHAR* UERLEventKindName(EUERLEventKind Kind)
{
	switch (Kind)
	{
	case EUERLEventKind::GroundFriction: return TEXT("ground_friction");
	case EUERLEventKind::TerrainTierParams: return TEXT("terrain_tier_params");
	case EUERLEventKind::RootPush: return TEXT("root_push");
	default: return TEXT("unknown");
	}
}

/** Carry selected per-Slot event values from Transport to the Worker domain. */
struct FUERLEventBatch
{
	/** Identify the event semantics carried by Values. */
	EUERLEventKind Kind = EUERLEventKind::GroundFriction;
	/** Identify selected Slots in stable batch order. */
	TArray<int32> SlotIds;
	/** Carry one value row per Slot: XY friction, X terrain scale, or XYZ velocity. */
	TArray<FVector> Values;
	/** Return the pre-generated tier selected for each terrain event row. */
	TArray<uint16> AppliedTerrainLevels;
};

/** Identify the geometry primitive a terrain tier is generated from. */
enum class EUERLTerrainPrimitive : uint8
{
	Plane,
	Heightfield,
	Boxes,
};

/** Describe one difficulty tier of the pre-generated terrain grid. */
struct FUERLTerrainTierConfig
{
	/** Store the contiguous difficulty index, starting at zero. */
	int32 Level = 0;
	/** Select the geometry primitive generated for this tier. */
	EUERLTerrainPrimitive Primitive = EUERLTerrainPrimitive::Plane;
	/** Store the deterministic generation seed for this tier. */
	uint64 Seed = 0;
	/** Store the spawn reset sampling window edge length; ignored by plane. */
	double PlatformWidth = 0.0;
	/** Carry the primitive-specific parameters consumed by the generator. */
	TSharedPtr<FJsonObject> Params;
};

/** Carry the full pre-generated terrain grid projected to the Worker. */
struct FUERLTerrainConfig
{
	/** Store the number of difficulty tiers; equals Tiers.Num(). */
	int32 NumLevels = 0;
	/** Store the surface size [x, y] of a single tier. */
	double CellSize[2] = { 0.0, 0.0 };
	/** Store the flat border width surrounding each tier. */
	double BorderWidth = 0.0;
	/**
	 * When false, generated terrain stays QueryOnly (no physics response).
	 * CartPole needs this: the PhysicsAsset prismatic joint is the rail, and a
	 * colliding plane freezes the cart exactly like the deleted track Environment
	 * avoided with QueryOnly meshes.
	 */
	bool bPhysicsCollision = true;
	/** Store the per-tier configuration ordered by contiguous level. */
	TArray<FUERLTerrainTierConfig> Tiers;

	/** Validate tier count, contiguous levels, and finite grid dimensions. */
	bool IsValid() const
	{
		if (NumLevels < 1 || Tiers.Num() != NumLevels
			|| !FMath::IsFinite(CellSize[0]) || CellSize[0] <= 0.0
			|| !FMath::IsFinite(CellSize[1]) || CellSize[1] <= 0.0
			|| !FMath::IsFinite(BorderWidth) || BorderWidth < 0.0)
		{
			return false;
		}
		for (int32 Index = 0; Index < Tiers.Num(); ++Index)
		{
			const FUERLTerrainTierConfig& Tier = Tiers[Index];
			if (Tier.Level != Index || !FMath::IsFinite(Tier.PlatformWidth) || Tier.PlatformWidth < 0.0)
			{
				return false;
			}
		}
		return true;
	}
};

/** Identify the closed set of body-state observations understood by the Worker. */
enum class EUERLObservationType : uint8
{
	None,
	JointPosition,
	JointVelocity,
	BodyPose,
	BodyLinearVelocity,
	BodyAngularVelocity,
	GroundClearance,
	Contact,
	ContactForce,
	TerrainHeight,
};

/** Number of forward samples in the generic Robot's local terrain-height scan. */
constexpr int32 UERLTerrainHeightScanForwardCount = 7;

/** Number of lateral samples in the generic Robot's local terrain-height scan. */
constexpr int32 UERLTerrainHeightScanLateralCount = 5;

/** Number of scalar samples in the generic Robot's local terrain-height scan. */
constexpr int32 UERLTerrainHeightScanWidth =
	UERLTerrainHeightScanForwardCount * UERLTerrainHeightScanLateralCount;

/** Carry the explicit topology binding attached to one robot observation field. */
struct FUERLObservationBinding
{
	/** Identify the observation quantity selected by Python. */
	EUERLObservationType Type = EUERLObservationType::None;
	/** Preserve the config-facing body name for diagnostics and response metadata. */
	FName BodyName;
	/** Identify the reflected body index used by the UE collector. */
	int32 BodyIndex = INDEX_NONE;
	/** Preserve the config-facing joint name for joint observations. */
	FName JointName;
	/** Identify the reflected joint index for joint position/velocity, if required. */
	int32 JointIndex = INDEX_NONE;

	/** Return whether this binding carries a complete, non-ambiguous selection. */
	bool IsValid() const
	{
		if (BodyName.IsNone() || BodyIndex < 0)
		{
			return false;
		}
		const bool bJointObservation = Type == EUERLObservationType::JointPosition
			|| Type == EUERLObservationType::JointVelocity;
		if (!bJointObservation && Type != EUERLObservationType::BodyPose
			&& Type != EUERLObservationType::BodyLinearVelocity
			&& Type != EUERLObservationType::BodyAngularVelocity
			&& Type != EUERLObservationType::GroundClearance
			&& Type != EUERLObservationType::Contact
			&& Type != EUERLObservationType::ContactForce
			&& Type != EUERLObservationType::TerrainHeight)
		{
			return false;
		}
		return (bJointObservation && JointIndex >= 0 && !JointName.IsNone())
			|| (!bJointObservation && JointIndex == INDEX_NONE && JointName.IsNone());
	}

	/** Compare bindings as part of selected-vs-available schema matching. */
	bool operator==(const FUERLObservationBinding& Other) const
	{
		return Type == Other.Type && BodyName == Other.BodyName
			&& BodyIndex == Other.BodyIndex && JointName == Other.JointName
			&& JointIndex == Other.JointIndex;
	}
};

/** Return the stable wire spelling of one observation type. */
inline const TCHAR* UERLObservationTypeName(EUERLObservationType Type)
{
	switch (Type)
	{
	case EUERLObservationType::JointPosition: return TEXT("joint_position");
	case EUERLObservationType::JointVelocity: return TEXT("joint_velocity");
	case EUERLObservationType::BodyPose: return TEXT("body_pose");
	case EUERLObservationType::BodyLinearVelocity: return TEXT("body_linear_velocity");
	case EUERLObservationType::BodyAngularVelocity: return TEXT("body_angular_velocity");
	case EUERLObservationType::GroundClearance: return TEXT("ground_clearance");
	case EUERLObservationType::Contact: return TEXT("contact");
	case EUERLObservationType::ContactForce: return TEXT("contact_force");
	case EUERLObservationType::TerrainHeight: return TEXT("terrain_height");
	default: return TEXT("none");
	}
}

/** Describe one named scalar or packed vector exposed during schema negotiation. */
struct FUERLFieldDescriptor
{
	/** Identify the stable field name used by both sides of the Bridge. */
	FName Name;
	/** Store the negotiated wire data type; MVP0 permits only float32. */
	FString DType = TEXT("float32");
	/** Store the field shape without the batch dimension. */
	TArray<int32> Shape;
	/** Store the declared physical unit or dimensionless marker. */
	FString Unit;
	/** Store the coordinate frame used to interpret the field. */
	FString CoordinateFrame;
	/** Store the semantic meaning of the field. */
	FString Semantic;
	/** Identify the Environment or Robot that publishes the field. */
	FString Source;
	/** Store the number of scalar elements represented by the field. */
	int32 Width = 1;
	/** Carry optional explicit robot observation binding metadata. */
	FUERLObservationBinding Observation;
	/** Carry optional indexed actuator column metadata for vector commands. */
	TArray<FUERLActuatorColumnDescriptor> ActuatorColumns;

	/** Validate metadata and ensure Width matches the declared shape. */
	bool IsValid() const
	{
		const bool bHasObservation = Observation.Type != EUERLObservationType::None
			|| !Observation.BodyName.IsNone() || Observation.BodyIndex != INDEX_NONE
			|| Observation.JointIndex != INDEX_NONE;
		if (Name.IsNone() || DType != TEXT("float32") || Width <= 0 || Unit.IsEmpty()
			|| CoordinateFrame.IsEmpty() || Semantic.IsEmpty() || Source.IsEmpty()
			|| (bHasObservation && !Observation.IsValid()))
		{
			return false;
		}
		if (!ActuatorColumns.IsEmpty())
		{
			if (ActuatorColumns.Num() != Width)
			{
				return false;
			}
			for (int32 Index = 0; Index < ActuatorColumns.Num(); ++Index)
			{
				if (!ActuatorColumns[Index].IsValid() || ActuatorColumns[Index].Index != Index)
				{
					return false;
				}
			}
		}
		int64 ShapeWidth = 1;
		for (int32 Dimension : Shape)
		{
			if (Dimension <= 0 || ShapeWidth > MAX_int32 / Dimension)
			{
				return false;
			}
			ShapeWidth *= Dimension;
		}
		return ShapeWidth == Width;
	}
};

/** Identify the single unlocked coordinate of a reflected PhysicsAsset joint. */
enum class EUERLJointCoordinate : uint8
{
	None,
	LinearX,
	LinearY,
	LinearZ,
	Swing1,
	Swing2,
	Twist,
};

/** Identify whether a joint coordinate is translational or rotational. */
enum class EUERLJointCoordinateType : uint8
{
	None,
	Prismatic,
	Revolute,
};

/** Identify the runtime motion type of a reflected PhysicsAsset body. */
enum class EUERLBodyMotionType : uint8
{
	Simulated,
	Kinematic,
};

/** Return the stable wire spelling of a reflected joint coordinate. */
inline const TCHAR* UERLJointCoordinateName(EUERLJointCoordinate Coordinate)
{
	switch (Coordinate)
	{
	case EUERLJointCoordinate::LinearX: return TEXT("linear_x");
	case EUERLJointCoordinate::LinearY: return TEXT("linear_y");
	case EUERLJointCoordinate::LinearZ: return TEXT("linear_z");
	case EUERLJointCoordinate::Swing1: return TEXT("swing1");
	case EUERLJointCoordinate::Swing2: return TEXT("swing2");
	case EUERLJointCoordinate::Twist: return TEXT("twist");
	default: return TEXT("none");
	}
}

/** Return the stable wire spelling of a reflected joint coordinate type. */
inline const TCHAR* UERLJointCoordinateTypeName(EUERLJointCoordinateType Type)
{
	switch (Type)
	{
	case EUERLJointCoordinateType::Prismatic: return TEXT("prismatic");
	case EUERLJointCoordinateType::Revolute: return TEXT("revolute");
	default: return TEXT("none");
	}
}

/** Return the SI unit for a reflected joint coordinate. */
inline const TCHAR* UERLJointCoordinateUnit(EUERLJointCoordinateType Type)
{
	switch (Type)
	{
	case EUERLJointCoordinateType::Prismatic: return TEXT("m");
	case EUERLJointCoordinateType::Revolute: return TEXT("rad");
	default: return TEXT("native");
	}
}

/** Describe one constraint frame in a body-local SI coordinate system. */
struct FUERLConstraintFrame
{
	/** Mark whether the frame was read from a PhysicsAsset constraint. */
	bool bValid = false;
	/** Store the frame origin in metres relative to the owning body. */
	FVector PositionMetres = FVector::ZeroVector;
	/** Store the frame orientation in Unreal's XYZW quaternion order. */
	FQuat Rotation = FQuat::Identity;
};

/** Describe one reflected joint of a robot topology. */
struct FUERLJointTopology
{
	/** Identify the joint by its stable constraint name. */
	FName Name;
	/** Identify the parent body of the joint by topology index. */
	int32 ParentBodyIndex = INDEX_NONE;
	/** Identify the child body driven by the joint by topology index. */
	int32 ChildBodyIndex = INDEX_NONE;
	/** Store the number of unlocked degrees of freedom. Valid generic joints have one. */
	int32 DegreesOfFreedom = 0;
	/** Store the unique unlocked constraint coordinate. */
	EUERLJointCoordinate Coordinate = EUERLJointCoordinate::None;
	/** Store whether the coordinate has a finite PhysicsAsset position limit. */
	bool bHasPositionLimit = false;
	/** Store the lower position limit in the coordinate's SI unit when bounded. */
	double LowerLimit = 0.0;
	/** Store the upper position limit in the coordinate's SI unit when bounded. */
	double UpperLimit = 0.0;
	/** Store the canonical asset reference position in the coordinate's SI unit. */
	double DefaultPosition = 0.0;
	/** Store whether the coordinate is prismatic or revolute. */
	EUERLJointCoordinateType CoordinateType = EUERLJointCoordinateType::None;
	/** Store the child-side constraint frame used for the coordinate axis. */
	FUERLConstraintFrame ChildFrame;
	/** Store the parent-side constraint frame used for the coordinate axis. */
	FUERLConstraintFrame ParentFrame;
};

/** Describe the reflected robot topology reported at initialization. */
struct FUERLRobotTopology
{
	/** List body names in stable topology index order. */
	TArray<FName> BodyNames;
	/** Store each body's effective simulated or kinematic motion type. */
	TArray<EUERLBodyMotionType> BodyMotionTypes;
	/** Identify the unique topology root body. */
	int32 RootBodyIndex = INDEX_NONE;
	/** Identify whether the root body is kinematic and therefore fixed-base. */
	bool bFixedBase = false;
	/** List the reflected joints referencing bodies by topology index. */
	TArray<FUERLJointTopology> Joints;
};
