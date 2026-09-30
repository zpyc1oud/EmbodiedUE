#include "UERLGenericRobotCommandApplier.h"

#include "UERLActuatorLaw.h"
#include "UERLGenericRobotKinematics.h"
#include "UERLGenericRobotObservationSampler.h"
#include "UERLRobotObservationPlan.h"

#include "Chaos/ChaosEngineInterface.h"
#include "Chaos/SimCallbackObject.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "PBDRigidsSolver.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsEngine/ConstraintInstance.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"

namespace
{
	/** Physics-thread copy of one effort actuator; handles stay owned by the component. */
	struct FSolverStepActuator
	{
		FPhysicsActorHandle Child = nullptr;
		FPhysicsActorHandle Parent = nullptr;
		bool bParentSimulated = false;
		bool bPrismatic = false;
		FTransform ChildRefFrame = FTransform::Identity;
		FTransform ParentRefFrame = FTransform::Identity;
		EUERLJointCoordinate Coordinate = EUERLJointCoordinate::None;
		UERLRobot::FUERLActuatorLawConfig Law;
		int32 TargetIndex = INDEX_NONE;
	};

	struct FSolverStepInput : public Chaos::FSimCallbackInput
	{
		TArray<FSolverStepActuator> Actuators;
		TArray<float> Targets;

		void Reset()
		{
			Actuators.Reset();
			Targets.Reset();
		}
	};

	bool IsDynamicBody(const Chaos::FRigidBodyHandle_Internal& Body)
	{
		const Chaos::EObjectStateType State = Body.ObjectState();
		return State == Chaos::EObjectStateType::Dynamic || State == Chaos::EObjectStateType::Sleeping;
	}

	/** Mirror ApplyGenericRobotActuatorForces for one effort actuator on the physics thread. */
	void ApplySolverStepActuator(const FSolverStepActuator& Actuator, float Target)
	{
		Chaos::FRigidBodyHandle_Internal* Child = Actuator.Child->GetPhysicsThreadAPI();
		Chaos::FRigidBodyHandle_Internal* Parent = Actuator.Parent->GetPhysicsThreadAPI();
		if (!Child || !Parent)
		{
			return;
		}
		const FTransform ChildFrame = ComposeGenericConstraintFrameWorld(
			Actuator.ChildRefFrame, FTransform(Child->R(), Child->X()));
		const FTransform ParentFrame = ComposeGenericConstraintFrameWorld(
			Actuator.ParentRefFrame, FTransform(Parent->R(), Parent->X()));
		const FVector AxisWorld = ChildFrame.TransformVectorNoScale(
			GenericJointCoordinateAxis(Actuator.Coordinate)).GetSafeNormal();
		if (AxisWorld.IsNearlyZero())
		{
			return;
		}
		const float Position = static_cast<float>(MeasureGenericJointPosition(
			ChildFrame, ParentFrame, Actuator.Coordinate));
		const float Velocity = Actuator.bPrismatic
			? static_cast<float>(FVector::DotProduct(
				FChaosEngineInterface::GetWorldVelocityAtPoint_AssumesLocked(Child, ChildFrame.GetLocation())
				- FChaosEngineInterface::GetWorldVelocityAtPoint_AssumesLocked(Parent, ParentFrame.GetLocation()),
				AxisWorld) / 100.0)
			: static_cast<float>(FVector::DotProduct(Child->W() - Parent->W(), AxisWorld));
		const double Effort = UERLRobot::ComputeUnifiedActuatorEffort(
			Actuator.Law, Target, Position, Velocity);
		const FVector Vector = AxisWorld * static_cast<float>(
			ConvertGenericJointEffortToChaos(Effort, Actuator.Coordinate));
		const bool bApplyParent = Actuator.bParentSimulated && IsDynamicBody(*Parent);
		if (Actuator.bPrismatic)
		{
			const FVector Anchor = (ChildFrame.GetLocation() + ParentFrame.GetLocation()) * 0.5;
			if (IsDynamicBody(*Child))
			{
				FChaosEngineInterface::AddForceAtPosition_AssumesLocked(
					Actuator.Child, Vector, Anchor, true, false, true);
			}
			if (bApplyParent)
			{
				FChaosEngineInterface::AddForceAtPosition_AssumesLocked(
					Actuator.Parent, -Vector, Anchor, true, false, true);
			}
		}
		else
		{
			if (IsDynamicBody(*Child))
			{
				FChaosEngineInterface::AddTorque_AssumesLocked(Actuator.Child, Vector, true, false, true);
			}
			if (bApplyParent)
			{
				FChaosEngineInterface::AddTorque_AssumesLocked(Actuator.Parent, -Vector, true, false, true);
			}
		}
	}

