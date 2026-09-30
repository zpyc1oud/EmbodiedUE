#pragma once

#include "CoreMinimal.h"
#include "Templates/UniquePtr.h"
#include "UERLInterfaceTypes.h"
#include "UERLSlotCollisionPlan.h"

class UWorld;
class AActor;
class UPrimitiveComponent;
class FUERLNamedActionReader;
class FUERLNamedStateWriter;

/** Describe one reset parameter and the distributions accepted by a provider. */
struct FUERLResetParameterDescriptor
{
	/** Identify the stable reset parameter name. */
	FName Name;
	/** List the distribution kinds accepted for this parameter. */
	TArray<EUERLDistributionType> SupportedDistributions;
};

/** Describe the State fields and reset parameters published by an Environment. */
struct FUERLEnvironmentDescriptor
{
	/** Identify the registered Environment implementation. */
	FName Id;
	/** Identify the descriptor version used by the registry. */
	int32 Version = 1;
	/** Declare whether collision geometry belongs to each Slot or is shared by the World. */
	EUERLEnvironmentCollisionScope CollisionScope = EUERLEnvironmentCollisionScope::SlotIsolated;
	/** List State fields available from the Environment. */
	TArray<FUERLFieldDescriptor> StateFields;
	/** List reset parameters accepted by the Environment. */
	TArray<FUERLResetParameterDescriptor> ResetParameters;
};

/** Describe the Action/State fields and reset parameters published by a Robot. */
struct FUERLRobotDescriptor
{
	/** Identify the registered Robot implementation. */
	FName Id;
	/** Identify the descriptor version used by the registry. */
	int32 Version = 1;
	/** List physical command fields accepted by the Robot. */
	TArray<FUERLFieldDescriptor> ActionFields;
	/** List State fields available from the Robot. */
	TArray<FUERLFieldDescriptor> StateFields;
	/** List reset parameters accepted by the Robot. */
	TArray<FUERLResetParameterDescriptor> ResetParameters;
	/** Carry the reflected asset topology reported to Python at initialization. */
	FUERLRobotTopology Topology;
};

/** Identify the owner policy used by Robot terrain/clearance observations. */
enum class EUERLTerrainQueryPurpose : uint8
{
	/** Training keeps the Environment-owned terrain whitelist or Slot channel. */
	TrainingOwned = 0,
	/** Direct deployment accepts any blocking WorldStatic ground in the World. */
	DeploymentWorldStatic = 1,
};

/** Identify one stable Slot and its world-space placement origin. */
struct FUERLSlotContext
{
	/** Identify the Slot row used by all batch operations. */
	int32 SlotId = INDEX_NONE;
	/** Store the world-space origin assigned to the Slot. */
	FVector Origin = FVector::ZeroVector;
	/** Store the world-space terrain reset base height in centimetres. */
	double GroundHeight = 0.0;
	/** Store the sampled world-space ground normal. */
	FVector GroundNormal = FVector::UpVector;
	/** Identify all Environment-owned Actors whose collision belongs to this Slot. */
	TArray<TWeakObjectPtr<AActor>> EnvironmentActors;
	/** Identify World terrain owners permitted for SharedWorld terrain scans. */
	TArray<TWeakObjectPtr<AActor>> TerrainQueryActors;
	/** Distinguish the training owner filter from direct deployment WorldStatic queries. */
	EUERLTerrainQueryPurpose TerrainQueryPurpose = EUERLTerrainQueryPurpose::TrainingOwned;
	/** Store the active generated terrain patch half-extents in UE centimetres. */
	FVector2D TerrainHalfExtent = FVector2D::ZeroVector;
	/** Store the generated patch center relative to the Slot origin. */
	FVector TerrainBoundsOriginOffset = FVector::ZeroVector;
	/** Identify whether this Slot has finite generated-terrain bounds. */
	bool bTerrainBoundsValid = false;
	/** Store the immutable collision/query profile compiled for this Slot. */
	FUERLSlotCollisionProfile CollisionProfile;
	/** Identify ground primitives that receive Session-mediated friction updates. */
	TArray<TWeakObjectPtr<UPrimitiveComponent>> GroundComponents;
};

