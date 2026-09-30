#include "UERLGenericRobotStateFields.h"

#include "UERLGenericRobotKinematics.h"
#include "UERLGenericRobotProvider.h"
#include "UERLInterfaceTypes.h"
#include "UERLTopologyReflector.h"

#include "Engine/SkeletalMesh.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	FUERLFieldDescriptor BodyField(
		const FName BodyName,
		int32 BodyIndex,
		EUERLObservationType Type,
		int32 Width,
		const TCHAR* Unit,
		const TCHAR* Semantic)
	{
		FUERLFieldDescriptor Field;
		const TCHAR* Namespace = TEXT("body");
		Field.Name = FName(*FString::Printf(
			TEXT("robot.%s.%s.%s"), Namespace, *BodyName.ToString(), UERLObservationTypeName(Type)));
		if (Width > 1)
		{
			Field.Shape = { Width };
		}
		Field.Unit = Unit;
		Field.CoordinateFrame = Type == EUERLObservationType::Contact
			|| Type == EUERLObservationType::ContactForce
			? TEXT("coordinate-free") : TEXT("slot/local");
		Field.Semantic = Semantic;
		Field.Source = TEXT("uerl.robot");
		Field.Width = Width;
		Field.Observation.Type = Type;
		Field.Observation.BodyName = BodyName;
		Field.Observation.BodyIndex = BodyIndex;
		return Field;
	}

	FUERLFieldDescriptor JointField(
		const FName BodyName,
		int32 BodyIndex,
		const FName JointName,
		int32 JointIndex,
		EUERLObservationType Type,
		const TCHAR* Unit,
		const TCHAR* CoordinateFrame)
	{
		FUERLFieldDescriptor Field;
		Field.Name = FName(*FString::Printf(
			TEXT("robot.joint.%s.%s"), *JointName.ToString(), UERLObservationTypeName(Type)));
		Field.Unit = Unit;
		Field.CoordinateFrame = CoordinateFrame;
		Field.Semantic = UERLObservationTypeName(Type);
		Field.Source = TEXT("uerl.robot");
		Field.Width = 1;
		Field.Observation.JointName = JointName;
		Field.Observation.JointIndex = JointIndex;
		Field.Observation.BodyName = BodyName;
		Field.Observation.BodyIndex = BodyIndex;
		Field.Observation.Type = Type;
		return Field;
	}

	bool AddUniqueField(
		TArray<FUERLFieldDescriptor>& Fields,
		TSet<FName>& Names,
		FUERLFieldDescriptor Field,
		FString& OutError)
	{
		if (Names.Contains(Field.Name))
		{
			OutError = FString::Printf(TEXT("generic Robot publishes duplicate State field '%s'"), *Field.Name.ToString());
			return false;
		}
		Names.Add(Field.Name);
		Fields.Add(MoveTemp(Field));
		return true;
	}
}

FUERLFieldDescriptor UERLGenericRobot::BuildGenericRobotActuatorActionField(
	const TArray<FUERLActuatorConfig>& Actuators)
{
	FUERLFieldDescriptor Field;
	Field.Name = TEXT("robot.actuator.target");
	Field.Shape = { Actuators.Num() };
	Field.Unit = Actuators.IsEmpty() ? TEXT("native") : Actuators[0].Unit;
	for (const FUERLActuatorConfig& Actuator : Actuators)
	{
		if (Actuator.Unit != Field.Unit)
		{
			Field.Unit = TEXT("mixed");
			break;
		}
	}
	Field.CoordinateFrame = TEXT("slot/robot");
	Field.Semantic = TEXT("actuator_target");
	Field.Source = TEXT("uerl.robot");
	Field.Width = Actuators.Num();
	for (const FUERLActuatorConfig& Actuator : Actuators)
	{
		FUERLActuatorColumnDescriptor& Column = Field.ActuatorColumns.AddDefaulted_GetRef();
		Column.Index = Actuator.Index;
		Column.JointName = Actuator.JointName;
		Column.JointIndex = Actuator.JointIndex;
		Column.CoordinateType = Actuator.CoordinateType;
		Column.Unit = Actuator.Unit;
		Column.TargetMode = Actuator.TargetMode;
	}
	return Field;
}

