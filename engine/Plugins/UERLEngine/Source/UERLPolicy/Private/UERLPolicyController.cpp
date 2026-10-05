#include "UERLPolicyController.h"

#include "UERLInterfaceTypes.h"
#include "UERLPolicyOperators.h"

#include "Engine/World.h"
#include "Misc/FileHelper.h"

namespace
{
	bool ControllerFail(FString& OutError, const FString& Message)
	{
		OutError = Message;
		return false;
	}

	bool ParseObservationType(const FString& Text, EUERLObservationType& OutType)
	{
		if (Text == TEXT("joint_position"))
		{
			OutType = EUERLObservationType::JointPosition;
			return true;
		}
		if (Text == TEXT("joint_velocity"))
		{
			OutType = EUERLObservationType::JointVelocity;
			return true;
		}
		if (Text == TEXT("body_pose"))
		{
			OutType = EUERLObservationType::BodyPose;
			return true;
		}
		if (Text == TEXT("body_linear_velocity"))
		{
			OutType = EUERLObservationType::BodyLinearVelocity;
			return true;
		}
		if (Text == TEXT("body_angular_velocity"))
		{
			OutType = EUERLObservationType::BodyAngularVelocity;
			return true;
		}
		if (Text == TEXT("ground_clearance"))
		{
			OutType = EUERLObservationType::GroundClearance;
			return true;
		}
		if (Text == TEXT("contact"))
		{
			OutType = EUERLObservationType::Contact;
			return true;
		}
		if (Text == TEXT("contact_force"))
		{
			OutType = EUERLObservationType::ContactForce;
			return true;
		}
		if (Text == TEXT("terrain_height"))
		{
			OutType = EUERLObservationType::TerrainHeight;
			return true;
		}
		return false;
	}

	/**
	 * Parse ``robot.<joint|body>.<part>.<type>`` into a runtime observation selector.
	 * The part name is used directly — no remapping table.
	 */
	bool ParseObservationSelector(
		FName FieldName,
		FUERLSkeletalMeshRuntimeObservation& Out,
		FString& OutError)
	{
		TArray<FString> Parts;
		FieldName.ToString().ParseIntoArray(Parts, TEXT("."), true);
		if (Parts.Num() != 4 || Parts[0] != TEXT("robot"))
		{
			return ControllerFail(
				OutError,
				FString::Printf(
					TEXT("observation field '%s' must be robot.<joint|body>.<part>.<type>"),
					*FieldName.ToString()));
		}

		EUERLObservationType Type = EUERLObservationType::None;
		if (!ParseObservationType(Parts[3], Type))
		{
			return ControllerFail(
				OutError,
				FString::Printf(TEXT("unknown observation type '%s' in '%s'"), *Parts[3], *FieldName.ToString()));
		}

		const bool bJoint = Parts[1] == TEXT("joint");
		const bool bBody = Parts[1] == TEXT("body");
		if (!bJoint && !bBody)
		{
			return ControllerFail(
				OutError,
				FString::Printf(
					TEXT("observation field '%s' kind must be joint or body"),
					*FieldName.ToString()));
		}
		const bool bJointType = Type == EUERLObservationType::JointPosition
			|| Type == EUERLObservationType::JointVelocity;
		if (bJoint != bJointType)
		{
			return ControllerFail(
				OutError,
				FString::Printf(
					TEXT("observation field '%s' kind/type mismatch"),
					*FieldName.ToString()));
		}

		Out.Type = Type;
		Out.TargetName = FName(*Parts[2]);
		return true;
	}

	bool CollectRequiredCommands(
		const FUERLObservationPlan& Plan,
		TArray<FUERLPolicyCommandChannel>& OutChannels,
		TMap<FName, int32>& OutWidths,
		FString& OutError)
	{
		OutChannels.Reset();
		OutWidths.Reset();
		for (int32 OpIndex = 0; OpIndex < Plan.Ops.Num(); ++OpIndex)
		{
			const FUERLPlanOp& Op = Plan.Ops[OpIndex];
			if (Op.Op != FName(TEXT("command")))
			{
				continue;
			}
			const FString* ChannelText = Op.Params.Strings.Find(TEXT("channel"));
			if (ChannelText == nullptr || ChannelText->IsEmpty())
			{
				return ControllerFail(
					OutError,
					FString::Printf(TEXT("ops[%d] command missing params.channel"), OpIndex));
			}
			const FName Channel(*ChannelText);
			if (const int32* Existing = OutWidths.Find(Channel))
			{
				if (*Existing != Op.Width)
				{
					return ControllerFail(
						OutError,
						FString::Printf(
							TEXT("command channel '%s' declared with conflicting widths %d and %d"),
							*Channel.ToString(),
							*Existing,
							Op.Width));
				}
				continue;
			}
			OutWidths.Add(Channel, Op.Width);
			FUERLPolicyCommandChannel& Entry = OutChannels.AddDefaulted_GetRef();
			Entry.Name = Channel;
			Entry.Width = Op.Width;
		}
		return true;
	}
}