	class FSolverStepCallback final : public Chaos::TSimCallbackObject<FSolverStepInput>
	{
	public:
		virtual FName GetFNameForStatId() const override
		{
			const static FLazyName StaticName("UERLGenericRobotSolverStepCommands");
			return StaticName;
		}

	private:
		virtual void OnPreSimulate_Internal() override
		{
			// Synchronous substeps each run this once; the latest published
			// targets persist until the next publication, like a training Step.
			if (const FSolverStepInput* Input = GetConsumerInput_Internal())
			{
				Actuators = Input->Actuators;
				Targets = Input->Targets;
			}
			for (const FSolverStepActuator& Actuator : Actuators)
			{
				if (Targets.IsValidIndex(Actuator.TargetIndex))
				{
					ApplySolverStepActuator(Actuator, Targets[Actuator.TargetIndex]);
				}
			}
		}

		TArray<FSolverStepActuator> Actuators;
		TArray<float> Targets;
	};

	Chaos::FPhysicsSolver* WorldSolver(UWorld* World)
	{
		FPhysScene* Scene = World ? World->GetPhysicsScene() : nullptr;
		return Scene ? Scene->GetSolver() : nullptr;
	}
}

bool IsGenericRobotDriveActuator(const FUERLActuatorConfig& Actuator)
{
	return Actuator.TargetMode == TEXT("position") && Actuator.CoordinateType == TEXT("revolute");
}

void ConfigureGenericRobotRevoluteAngularDrive(
	FConstraintInstance& Constraint,
	const FUERLActuatorConfig& Actuator)
{
	// TwistAndSwing drives only the unlocked swing axis, so one swing flag
	// serves both swing1 (Z) and swing2 (Y) coordinates.
	const bool bTwist = Actuator.Coordinate == UERLJointCoordinateName(EUERLJointCoordinate::Twist);
	Constraint.SetAngularDriveMode(EAngularDriveMode::TwistAndSwing);
	Constraint.SetOrientationDriveTwistAndSwing(bTwist, !bTwist);
	Constraint.SetAngularVelocityDriveTwistAndSwing(bTwist, !bTwist);
	Constraint.SetAngularDriveParams(
		static_cast<float>(Actuator.Stiffness * 10000.0),
		static_cast<float>(Actuator.Damping * 10000.0),
		static_cast<float>(Actuator.EffortLimit * 10000.0));
	Constraint.SetAngularDriveAccelerationMode(false);
}

void ConfigureGenericRobotRevoluteAngularDrives(
	USkeletalMeshComponent& Component,
	const TArray<int32>& ConstraintIndices,
	TConstArrayView<FUERLActuatorConfig> Actuators)
{
	for (const FUERLActuatorConfig& Actuator : Actuators)
	{
		if (!IsGenericRobotDriveActuator(Actuator))
		{
			continue;
		}
		checkf(
			ConstraintIndices.IsValidIndex(Actuator.JointIndex),
			TEXT("generic Robot revolute drive has invalid joint index %d"), Actuator.JointIndex);
		FConstraintInstance* Constraint = Component.GetConstraintInstanceByIndex(
			ConstraintIndices[Actuator.JointIndex]);
		checkf(Constraint, TEXT("generic Robot revolute drive has no runtime constraint"));
		ConfigureGenericRobotRevoluteAngularDrive(*Constraint, Actuator);
	}
}