bool UERLGenericRobot::BuildGenericRobotStateFields(
	const FUERLRobotTopology& Topology,
	TArray<FUERLFieldDescriptor>& OutFields,
	FString& OutError)
{
	OutFields.Reset();
	OutError.Reset();
	TSet<FName> Names;
	OutFields.Reserve(Topology.BodyNames.Num() * 5 + Topology.Joints.Num() * 2 + 1);
	for (int32 BodyIndex = 0; BodyIndex < Topology.BodyNames.Num(); ++BodyIndex)
	{
		const FName BodyName = Topology.BodyNames[BodyIndex];
		if (!AddUniqueField(OutFields, Names,
			BodyField(BodyName, BodyIndex, EUERLObservationType::BodyPose, 7, TEXT("m,quat_xyzw"), TEXT("body_pose")), OutError)
			|| !AddUniqueField(OutFields, Names,
			BodyField(BodyName, BodyIndex, EUERLObservationType::BodyLinearVelocity, 3, TEXT("m/s"), TEXT("body_linear_velocity")), OutError)
			|| !AddUniqueField(OutFields, Names,
			BodyField(BodyName, BodyIndex, EUERLObservationType::BodyAngularVelocity, 3, TEXT("rad/s"), TEXT("body_angular_velocity")), OutError)
			|| !AddUniqueField(OutFields, Names,
			BodyField(BodyName, BodyIndex, EUERLObservationType::GroundClearance, 1, TEXT("m"), TEXT("ground_clearance")), OutError)
			|| !AddUniqueField(OutFields, Names,
			BodyField(BodyName, BodyIndex, EUERLObservationType::Contact, 1, TEXT("fraction"), TEXT("contact")), OutError)
			|| !AddUniqueField(OutFields, Names,
			BodyField(BodyName, BodyIndex, EUERLObservationType::ContactForce, 1, TEXT("N"), TEXT("contact_force")), OutError))
		{
			OutFields.Reset();
			return false;
		}
	}
	if (Topology.BodyNames.IsValidIndex(Topology.RootBodyIndex)
		&& !AddUniqueField(OutFields, Names,
			BodyField(
				Topology.BodyNames[Topology.RootBodyIndex],
				Topology.RootBodyIndex,
				EUERLObservationType::TerrainHeight,
				UERLTerrainHeightScanWidth,
				TEXT("m"),
				TEXT("terrain_height")), OutError))
	{
		OutFields.Reset();
		return false;
	}
	for (int32 JointIndex = 0; JointIndex < Topology.Joints.Num(); ++JointIndex)
	{
		const FUERLJointTopology& Joint = Topology.Joints[JointIndex];
		if (Joint.DegreesOfFreedom != 1)
		{
			continue;
		}
		if (Joint.Coordinate == EUERLJointCoordinate::None)
		{
			OutError = FString::Printf(
				TEXT("generic Robot scalar joint %d has no resolved coordinate"), JointIndex);
			OutFields.Reset();
			return false;
		}
		if (!Topology.BodyNames.IsValidIndex(Joint.ChildBodyIndex))
		{
			OutError = FString::Printf(
				TEXT("generic Robot scalar joint %d references an invalid child body"), JointIndex);
			OutFields.Reset();
			return false;
		}
		const FName BodyName = Topology.BodyNames[Joint.ChildBodyIndex];
		const bool bAngular = IsGenericAngularCoordinate(Joint.Coordinate);
		const TCHAR* PositionUnit = bAngular ? TEXT("rad") : TEXT("m");
		const TCHAR* VelocityUnit = bAngular ? TEXT("rad/s") : TEXT("m/s");
		if (!AddUniqueField(OutFields, Names,
			JointField(BodyName, Joint.ChildBodyIndex, Joint.Name, JointIndex, EUERLObservationType::JointPosition,
				PositionUnit, TEXT("constraint")), OutError)
			|| !AddUniqueField(OutFields, Names,
			JointField(BodyName, Joint.ChildBodyIndex, Joint.Name, JointIndex, EUERLObservationType::JointVelocity,
				VelocityUnit, TEXT("constraint")), OutError))
		{
			OutFields.Reset();
			return false;
		}
	}
	return true;
}