void FUERLPolicyCommands::Set(FName Channel, TConstArrayView<float> Values)
{
	TArray<float>& Slot = Channels.FindOrAdd(Channel);
	Slot.SetNumUninitialized(Values.Num());
	if (Values.Num() > 0)
	{
		FMemory::Memcpy(Slot.GetData(), Values.GetData(), Values.Num() * sizeof(float));
	}
}

const TArray<float>* FUERLPolicyCommands::Find(FName Channel) const
{
	return Channels.Find(Channel);
}

void FUERLPolicyCommands::Reset()
{
	Channels.Reset();
}

bool FUERLPolicyController::Initialize(
	UWorld& World,
	const FUERLPolicyControllerConfig& Config,
	FString& OutError)
{
	TArray<uint8> ArtifactBytes;
	if (Config.ArtifactPath.IsEmpty()
		|| !FFileHelper::LoadFileToArray(ArtifactBytes, *Config.ArtifactPath))
	{
		return ControllerFail(
			OutError,
			FString::Printf(TEXT("could not load policy artifact '%s'"), *Config.ArtifactPath));
	}
	return InitializeFromBytes(World, ArtifactBytes, Config, OutError);
}

bool FUERLPolicyController::InitializeFromBytes(
	UWorld& World,
	TConstArrayView<uint8> ArtifactBytes,
	const FUERLPolicyControllerConfig& Config,
	FString& OutError)
{
	// Full teardown before (re)building — soft Reset() only clears history buffers.
	bInitialized = false;
	Artifact.Reset();
	ObservationRuntime.Reset();
	ActionRuntime.Reset();
	Network.Reset();
	Robot.Reset();
	SelectedStateFields.Reset();
	RequiredCommandChannels.Reset();
	RawState.Reset();
	Observation.Reset();
	Action.Reset();
	Targets.Reset();
	PreviousAction.Reset();
	LastPreviousActionInput.Reset();
	LastStepCommands.Reset();
	LastTiming = FUERLControlTiming();
	OutError.Reset();

	if (ArtifactBytes.IsEmpty() || Config.AssetPath.IsEmpty())
	{
		return ControllerFail(OutError, TEXT("policy controller requires artifact bytes and AssetPath"));
	}
	if (!Config.GroundNormal.IsNormalized() || !FMath::IsFinite(Config.InitialRootHeightMeters))
	{
		return ControllerFail(OutError, TEXT("policy controller world placement is invalid"));
	}

	if (!Artifact.LoadFromBytes(ArtifactBytes, OutError))
	{
		return false;
	}
	if (!Network.Build(Artifact, OutError))
	{
		return false;
	}

	TMap<FName, int32> CommandWidths;
	if (!CollectRequiredCommands(Artifact.ObservationPlan(), RequiredCommandChannels, CommandWidths, OutError))
	{
		return false;
	}

	FUERLSkeletalMeshRobotRuntimeConfig RuntimeConfig;
	RuntimeConfig.AssetPath = Config.AssetPath;
	RuntimeConfig.ClaimedMesh = Config.ClaimedMesh;
	RuntimeConfig.PlacementTransform = Config.PlacementTransform;
	RuntimeConfig.bHasPlacementTransform = Config.bHasPlacementTransform;
	RuntimeConfig.GroundOrigin = Config.GroundOrigin;
	RuntimeConfig.GroundNormal = Config.GroundNormal;
	RuntimeConfig.TerrainQueryActors = Config.TerrainQueryActors;
	RuntimeConfig.GroundQueryIgnoreActor = Config.GroundQueryIgnoreActor;
	RuntimeConfig.InitialRootHeightMeters = Config.InitialRootHeightMeters;
	RuntimeConfig.bClaimAuthoredActor = Config.bClaimAuthoredActor;

	for (const FName FieldName : Artifact.ObservationPlan().StateRequirements)
	{
		FUERLSkeletalMeshRuntimeObservation ObservationSelector;
		if (!ParseObservationSelector(FieldName, ObservationSelector, OutError))
		{
			return false;
		}
		RuntimeConfig.Observations.Add(ObservationSelector);
	}
	if (RuntimeConfig.Observations.Num() == 0)
	{
		return ControllerFail(OutError, TEXT("observation plan state_requirements is empty"));
	}

	const FUERLPolicyRobotRuntime& RobotRuntimeSegment = Artifact.RobotRuntime();
	if (RobotRuntimeSegment.Actuators.Num() == 0)
	{
		return ControllerFail(OutError, TEXT("artifact robot_runtime.actuators is empty"));
	}
	RuntimeConfig.Actuators.Reserve(RobotRuntimeSegment.Actuators.Num());
	for (const FUERLPolicyRobotRuntimeActuator& Source : RobotRuntimeSegment.Actuators)
	{
		FUERLSkeletalMeshRuntimeActuator& Actuator = RuntimeConfig.Actuators.AddDefaulted_GetRef();
		Actuator.JointName = Source.JointName;
		Actuator.Stiffness = Source.Stiffness;
		Actuator.Damping = Source.Damping;
		Actuator.EffortLimit = Source.EffortLimit;
		Actuator.DefaultPosition = Source.DefaultPosition;
	}

	if (!Robot.Initialize(World, RuntimeConfig, OutError))
	{
		return false;
	}
	SelectedStateFields = Robot.SelectedStateFields();

	if (!ObservationRuntime.Compile(
		Artifact.ObservationPlan(),
		SelectedStateFields,
		CommandWidths,
		OutError))
	{
		Robot.Reset();
		return false;
	}

	TMap<FName, int32> ActionCommandWidths;
	// Action plan command_fields map onto the physical actuator vector (1 DOF each),
	// concatenated in declaration order. Total width must match actuator count.
	if (Artifact.ActionPlan().CommandFields.Num() == 1
		&& Robot.ActuatorCount() > 0)
	{
		ActionCommandWidths.Add(Artifact.ActionPlan().CommandFields[0], Robot.ActuatorCount());
	}
	else if (Artifact.ActionPlan().CommandFields.Num() == Robot.ActuatorCount())
	{
		for (const FName FieldName : Artifact.ActionPlan().CommandFields)
		{
			ActionCommandWidths.Add(FieldName, 1);
		}
	}
	if (!ActionRuntime.Compile(Artifact.ActionPlan(), ActionCommandWidths, OutError))
	{
		Robot.Reset();
		return false;
	}
	if (ObservationRuntime.OutputWidth() != Network.InputWidth())
	{
		Robot.Reset();
		return ControllerFail(
			OutError,
			FString::Printf(
				TEXT("observation plan width %d != network input width %d"),
				ObservationRuntime.OutputWidth(),
				Network.InputWidth()));
	}
	if (ActionRuntime.OutputWidth() != Robot.ActuatorCount())
	{
		Robot.Reset();
		return ControllerFail(
			OutError,
			FString::Printf(
				TEXT("action plan output width %d != actuator count %d"),
				ActionRuntime.OutputWidth(),
				Robot.ActuatorCount()));
	}
	if (Network.OutputWidth() != Artifact.ActionPlan().PolicyWidth)
	{
		Robot.Reset();
		return ControllerFail(
			OutError,
			FString::Printf(
				TEXT("network output width %d != action plan policy_width %d"),
				Network.OutputWidth(),
				Artifact.ActionPlan().PolicyWidth));
	}

	// Preallocate all control-step buffers once.
	RawState.SetNumZeroed(Robot.StateWidth());
	Observation.SetNumZeroed(ObservationRuntime.OutputWidth());
	Action.SetNumZeroed(Network.OutputWidth());
	Targets.SetNumZeroed(ActionRuntime.OutputWidth());
	// previous_action is raw policy output (ticket 07 / 25), not decoded targets.
	PreviousAction.SetNumZeroed(Network.OutputWidth());
	bInitialized = true;
	OutError.Reset();
	return true;
}

