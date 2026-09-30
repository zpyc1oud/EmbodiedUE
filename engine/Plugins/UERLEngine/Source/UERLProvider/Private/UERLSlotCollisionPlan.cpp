#include "UERLSlotCollisionPlan.h"

#include "Components/PrimitiveComponent.h"
#include "PhysicsEngine/BodyInstance.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"

bool FUERLSlotCollisionProfile::IsValid() const
{
	return CollisionScope == EUERLEnvironmentCollisionScope::SlotIsolated
		? ChannelIndex < FUERLSlotCollisionPlan::MaxSlots && CollisionGroupValue == 0
		: ChannelIndex == MAX_uint8 && CollisionGroupValue > 0;
}

EUERLEnvironmentCollisionScope FUERLSlotCollisionProfile::Scope() const
{
	checkf(IsValid(), TEXT("Slot collision profile is not compiled"));
	return CollisionScope;
}

ECollisionChannel FUERLSlotCollisionProfile::Channel() const
{
	checkf(IsValid() && CollisionScope == EUERLEnvironmentCollisionScope::SlotIsolated,
		TEXT("only an isolated Slot collision profile has a serialized channel"));
	return static_cast<ECollisionChannel>(ChannelIndex);
}

int32 FUERLSlotCollisionProfile::CollisionGroup() const
{
	checkf(IsValid() && CollisionScope == EUERLEnvironmentCollisionScope::SharedWorld,
		TEXT("only a shared-world Slot collision profile has a non-zero collision group"));
	return CollisionGroupValue;
}

void FUERLSlotCollisionProfile::ApplyRobot(UPrimitiveComponent& Component) const
{
	Component.SetCollisionResponseToAllChannels(ECR_Ignore);
	if (Scope() == EUERLEnvironmentCollisionScope::SlotIsolated)
	{
		const ECollisionChannel SlotChannel = Channel();
		Component.SetCollisionObjectType(SlotChannel);
		Component.SetCollisionResponseToChannel(SlotChannel, ECR_Block);
		return;
	}
	Component.SetCollisionObjectType(ECC_PhysicsBody);
	Component.SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
	Component.SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);
}

void FUERLSlotCollisionProfile::ApplySlotEnvironment(UPrimitiveComponent& Component) const
{
	const ECollisionChannel SlotChannel = Channel();
	Component.SetCollisionObjectType(SlotChannel);
	Component.SetCollisionResponseToAllChannels(ECR_Ignore);
	Component.SetCollisionResponseToChannel(SlotChannel, ECR_Block);
}

bool FUERLSlotCollisionProfile::ApplyRobotBody(FBodyInstance& Body) const
{
	checkf(IsValid(), TEXT("Slot collision profile is not compiled"));
	if (CollisionScope == EUERLEnvironmentCollisionScope::SlotIsolated)
	{
		return true;
	}
	FPhysicsActorHandle Handle = Body.GetPhysicsActorHandle();
	if (Handle == nullptr)
	{
		return false;
	}
	Chaos::FPBDRigidParticle* Particle = Handle->GetParticle_LowLevel()->CastToRigidParticle();
	if (Particle == nullptr)
	{
		return false;
	}
	Particle->SetCollisionGroup(CollisionGroupValue);
	return true;
}

bool FUERLSlotCollisionPlan::Compile(
	int32 NumSlots,
	EUERLEnvironmentCollisionScope Scope,
	FString& OutError)
{
	Reset();
	const int32 MaxForScope = Scope == EUERLEnvironmentCollisionScope::SlotIsolated
		? MaxSlots
		: MaxSharedWorldSlots;
	if (NumSlots <= 0 || NumSlots > MaxForScope)
	{
		OutError = FString::Printf(
			TEXT("%s Slot collision plan requires 1..%d Slots, received %d"),
			Scope == EUERLEnvironmentCollisionScope::SlotIsolated ? TEXT("isolated") : TEXT("shared-world"),
			MaxForScope,
			NumSlots);
		return false;
	}
	Profiles.Reserve(NumSlots);
	for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
	{
		Profiles.Add(Scope == EUERLEnvironmentCollisionScope::SlotIsolated
			? FUERLSlotCollisionProfile(Scope, static_cast<uint8>(SlotId), 0)
			: FUERLSlotCollisionProfile(Scope, MAX_uint8, SlotId + 1));
	}
	return true;
}

void FUERLSlotCollisionPlan::Reset()
{
	Profiles.Reset();
}

const FUERLSlotCollisionProfile& FUERLSlotCollisionPlan::Profile(int32 SlotId) const
{
	checkf(Profiles.IsValidIndex(SlotId), TEXT("Slot collision profile index is out of range"));
	return Profiles[SlotId];
}