bool ApplyGenericRobotActuatorForces(
	const FUERLRobotCommandSlotView& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FUERLActuatorConfig> Actuators,
	FString& OutError)
{
	if (!Slot.Component || !Slot.ConstraintIndices || !Slot.Targets
		|| Slot.Targets->Num() < Actuators.Num())
	{
		OutError = TEXT("generic Robot actuator target cache is not initialized");
		return false;
	}

	FUERLRobotObservationSlotView ObservationSlot;
	ObservationSlot.Component = Slot.Component;
	ObservationSlot.ConstraintIndices = Slot.ConstraintIndices;

	for (const FUERLActuatorConfig& Actuator : Actuators)
	{
		if (!Topology.Joints.IsValidIndex(Actuator.JointIndex)
			|| !Slot.ConstraintIndices->IsValidIndex(Actuator.JointIndex)
			|| !Slot.Targets->IsValidIndex(Actuator.Index))
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator %d has an invalid cached joint index"), Actuator.Index);
			return false;
		}
		const FUERLJointTopology& Joint = Topology.Joints[Actuator.JointIndex];
		if (!Topology.BodyNames.IsValidIndex(Joint.ChildBodyIndex)
			|| !Topology.BodyNames.IsValidIndex(Joint.ParentBodyIndex))
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' has invalid joint bodies"),
				*Actuator.JointName.ToString());
			return false;
		}
		FConstraintInstance* Constraint = Slot.Component->GetConstraintInstanceByIndex(
			(*Slot.ConstraintIndices)[Actuator.JointIndex]);
		if (!Constraint)
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' has no runtime constraint"),
				*Actuator.JointName.ToString());
			return false;
		}
		const FName ChildName = Topology.BodyNames[Joint.ChildBodyIndex];
		const FName ParentName = Topology.BodyNames[Joint.ParentBodyIndex];
		FBodyInstance* ChildInstance = Slot.Component->GetBodyInstance(ChildName);
		FBodyInstance* ParentInstance = Slot.Component->GetBodyInstance(ParentName);
		if (!ChildInstance || !ParentInstance)
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' has no runtime body instance"),
				*Actuator.JointName.ToString());
			return false;
		}
		const FTransform ChildFrame = ComposeGenericConstraintFrameWorld(
			Constraint->GetRefFrame(EConstraintFrame::Frame1),
			ChildInstance->GetUnrealWorldTransform());
		const FTransform ParentFrame = ComposeGenericConstraintFrameWorld(
			Constraint->GetRefFrame(EConstraintFrame::Frame2),
			ParentInstance->GetUnrealWorldTransform());
		const FVector AxisWorld = ChildFrame.TransformVectorNoScale(
			GenericJointCoordinateAxis(Joint.Coordinate)).GetSafeNormal();
		if (AxisWorld.IsNearlyZero())
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' has a zero constraint axis"),
				*Actuator.JointName.ToString());
			return false;
		}

		FUERLRobotObservationPlanEntry Entry;
		Entry.BodyIndex = Joint.ChildBodyIndex;
		Entry.JointIndex = Actuator.JointIndex;
		float Position = 0.0f;
		float Velocity = 0.0f;
		if (!ReadGenericRobotJointScalar(ObservationSlot, Topology, Entry, false, Position)
			|| !ReadGenericRobotJointScalar(ObservationSlot, Topology, Entry, true, Velocity))
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' could not read joint state"),
				*Actuator.JointName.ToString());
			return false;
		}
		const UERLRobot::FUERLActuatorLawConfig Law = {
			Actuator.Stiffness, Actuator.Damping, Actuator.EffortLimit };
		const double Effort = UERLRobot::ComputeUnifiedActuatorEffort(
			Law, (*Slot.Targets)[Actuator.Index], Position, Velocity);
		const FVector Anchor = (ChildFrame.GetLocation() + ParentFrame.GetLocation()) * 0.5;
		const bool bParentSimulated = Topology.BodyMotionTypes.IsValidIndex(Joint.ParentBodyIndex)
			&& Topology.BodyMotionTypes[Joint.ParentBodyIndex] == EUERLBodyMotionType::Simulated;
		if (Actuator.CoordinateType == TEXT("prismatic"))
		{
			const FVector Force = AxisWorld * static_cast<float>(
				ConvertGenericJointEffortToChaos(Effort, Joint.Coordinate));
			Slot.Component->AddForceAtLocation(Force, Anchor, ChildName);
			if (bParentSimulated)
			{
				Slot.Component->AddForceAtLocation(-Force, Anchor, ParentName);
			}
		}
		else if (Actuator.CoordinateType == TEXT("revolute"))
		{
			if (Actuator.TargetMode == TEXT("position"))
			{
				Constraint->SetAngularOrientationTarget(FQuat(
					GenericJointCoordinateAxis(Joint.Coordinate),
					(*Slot.Targets)[Actuator.Index]));
			}
			else
			{
				// Apply SI torque along the constraint Frame1 world axis.
				const FVector Torque = AxisWorld * static_cast<float>(
					ConvertGenericJointEffortToChaos(Effort, Joint.Coordinate));
				Slot.Component->AddTorqueInRadians(Torque, ChildName, false);
				if (bParentSimulated)
				{
					Slot.Component->AddTorqueInRadians(-Torque, ParentName, false);
				}
			}
		}
		else
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' has an unknown coordinate type"),
				*Actuator.JointName.ToString());
			return false;
		}
	}
	return true;
}

class FUERLGenericRobotSolverStepCommands::FImpl
{
public:
	TWeakObjectPtr<UWorld> World;
	FSolverStepCallback* Callback = nullptr;
	TArray<FSolverStepActuator> Actuators;
};

FUERLGenericRobotSolverStepCommands::FUERLGenericRobotSolverStepCommands()
	: Impl(MakeUnique<FImpl>())
{
}

