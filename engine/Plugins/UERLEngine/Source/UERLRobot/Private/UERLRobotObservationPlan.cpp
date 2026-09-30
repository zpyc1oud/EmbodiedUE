#include "UERLRobotObservationPlan.h"

namespace
{
	int32 ExpectedObservationWidth(EUERLObservationType Type)
	{
		switch (Type)
		{
		case EUERLObservationType::JointPosition:
		case EUERLObservationType::JointVelocity:
		case EUERLObservationType::GroundClearance:
			return 1;
		case EUERLObservationType::BodyPose:
			return 7;
		case EUERLObservationType::BodyLinearVelocity:
		case EUERLObservationType::BodyAngularVelocity:
			return 3;
		case EUERLObservationType::Contact:
		case EUERLObservationType::ContactForce:
			return 1;
		case EUERLObservationType::TerrainHeight:
			return UERLTerrainHeightScanWidth;
		default:
			return 0;
		}
	}

	bool IsJointObservation(EUERLObservationType Type)
	{
		return Type == EUERLObservationType::JointPosition || Type == EUERLObservationType::JointVelocity;
	}
}

void SelectRobotObservationFields(
	const TArray<FUERLFieldDescriptor>& AvailableFields,
	const TArray<FUERLFieldDescriptor>& SelectedFields,
	TArray<FUERLFieldDescriptor>& OutFields)
{
	OutFields.Reset();
	TSet<FName> AvailableNames;
	AvailableNames.Reserve(AvailableFields.Num());
	for (const FUERLFieldDescriptor& Field : AvailableFields)
	{
		AvailableNames.Add(Field.Name);
	}
	for (const FUERLFieldDescriptor& Field : SelectedFields)
	{
		if (AvailableNames.Contains(Field.Name))
		{
			OutFields.Add(Field);
			continue;
		}
		const FUERLFieldDescriptor* Match = AvailableFields.FindByPredicate(
			[&Field](const FUERLFieldDescriptor& Candidate)
			{
				return Field.Observation.Type != EUERLObservationType::None
					&& Candidate.Observation == Field.Observation
					&& Candidate.Width == Field.Width
					&& Candidate.Unit == Field.Unit
					&& Candidate.CoordinateFrame == Field.CoordinateFrame
					&& Candidate.Semantic == Field.Semantic
					&& Candidate.Source == Field.Source;
			});
		if (Match)
		{
			OutFields.Add(Field);
		}
	}
}