/** Carry indexed reset values and the target terrain placement for one Slot. */
struct FUERLResetRow
{
	/** Identify the Slot receiving these reset values. */
	int32 SlotId = INDEX_NONE;
	/** Store the terrain difficulty level selected for this reset. */
	uint16 TerrainLevel = 0;
	/** Store the target world-space Slot origin. */
	FVector Origin = FVector::ZeroVector;
	/** Store the target world-space terrain reset base height in centimetres. */
	double GroundHeight = 0.0;
	/** Store the target world-space ground normal. */
	FVector GroundNormal = FVector::UpVector;
	/** Store provider-specific reset values in negotiated binding order. */
	TArray<double> Values;
	/** Optional full ground frame for pose reset; legacy rows derive it from GroundNormal. */
	FQuat GroundRotation = FQuat::Identity;
	bool bHasGroundRotation = false;
};

/** Carry reset rows for the Slots selected by one reset operation. */
struct FUERLResetBatch
{
	/** Store one reset row for each selected Slot. */
	TArray<FUERLResetRow> Rows;

	/** Return the reset row for SlotId, or nullptr when it is absent. */
	UERLPROVIDER_API const FUERLResetRow* Find(int32 SlotId) const;
};

/** Identify a recoverable per-Slot physical fault reported in State. */
enum class EUERLSlotFaultCode : uint8
{
	None = 0,
	NonFiniteStagingState = 1,
	MissingObject = 2,
	ConstraintInvalid = 3,
	PhysicsStateInvalid = 4,
	/** The robot reached the edge of a finite generated terrain patch. */
	TerrainBoundary = 5,
};

class UERLPROVIDER_API IUERLEnvironment
{
public:
	virtual ~IUERLEnvironment() = default;
	/**
	 * Create the Environment-owned objects for every stable Slot.
	 *
	 * @param World Supply the World that owns the created objects.
	 * @param NumSlots Supply the requested number of Slot instances.
	 * @param OutSlots Receive stable Slot contexts for the Robot.
	 * @param OutError Receive a diagnostic when creation fails.
	 * @return true when all Slot objects were created.
	 */
	virtual bool CreateSlots(
		UWorld& World,
		int32 NumSlots,
		const FUERLSlotCollisionPlan& CollisionPlan,
		const FUERLTerrainConfig& TerrainConfig,
		TArray<FUERLSlotContext>& OutSlots,
		FString& OutError) = 0;
	/** Resolve the cached Ground frame for one Slot and terrain level. */
	virtual bool ResolveGroundFrame(
		int32 SlotId,
		uint16 TerrainLevel,
		FVector& OutOrigin,
		double& OutGroundHeight,
		FVector& OutGroundNormal,
		FString& OutError) const = 0;
	/** Apply indexed reset values to the selected Environment Slots. */
	virtual bool ResetSlots(const FUERLResetBatch& Reset, FString& OutError) = 0;
	/** Publish selected Environment State fields into the Worker writer. */
	virtual void CollectState(const TArray<int32>& Slots, FUERLNamedStateWriter& Writer) const = 0;
	/** Validate one Slot and return a recoverable fault code when possible. */
	virtual EUERLSlotFaultCode ValidateSlot(int32 SlotId, FString& OutReason) const = 0;
	/** Destroy all Environment-owned objects and release World resources. */
	virtual void DestroySlots() = 0;
};