FUERLGenericRobotSolverStepCommands::~FUERLGenericRobotSolverStepCommands()
{
	Unregister();
}

bool FUERLGenericRobotSolverStepCommands::Register(
	UWorld& World,
	const FUERLRobotCommandSlotView& Slot,
	const FUERLRobotTopology& Topology,
	TConstArrayView<FUERLActuatorConfig> Actuators,
	FString& OutError)
{
	Unregister();
	if (!Slot.Component || !Slot.ConstraintIndices)
	{
		OutError = TEXT("generic Robot solver-step commands have no Slot component");
		return false;
	}
	TArray<FSolverStepActuator> SolverActuators;
	for (const FUERLActuatorConfig& Actuator : Actuators)
	{
		if (IsGenericRobotDriveActuator(Actuator))
		{
			continue;
		}
		const FUERLJointTopology* Joint = Topology.Joints.IsValidIndex(Actuator.JointIndex)
			? &Topology.Joints[Actuator.JointIndex] : nullptr;
		FConstraintInstance* Constraint = Joint && Slot.ConstraintIndices->IsValidIndex(Actuator.JointIndex)
			? Slot.Component->GetConstraintInstanceByIndex((*Slot.ConstraintIndices)[Actuator.JointIndex])
			: nullptr;
		FBodyInstance* ChildInstance = Joint && Topology.BodyNames.IsValidIndex(Joint->ChildBodyIndex)
			? Slot.Component->GetBodyInstance(Topology.BodyNames[Joint->ChildBodyIndex]) : nullptr;
		FBodyInstance* ParentInstance = Joint && Topology.BodyNames.IsValidIndex(Joint->ParentBodyIndex)
			? Slot.Component->GetBodyInstance(Topology.BodyNames[Joint->ParentBodyIndex]) : nullptr;
		if (!Constraint || !ChildInstance || !ParentInstance
			|| !ChildInstance->GetPhysicsActorHandle() || !ParentInstance->GetPhysicsActorHandle())
		{
			OutError = FString::Printf(
				TEXT("generic Robot actuator '%s' has no runtime constraint or physics bodies"),
				*Actuator.JointName.ToString());
			return false;
		}
		FSolverStepActuator& SolverActuator = SolverActuators.AddDefaulted_GetRef();
		SolverActuator.Child = ChildInstance->GetPhysicsActorHandle();
		SolverActuator.Parent = ParentInstance->GetPhysicsActorHandle();
		SolverActuator.bParentSimulated = Topology.BodyMotionTypes.IsValidIndex(Joint->ParentBodyIndex)
			&& Topology.BodyMotionTypes[Joint->ParentBodyIndex] == EUERLBodyMotionType::Simulated;
		SolverActuator.bPrismatic = Actuator.CoordinateType == TEXT("prismatic");
		SolverActuator.ChildRefFrame = Constraint->GetRefFrame(EConstraintFrame::Frame1);
		SolverActuator.ParentRefFrame = Constraint->GetRefFrame(EConstraintFrame::Frame2);
		SolverActuator.Coordinate = Joint->Coordinate;
		SolverActuator.Law = { Actuator.Stiffness, Actuator.Damping, Actuator.EffortLimit };
		SolverActuator.TargetIndex = Actuator.Index;
	}
	if (SolverActuators.IsEmpty())
	{
		return true;
	}
	Chaos::FPhysicsSolver* Solver = WorldSolver(&World);
	if (!Solver)
	{
		OutError = TEXT("generic Robot solver-step commands require a World Chaos solver");
		return false;
	}
	Impl->World = &World;
	Impl->Actuators = MoveTemp(SolverActuators);
	Impl->Callback = Solver->CreateAndRegisterSimCallbackObject_External<FSolverStepCallback>();
	if (Slot.Targets)
	{
		PublishTargets(*Slot.Targets);
	}
	return true;
}

void FUERLGenericRobotSolverStepCommands::PublishTargets(TConstArrayView<float> Targets)
{
	if (!Impl->Callback)
	{
		return;
	}
	FSolverStepInput* Input = Impl->Callback->GetProducerInputData_External();
	Input->Actuators = Impl->Actuators;
	Input->Targets.Reset(Targets.Num());
	Input->Targets.Append(Targets.GetData(), Targets.Num());
}

void FUERLGenericRobotSolverStepCommands::Unregister()
{
	if (Impl->Callback)
	{
		if (Chaos::FPhysicsSolver* Solver = WorldSolver(Impl->World.Get()))
		{
			Solver->UnregisterAndFreeSimCallbackObject_External(Impl->Callback);
		}
	}
	Impl->Callback = nullptr;
	Impl->World.Reset();
	Impl->Actuators.Reset();
}
