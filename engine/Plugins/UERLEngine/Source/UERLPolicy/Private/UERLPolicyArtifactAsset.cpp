#include "UERLPolicyArtifactAsset.h"

#include "Engine/SkeletalMesh.h"
#include "Misc/DataValidation.h"
#include "UERLTopologyReflector.h"

void FUERLPolicyArtifactSummary::Reset()
{
	TaskId = NAME_None;
	RobotId = NAME_None;
	PhysicsDt = 0.0;
	DecimationMin = 0;
	DecimationMax = 0;
	CommandChannels.Reset();
	CommandWidths.Reset();
	OnnxBytes = 0;
}

void FUERLPolicyArtifactSummary::ReadFrom(const FUERLPolicyArtifact& Artifact)
{
	TaskId = Artifact.TaskId();
	RobotId = Artifact.RobotId();
	PhysicsDt = Artifact.Timing().PhysicsDt;
	DecimationMin = Artifact.Timing().DecimationMin;
	DecimationMax = Artifact.Timing().DecimationMax;
	OnnxBytes = Artifact.OnnxBytes().Num();
	CommandChannels.Reset();
	CommandWidths.Reset();
	for (const FUERLPlanOp& Op : Artifact.ObservationPlan().Ops)
	{
		if (Op.Op != FName(TEXT("command")))
		{
			continue;
		}
		const FString* Channel = Op.Params.Strings.Find(TEXT("channel"));
		if (!Channel || Channel->IsEmpty())
		{
			continue;
		}
		const FName ChannelName(*Channel);
		const int32 Existing = CommandChannels.IndexOfByKey(ChannelName);
		if (Existing == INDEX_NONE)
		{
			CommandChannels.Add(ChannelName);
			CommandWidths.Add(Op.Width);
		}
	}
}

bool UUERLPolicyArtifactAsset::LoadArtifact(FUERLPolicyArtifact& OutArtifact, FString& OutError) const
{
	if (ArtifactBytes.IsEmpty())
	{
		OutError = TEXT("policy asset has no ArtifactBytes");
		return false;
	}
	return OutArtifact.LoadFromBytes(ArtifactBytes, OutError);
}

bool UUERLPolicyArtifactAsset::SetImportedBytes(TConstArrayView<uint8> Bytes, FString& OutError)
{
	FUERLPolicyArtifact Parsed;
	if (!Parsed.LoadFromBytes(Bytes, OutError))
	{
		return false;
	}
	ArtifactBytes.Reset(Bytes.Num());
	ArtifactBytes.Append(Bytes.GetData(), Bytes.Num());
	Summary.ReadFrom(Parsed);
	OutError.Reset();
	return true;
}

bool UUERLPolicyArtifactAsset::ValidateRobotMesh(FString& OutError) const
{
	OutError.Reset();
	if (!RobotMesh)
	{
		OutError = TEXT("policy asset requires a RobotMesh reference");
		return false;
	}
	if (!RobotMesh->GetPhysicsAsset())
	{
		OutError = FString::Printf(TEXT("RobotMesh '%s' has no PhysicsAsset"), *RobotMesh->GetPathName());
		return false;
	}
	FUERLRobotTopology Topology;
	if (!FUERLTopologyReflector::ReflectSkeletalMesh(RobotMesh, Topology, OutError))
	{
		return false;
	}
	return true;
}

bool UUERLPolicyArtifactAsset::ValidateForDeployment(FString& OutError) const
{
	FUERLPolicyArtifact Parsed;
	if (!LoadArtifact(Parsed, OutError))
	{
		OutError += TEXT("; reimport the .uerlpol2 source to repair the asset");
		return false;
	}
	if (!RobotMesh)
	{
		OutError = FString::Printf(
			TEXT("policy asset '%s' has no resolvable RobotMesh; assign the deployment SkeletalMesh in the asset editor, or migrate the mesh assets to the path the artifact references"),
			*GetPathName());
		return false;
	}
	if (!ValidateRobotMesh(OutError))
	{
		return false;
	}
	OutError.Reset();
	return true;
}

#if WITH_EDITOR
EDataValidationResult UUERLPolicyArtifactAsset::IsDataValid(FDataValidationContext& Context) const
{
	FString Error;
	if (!ValidateForDeployment(Error))
	{
		Context.AddError(FText::FromString(Error));
		return EDataValidationResult::Invalid;
	}
	return EDataValidationResult::Valid;
}
#endif