bool FUERLPolicyController::Step(
	const FUERLPolicyCommands& Commands,
	const FUERLControlTiming& Timing,
	FString& OutError,
	bool bCaptureDiagnostics)
{
	OutError.Reset();
	if (!bInitialized)
	{
		return ControllerFail(OutError, TEXT("policy controller is not initialized"));
	}
	if (!Timing.IsValid())
	{
		return ControllerFail(
			OutError,
			TEXT("policy control timing must contain positive finite observation and solver-step dt"));
	}

	// Step is called from the host's post-physics boundary.  Use the dt of the
	// solver result just completed; game/control elapsed time is not an impulse
	// denominator.
	Robot.SamplePhysicsContacts(Timing.LastSolverStepSeconds);
	if (!Robot.CollectState(RawState, OutError))
	{
		return false;
	}
	auto AssertFinite = [&OutError](const TCHAR* Label, TConstArrayView<float> Values) -> bool
	{
		for (int32 Index = 0; Index < Values.Num(); ++Index)
		{
			if (!FMath::IsFinite(Values[Index]))
			{
				return ControllerFail(
					OutError,
					FString::Printf(TEXT("%s[%d] is not finite"), Label, Index));
			}
		}
		return true;
	};
	if (!AssertFinite(TEXT("raw_state"), RawState))
	{
		return false;
	}

	FUERLPlanInputs ObsInputs;
	ObsInputs.RawState = RawState;
	ObsInputs.PreviousAction = PreviousAction;
	ObsInputs.ControlFrameDtSeconds = static_cast<float>(Timing.ObservationDtSeconds);
	TMap<FName, TArray<float>> StepCommands;
	for (const FUERLPolicyCommandChannel& Channel : RequiredCommandChannels)
	{
		const TArray<float>* Values = Commands.Find(Channel.Name);
		if (Values == nullptr)
		{
			return ControllerFail(
				OutError,
				FString::Printf(TEXT("required command channel '%s' is missing"), *Channel.Name.ToString()));
		}
		if (Values->Num() != Channel.Width)
		{
			return ControllerFail(
				OutError,
				FString::Printf(
					TEXT("command channel '%s' width %d does not match required width %d"),
					*Channel.Name.ToString(),
					Values->Num(),
					Channel.Width));
		}
		ObsInputs.Commands.Add(Channel.Name, *Values);
		if (bCaptureDiagnostics)
		{
			StepCommands.Add(Channel.Name, *Values);
		}
	}

	if (!ObservationRuntime.Execute(ObsInputs, Observation, OutError))
	{
		return false;
	}
	if (!Network.Evaluate(Observation, Action, OutError))
	{
		return false;
	}
	if (!AssertFinite(TEXT("observation"), Observation)
		|| !AssertFinite(TEXT("action"), Action))
	{
		return false;
	}

	FUERLPlanInputs ActionInputs;
	ActionInputs.PolicyAction = Action;
	ActionInputs.PreviousAction = PreviousAction;
	if (!ActionRuntime.Execute(ActionInputs, Targets, OutError))
	{
		return false;
	}
	if (!AssertFinite(TEXT("targets"), Targets))
	{
		return false;
	}
	if (!Robot.ApplyActuatorTargets(Targets, OutError))
	{
		return false;
	}

	if (bCaptureDiagnostics)
	{
		LastPreviousActionInput = PreviousAction;
		LastStepCommands = MoveTemp(StepCommands);
	}
	else
	{
		LastPreviousActionInput.Reset();
		LastStepCommands.Reset();
	}
	PreviousAction = Action;
	LastTiming = Timing;
	OutError.Reset();
	return true;
}