class UERLPROVIDER_API IUERLRobot
{
public:
	virtual ~IUERLRobot() = default;
	/**
	 * Spawn Robot objects into the Environment-provided Slot contexts.
	 *
	 * @param World Supply the World that owns the spawned objects.
	 * @param Slots Supply the stable Slot placement contexts.
	 * @param OutError Receive a diagnostic when spawning fails.
	 * @return true when all Robot objects were spawned.
	 */
	virtual bool SpawnIntoSlots(UWorld& World, const TArray<FUERLSlotContext>& Slots, FString& OutError) = 0;
	/** Prepare provider-owned State collection from the selected schema before spawning. */
	virtual bool PrepareState(const TArray<FUERLFieldDescriptor>& SelectedStateFields, FString& OutError)
	{
		return true;
	}
	/** Apply the selected physical command fields to the Robot Slots. */
	virtual bool ApplyCommands(const FUERLNamedActionReader& Reader, FString& OutError) = 0;
	/** Return whether this Robot requires the same command to be applied before every physics frame. */
	virtual bool RequiresCommandsEveryPhysicsFrame() const { return true; }
	/** Sample terminal support and the latest completed solver-step force. */
	virtual void SamplePhysicsContacts(double SolverStepSeconds) { (void)SolverStepSeconds; }
	/** Apply one Session-mediated event to the selected Robot Slots. */
	virtual bool ApplyEvent(const FUERLEventBatch&, FString& OutError)
	{
		OutError = TEXT("Robot does not support Session-mediated events");
		return false;
	}
	/** Apply indexed reset values to the selected Robot Slots. */
	virtual bool ResetSlots(const FUERLResetBatch& Reset, FString& OutError) = 0;
	/** Read the owned Slot actor transform for direct-runtime pose resets. */
	virtual bool GetPrimaryActorTransform(FTransform& OutTransform, FString& OutError) const
	{
		OutTransform = FTransform::Identity;
		OutError = TEXT("Robot does not expose a primary actor transform");
		return false;
	}
	/** Clear contact samples owned by the Robot, including direct-runtime reset paths. */
	virtual void ClearContactState() {}
	/** Clear contact and cached terrain samples owned by the Robot. */
	virtual void ClearObservationCaches() { ClearContactState(); }
	/** Publish selected Robot State fields into the Worker writer. */
	virtual void CollectState(const TArray<int32>& Slots, FUERLNamedStateWriter& Writer) const = 0;
	/**
	 * Try to publish selected Robot State fields and return recoverable query errors.
	 * The default keeps existing providers source-compatible; Robot samplers that
	 * perform World queries override this seam instead of relying on checkf.
	 */
	virtual bool TryCollectState(
		const TArray<int32>& Slots,
		FUERLNamedStateWriter& Writer,
		FString& OutError) const
	{
		CollectState(Slots, Writer);
		OutError.Reset();
		return true;
	}
	/** Validate one Robot Slot and return a recoverable fault code when possible. */
	virtual EUERLSlotFaultCode ValidateSlot(int32 SlotId, FString& OutReason) const = 0;
	/** Destroy all Robot objects and release World resources. */
	virtual void DestroySlots() = 0;
};

/** Create and validate one Environment implementation from typed provider config. */
class UERLPROVIDER_API IUERLEnvironmentFactory
{
public:
	virtual ~IUERLEnvironmentFactory() = default;
	/** Return the stable Environment descriptor used during schema negotiation. */
	virtual const FUERLEnvironmentDescriptor& Describe() const = 0;
	/** Validate input config and return the effective typed Environment config. */
	virtual bool ValidateConfig(const FUERLProviderConfig& Input, FUERLProviderConfig& OutEffective, FString& OutError) const = 0;
	/** Create the Environment after its config has been validated. */
	virtual TUniquePtr<IUERLEnvironment> Create(const FUERLProviderConfig& EffectiveConfig) const = 0;
};

/** Create and validate one Robot implementation from typed provider config. */
class UERLPROVIDER_API IUERLRobotFactory
{
public:
	virtual ~IUERLRobotFactory() = default;
	/** Return the stable Robot descriptor used during schema negotiation. */
	virtual const FUERLRobotDescriptor& Describe() const = 0;
	/** Validate input config and return the effective typed Robot config. */
	virtual bool ValidateConfig(const FUERLProviderConfig& Input, FUERLProviderConfig& OutEffective, FString& OutError) const = 0;
	/** Create the Robot after its config has been validated. */
	virtual TUniquePtr<IUERLRobot> Create(const FUERLProviderConfig& EffectiveConfig) const = 0;
};