bool UERLGenericRobot::ValidateGenericRobotProviderConfig(
	const FUERLProviderConfig& Input,
	FUERLRobotDescriptor& InOutDescriptor,
	FUERLProviderConfig& OutEffective,
	FString& OutError)
{
	InOutDescriptor.StateFields.Reset();
	InOutDescriptor.Topology = FUERLRobotTopology();
	if (!FUERLProviderConfig::IsValidAssetPath(Input.AssetPath) || Input.AssetPath.IsEmpty())
	{
		OutError = TEXT("generic Robot requires a non-empty UE object asset_path");
		return false;
	}
	if (!Input.ResetDistributions.IsEmpty())
	{
		OutError = TEXT("generic SkeletalMesh Robot does not accept UE reset distributions");
		return false;
	}
	for (const TPair<FName, double>& Pair : Input.Scalars)
	{
		if ((Pair.Key != ClaimAuthoredActor && Pair.Key != SolverStepCommands)
			|| (Pair.Value != 0.0 && Pair.Value != 1.0))
		{
			OutError = FString::Printf(
				TEXT("unknown or non-boolean generic Robot scalar '%s'"), *Pair.Key.ToString());
			return false;
		}
	}
	USkeletalMesh* Mesh = LoadObject<USkeletalMesh>(nullptr, *Input.AssetPath);
	if (!Mesh)
	{
		OutError = FString::Printf(
			TEXT("generic Robot asset is not a loadable USkeletalMesh: '%s'"), *Input.AssetPath);
		return false;
	}
	UPhysicsAsset* PhysicsAsset = Mesh->GetPhysicsAsset();
	if (!PhysicsAsset)
	{
		OutError = FString::Printf(
			TEXT("generic Robot SkeletalMesh has no PhysicsAsset: '%s'"), *Input.AssetPath);
		return false;
	}
	FUERLRobotTopology Topology;
	if (!FUERLTopologyReflector::ReflectSkeletalMesh(Mesh, Topology, OutError))
	{
		return false;
	}
	InOutDescriptor.Topology = MoveTemp(Topology);
	if (!BuildGenericRobotStateFields(InOutDescriptor.Topology, InOutDescriptor.StateFields, OutError))
	{
		return false;
	}
	for (int32 ActuatorIndex = 0; ActuatorIndex < Input.Actuators.Num(); ++ActuatorIndex)
	{
		const FUERLActuatorConfig& Actuator = Input.Actuators[ActuatorIndex];
		if (!Actuator.IsValid() || Actuator.Index != ActuatorIndex)
		{
			OutError = TEXT("generic Robot actuator projection is invalid or out of order");
			return false;
		}
		if (!InOutDescriptor.Topology.Joints.IsValidIndex(Actuator.JointIndex)
			|| InOutDescriptor.Topology.Joints[Actuator.JointIndex].Name != Actuator.JointName)
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' does not match reflected topology"),
				*Actuator.JointName.ToString());
			return false;
		}
		const FUERLJointTopology& Joint = InOutDescriptor.Topology.Joints[Actuator.JointIndex];
		if (Joint.CoordinateType == EUERLJointCoordinateType::Prismatic
			&& Actuator.CoordinateType != TEXT("prismatic"))
		{
			OutError = TEXT("generic Robot actuator coordinate type does not match topology");
			return false;
		}
		if (Joint.CoordinateType == EUERLJointCoordinateType::Revolute
			&& Actuator.CoordinateType != TEXT("revolute"))
		{
			OutError = TEXT("generic Robot actuator coordinate type does not match topology");
			return false;
		}
		if (Actuator.Coordinate != UERLJointCoordinateName(Joint.Coordinate))
		{
			OutError = TEXT("generic Robot actuator coordinate does not match topology");
			return false;
		}
		const FString ExpectedUnit = Actuator.TargetMode == TEXT("position")
			? UERLJointCoordinateUnit(Joint.CoordinateType)
			: Joint.CoordinateType == EUERLJointCoordinateType::Prismatic ? TEXT("N") : TEXT("N*m");
		if (Actuator.Unit != ExpectedUnit)
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' has unit '%s', expected '%s'"),
				*Actuator.JointName.ToString(), *Actuator.Unit, *ExpectedUnit);
			return false;
		}
		const FString ExpectedMode = Actuator.Stiffness > 0.0 ? TEXT("position") : TEXT("effort");
		if (Actuator.TargetMode != ExpectedMode)
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' has target mode '%s', expected '%s'"),
				*Actuator.JointName.ToString(), *Actuator.TargetMode, *ExpectedMode);
			return false;
		}
		if (Joint.bHasPositionLimit
			&& (Actuator.DefaultPosition < Joint.LowerLimit || Actuator.DefaultPosition > Joint.UpperLimit))
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' default position is outside the joint limit"),
				*Actuator.JointName.ToString());
			return false;
		}
		if (Actuator.Stiffness == 0.0 && Actuator.Damping == 0.0)
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' has zero gains"), *Actuator.JointName.ToString());
			return false;
		}
	}
	for (int32 ResetIndex = 0; ResetIndex < Input.ResetBindings.Num(); ++ResetIndex)
	{
		const FUERLResetBinding& Binding = Input.ResetBindings[ResetIndex];
		if (!Binding.IsValid() || Binding.Index != ResetIndex)
		{
			OutError = TEXT("generic Robot reset projection is invalid or out of order");
			return false;
		}
		if (Binding.TargetType.StartsWith(TEXT("root")) && InOutDescriptor.Topology.bFixedBase)
		{
			OutError = TEXT("generic Robot fixed-base topology rejects root reset targets");
			return false;
		}
		if (Binding.TargetType.StartsWith(TEXT("joint"))
			&& (!InOutDescriptor.Topology.Joints.IsValidIndex(Binding.JointIndex)
				|| InOutDescriptor.Topology.Joints[Binding.JointIndex].Name.IsNone()))
		{
			OutError = FString::Printf(
				TEXT("generic Robot reset '%s' does not match reflected topology"),
				*Binding.Name.ToString());
			return false;
		}
		if (Binding.TargetType.StartsWith(TEXT("joint"))
			&& Binding.Coordinate != UERLJointCoordinateName(
				InOutDescriptor.Topology.Joints[Binding.JointIndex].Coordinate))
		{
			OutError = FString::Printf(
				TEXT("generic Robot reset '%s' coordinate does not match topology"),
				*Binding.Name.ToString());
			return false;
		}
	}
	InOutDescriptor.ActionFields.Reset();
	if (!Input.Actuators.IsEmpty())
	{
		InOutDescriptor.ActionFields.Add(BuildGenericRobotActuatorActionField(Input.Actuators));
	}
	OutEffective = Input;
	return true;
}