bool FUERLPolicyController::CollectState(TArray<float>& OutState, FString& OutError) const
{
	OutError.Reset();
	if (!bInitialized)
	{
		return ControllerFail(OutError, TEXT("policy controller is not initialized"));
	}
	return Robot.CollectState(OutState, OutError);
}

bool FUERLPolicyController::GetRobotTransform(FTransform& OutTransform, FString& OutError) const
{
	OutError.Reset();
	if (!bInitialized)
	{
		OutTransform = FTransform::Identity;
		return ControllerFail(OutError, TEXT("policy controller is not initialized"));
	}
	return Robot.GetPrimaryActorTransform(OutTransform, OutError);
}

void FUERLPolicyController::Reset()
{
	Robot.ClearObservationCaches();
	for (float& Value : PreviousAction)
	{
		Value = 0.0f;
	}
	for (float& Value : RawState)
	{
		Value = 0.0f;
	}
	for (float& Value : Observation)
	{
		Value = 0.0f;
	}
	for (float& Value : Action)
	{
		Value = 0.0f;
	}
	for (float& Value : Targets)
	{
		Value = 0.0f;
	}
	LastTiming = FUERLControlTiming();
}

void FUERLPolicyController::Shutdown()
{
	bInitialized = false;
	Robot.Reset();
	Artifact.Reset();
	ObservationRuntime.Reset();
	ActionRuntime.Reset();
	Network.Reset();
	SelectedStateFields.Reset();
	RequiredCommandChannels.Reset();
	RawState.Reset();
	Observation.Reset();
	Action.Reset();
	Targets.Reset();
	PreviousAction.Reset();
	LastPreviousActionInput.Reset();
	LastStepCommands.Reset();
	LastTiming = FUERLControlTiming();
}

bool FUERLPolicyController::ResetToReferencePose(FString& OutError)
{
	OutError.Reset();
	if (!bInitialized)
	{
		return ControllerFail(OutError, TEXT("policy controller is not initialized"));
	}
	if (!Robot.ResetToReferencePose(OutError))
	{
		return false;
	}
	Reset();
	return true;
}