bool CompileRobotObservationPlan(
	const FUERLRobotTopology& Topology,
	const TArray<FUERLFieldDescriptor>& SelectedFields,
	TArray<FUERLRobotObservationPlanEntry>& OutPlan,
	FString& OutError)
{
	OutPlan.Reset();
	OutError.Reset();
	TSet<FName> SeenFields;
	OutPlan.Reserve(SelectedFields.Num());

	for (const FUERLFieldDescriptor& Field : SelectedFields)
	{
		const FUERLObservationBinding& Observation = Field.Observation;
		const int32 ExpectedWidth = ExpectedObservationWidth(Observation.Type);
		if (!Field.IsValid() || Observation.Type == EUERLObservationType::None)
		{
			OutError = FString::Printf(TEXT("robot observation field '%s' has invalid binding"), *Field.Name.ToString());
			OutPlan.Reset();
			return false;
		}
		if (ExpectedWidth == 0 || Field.Width != ExpectedWidth)
		{
			OutError = FString::Printf(
				TEXT("robot observation field '%s' has width %d, expected %d"),
				*Field.Name.ToString(), Field.Width, ExpectedWidth);
			OutPlan.Reset();
			return false;
		}
		if (Observation.Type == EUERLObservationType::Contact
			&& (Field.Shape.Num() != 0 || Field.Unit != TEXT("fraction")
				|| Field.CoordinateFrame != TEXT("coordinate-free")
				|| Field.Semantic != TEXT("contact") || Field.Source != TEXT("uerl.robot")))
		{
			OutError = FString::Printf(
				TEXT("robot contact field '%s' has invalid scalar metadata"), *Field.Name.ToString());
			OutPlan.Reset();
			return false;
		}
		if (Observation.Type == EUERLObservationType::ContactForce
			&& (Field.Shape.Num() != 0 || Field.Unit != TEXT("N")
				|| Field.CoordinateFrame != TEXT("coordinate-free")
				|| Field.Semantic != TEXT("contact_force") || Field.Source != TEXT("uerl.robot")))
		{
			OutError = FString::Printf(
				TEXT("robot contact-force field '%s' has invalid scalar metadata"), *Field.Name.ToString());
			OutPlan.Reset();
			return false;
		}
		if (Observation.Type == EUERLObservationType::TerrainHeight
			&& (Field.Shape != TArray<int32>{ UERLTerrainHeightScanWidth }
				|| Field.Unit != TEXT("m")
				|| Field.CoordinateFrame != TEXT("slot/local")
				|| Field.Semantic != TEXT("terrain_height")
				|| Field.Source != TEXT("uerl.robot")))
		{
			OutError = FString::Printf(
				TEXT("robot terrain-height field '%s' has invalid scan metadata"), *Field.Name.ToString());
			OutPlan.Reset();
			return false;
		}
		if (IsJointObservation(Observation.Type) && Field.CoordinateFrame != TEXT("constraint"))
		{
			OutError = FString::Printf(TEXT("robot joint field '%s' must use the constraint frame"), *Field.Name.ToString());
			OutPlan.Reset();
			return false;
		}
		if (!IsJointObservation(Observation.Type)
			&& Observation.Type != EUERLObservationType::Contact
			&& Observation.Type != EUERLObservationType::ContactForce
			&& Field.CoordinateFrame != TEXT("slot/local"))
		{
			OutError = FString::Printf(TEXT("robot body field '%s' must use the Slot-local frame"), *Field.Name.ToString());
			OutPlan.Reset();
			return false;
		}
		if (Field.Name.IsNone() || SeenFields.Contains(Field.Name))
		{
			OutError = FString::Printf(TEXT("robot observation field '%s' is duplicated"), *Field.Name.ToString());
			OutPlan.Reset();
			return false;
		}
		SeenFields.Add(Field.Name);
		if (!Topology.BodyNames.IsValidIndex(Observation.BodyIndex)
			|| Topology.BodyNames[Observation.BodyIndex] != Observation.BodyName)
		{
			OutError = FString::Printf(
				TEXT("robot observation field '%s' body index/name does not match topology"),
				*Field.Name.ToString());
			OutPlan.Reset();
			return false;
		}
		if (Observation.Type == EUERLObservationType::TerrainHeight
			&& Observation.BodyIndex != Topology.RootBodyIndex)
		{
			OutError = FString::Printf(
				TEXT("robot terrain-height field '%s' must target the topology root body"),
				*Field.Name.ToString());
			OutPlan.Reset();
			return false;
		}

		FUERLRobotObservationPlanEntry& Entry = OutPlan.AddDefaulted_GetRef();
		Entry.FieldName = Field.Name;
		Entry.Type = Observation.Type;
		Entry.BodyIndex = Observation.BodyIndex;
		Entry.Width = Field.Width;
		if (IsJointObservation(Observation.Type))
		{
			if (!Topology.Joints.IsValidIndex(Observation.JointIndex))
			{
				OutError = FString::Printf(
					TEXT("robot observation field '%s' joint index is out of range"),
					*Field.Name.ToString());
				OutPlan.Reset();
				return false;
			}
			const FUERLJointTopology& Joint = Topology.Joints[Observation.JointIndex];
			if (Joint.Name != Observation.JointName)
			{
				OutError = FString::Printf(
					TEXT("robot observation field '%s' joint index/name does not match topology"),
					*Field.Name.ToString());
				OutPlan.Reset();
				return false;
			}
			if (Joint.ChildBodyIndex != Observation.BodyIndex || Joint.DegreesOfFreedom != 1)
			{
				OutError = FString::Printf(
					TEXT("robot observation field '%s' joint binding does not identify a scalar child joint"),
					*Field.Name.ToString());
				OutPlan.Reset();
				return false;
			}
			const bool bAngular = Joint.CoordinateType == EUERLJointCoordinateType::Revolute;
			const FString ExpectedUnit = Observation.Type == EUERLObservationType::JointPosition
				? (bAngular ? TEXT("rad") : TEXT("m"))
				: (bAngular ? TEXT("rad/s") : TEXT("m/s"));
			if (Field.Unit != ExpectedUnit)
			{
				OutError = FString::Printf(
					TEXT("robot joint field '%s' has unit '%s', expected '%s'"),
					*Field.Name.ToString(), *Field.Unit, *ExpectedUnit);
				OutPlan.Reset();
				return false;
			}
			Entry.JointIndex = Observation.JointIndex;
		}
	}
	return true;
}
