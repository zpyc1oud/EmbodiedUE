#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"

#include "Chaos/Defines.h"
#include "Chaos/PhysicalMaterials.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "PreviewScene.h"
#include "Dom/JsonObject.h"
#include "UERLEnvironmentPool.h"
#include "UERLIsolatedGridEnvironment.h"
#include "UERLRegistry.h"
#include "UERLSharedMapEnvironment.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

namespace
{
	struct FResetProbe
	{
		double Position = -1.0;
		FVector LastOrigin = FVector::ZeroVector;
		TArray<FUERLSlotContext> Slots;
		bool FailReset = false;
	};

	struct FSharedSlotProbe
	{
		TArray<FUERLSlotContext> Slots;
		int32 ResetRows = 0;
		mutable int32 CollectedRows = 0;
	};

	class FProbeEnvironment final : public IUERLEnvironment
	{
	public:
		bool CreateSlots(
			UWorld& World,
			int32 NumSlots,
			const FUERLSlotCollisionPlan& CollisionPlan,
			const FUERLTerrainConfig&,
			TArray<FUERLSlotContext>& OutSlots,
			FString&) override
		{
			for (int32 SlotId = 0; SlotId < NumSlots; ++SlotId)
			{
				AActor* Owner = World.SpawnActor<AActor>();
				UBoxComponent* Collision = NewObject<UBoxComponent>(Owner);
				Owner->SetRootComponent(Collision);
				Collision->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				Collision->RegisterComponent();
				Owners.Add(Owner);

				FUERLSlotContext& Slot = OutSlots.AddDefaulted_GetRef();
				Slot.SlotId = SlotId;
				Slot.CollisionProfile = CollisionPlan.Profile(SlotId);
				Slot.CollisionProfile.ApplySlotEnvironment(*Collision);
				Slot.EnvironmentActors.Add(Owner);
			}
			return true;
		}
		bool ResolveGroundFrame(
			int32,
			uint16,
			FVector&,
			double&,
			FVector&,
			FString& OutError) const override
		{
			OutError = TEXT("probe Environment has no terrain levels");
			return false;
		}

		bool ResetSlots(const FUERLResetBatch&, FString&) override { return true; }
		void CollectState(const TArray<int32>&, FUERLNamedStateWriter&) const override {}
		EUERLSlotFaultCode ValidateSlot(int32, FString&) const override { return EUERLSlotFaultCode::None; }
		void DestroySlots() override
		{
			for (const TWeakObjectPtr<AActor>& Owner : Owners)
			{
				if (Owner.IsValid())
				{
					Owner->Destroy();
				}
			}
			Owners.Reset();
		}

	private:
		TArray<TWeakObjectPtr<AActor>> Owners;
	};

	class FProbeEnvironmentFactory final : public IUERLEnvironmentFactory
	{
	public:
		const FUERLEnvironmentDescriptor& Describe() const override { return Descriptor; }
		bool ValidateConfig(const FUERLProviderConfig& Input, FUERLProviderConfig& Out, FString&) const override
		{
			Out = Input;
			return true;
		}
		TUniquePtr<IUERLEnvironment> Create(const FUERLProviderConfig&) const override
		{
			return MakeUnique<FProbeEnvironment>();
		}

	private:
		FUERLEnvironmentDescriptor Descriptor;
	};

	class FProbeRobot final : public IUERLRobot
	{
	public:
		explicit FProbeRobot(const TSharedRef<FResetProbe>& InProbe) : Probe(InProbe) {}

		bool SpawnIntoSlots(UWorld&, const TArray<FUERLSlotContext>& InSlots, FString&) override
		{
			Probe->Slots = InSlots;
			return true;
		}
		bool ApplyCommands(const FUERLNamedActionReader&, FString&) override { return true; }
		bool ResetSlots(const FUERLResetBatch& Reset, FString&) override
		{
			check(Reset.Rows.Num() == 1);
			if (Probe->FailReset)
			{
				return false;
			}
			Probe->LastOrigin = Reset.Rows[0].Origin;
			Probe->Position = Reset.Rows[0].Values.IsEmpty() ? 0.35 : Reset.Rows[0].Values[0];
			return true;
		}
		void CollectState(const TArray<int32>&, FUERLNamedStateWriter&) const override {}
		EUERLSlotFaultCode ValidateSlot(int32, FString&) const override { return EUERLSlotFaultCode::None; }
		void DestroySlots() override {}

	private:
		TSharedRef<FResetProbe> Probe;
	};

	class FProbeRobotFactory final : public IUERLRobotFactory
	{
	public:
		explicit FProbeRobotFactory(const TSharedRef<FResetProbe>& InProbe) : Probe(InProbe) {}

		const FUERLRobotDescriptor& Describe() const override { return Descriptor; }
		bool ValidateConfig(const FUERLProviderConfig& Input, FUERLProviderConfig& Out, FString&) const override
		{
			Out = Input;
			return true;
		}
		TUniquePtr<IUERLRobot> Create(const FUERLProviderConfig&) const override
		{
			return MakeUnique<FProbeRobot>(Probe);
		}

	private:
		TSharedRef<FResetProbe> Probe;
		FUERLRobotDescriptor Descriptor;
	};

	class FSharedProbeRobot final : public IUERLRobot
	{
	public:
		explicit FSharedProbeRobot(const TSharedRef<FSharedSlotProbe>& InProbe) : Probe(InProbe) {}

		bool SpawnIntoSlots(UWorld&, const TArray<FUERLSlotContext>& InSlots, FString&) override
		{
			Probe->Slots = InSlots;
			return true;
		}
		bool ApplyCommands(const FUERLNamedActionReader&, FString&) override { return true; }
		bool ResetSlots(const FUERLResetBatch& Reset, FString&) override
		{
			Probe->ResetRows = Reset.Rows.Num();
			return true;
		}
		void CollectState(const TArray<int32>& SlotIds, FUERLNamedStateWriter&) const override
		{
			Probe->CollectedRows = SlotIds.Num();
		}
		EUERLSlotFaultCode ValidateSlot(int32, FString&) const override { return EUERLSlotFaultCode::None; }
		void DestroySlots() override {}

	private:
		TSharedRef<FSharedSlotProbe> Probe;
	};

	class FSharedProbeRobotFactory final : public IUERLRobotFactory
	{
	public:
		explicit FSharedProbeRobotFactory(const TSharedRef<FSharedSlotProbe>& InProbe) : Probe(InProbe) {}

		const FUERLRobotDescriptor& Describe() const override { return Descriptor; }
		bool ValidateConfig(const FUERLProviderConfig& Input, FUERLProviderConfig& Out, FString&) const override
		{
			Out = Input;
			return true;
		}
		TUniquePtr<IUERLRobot> Create(const FUERLProviderConfig&) const override
		{
			return MakeUnique<FSharedProbeRobot>(Probe);
		}

	private:
		TSharedRef<FSharedSlotProbe> Probe;
		FUERLRobotDescriptor Descriptor;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLEnvironmentPoolCanonicalInitializeTest,
	"UERL.Unit.Worker.EnvironmentPool.AC_UE_UNIT_ENV_POOL_001.CanonicalInitialize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLEnvironmentPoolCanonicalInitializeTest::RunTest(const FString& Parameters)
{
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.SetTransactional(false)
			.SetEditor(false));
	TSharedRef<FResetProbe> Probe = MakeShared<FResetProbe>();
	TSharedRef<IUERLEnvironmentFactory> EnvironmentFactory = MakeShared<FProbeEnvironmentFactory>();
	TSharedRef<IUERLRobotFactory> RobotFactory = MakeShared<FProbeRobotFactory>(Probe);

	FUERLProviderConfig RobotConfig;
	FUERLResetBinding& Binding = RobotConfig.ResetBindings.AddDefaulted_GetRef();
	Binding.Index = 0;
	Binding.Name = TEXT("probe.position");
	Binding.TargetType = TEXT("joint_position");

	FUERLEnvironmentPool Pool;
	FUERLProviderConfig EmptyConfig;
	FUERLTerrainConfig EmptyTerrain;
	FString Error;
	TestTrue(TEXT("probe Environment Pool creates"), Pool.Create(
		*Scene->GetWorld(), 1,
		EnvironmentFactory, EmptyConfig,
		RobotFactory, RobotConfig,
		{}, EmptyTerrain, Error));
	TestTrue(TEXT("Initialize uses the Robot canonical state"), Pool.InitializeSlots({}, Error));
	TestTrue(TEXT("non-zero canonical position survives Initialize"), FMath::IsNearlyEqual(Probe->Position, 0.35));
	TestEqual(TEXT("Initialize does not advance the episode"), Pool.EpisodeIndices()[0], uint64(0));

	TestTrue(TEXT("episode Reset accepts an explicit zero override"), Pool.ResetSlots({ 0 }, {}, { 0.0f }, Error));
	TestTrue(TEXT("zero remains a legal Python-owned value"), FMath::IsNearlyZero(Probe->Position));
	TestEqual(TEXT("episode Reset advances the episode"), Pool.EpisodeIndices()[0], uint64(1));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ENV-POOL-001: Initialize preserves non-zero canonical state; Reset applies zero override"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLEnvironmentPoolResetTransactionTest,
	"UERL.Unit.Worker.EnvironmentPool.AC_UE_UNIT_ENV_POOL_002.ResetTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLEnvironmentPoolResetTransactionTest::RunTest(const FString& Parameters)
{
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.SetTransactional(false)
			.SetEditor(false));
	TSharedRef<FResetProbe> Probe = MakeShared<FResetProbe>();
	TSharedRef<IUERLEnvironmentFactory> EnvironmentFactory = MakeShared<FProbeEnvironmentFactory>();
	TSharedRef<IUERLRobotFactory> RobotFactory = MakeShared<FProbeRobotFactory>(Probe);

	FUERLProviderConfig RobotConfig;
	FUERLResetBinding& Binding = RobotConfig.ResetBindings.AddDefaulted_GetRef();
	Binding.Index = 0;
	Binding.Name = TEXT("probe.position");
	Binding.TargetType = TEXT("joint_position");

	FUERLEnvironmentPool Pool;
	FUERLProviderConfig EmptyConfig;
	FUERLTerrainConfig EmptyTerrain;
	FString Error;
	TestTrue(TEXT("probe Environment Pool creates"), Pool.Create(
		*Scene->GetWorld(), 1,
		EnvironmentFactory, EmptyConfig,
		RobotFactory, RobotConfig,
		{}, EmptyTerrain, Error));
	TestTrue(TEXT("Initialize establishes the canonical Robot state"), Pool.InitializeSlots({}, Error));

	Probe->FailReset = true;
	const double PositionBeforeFailedReset = Probe->Position;
	TestFalse(TEXT("a rejected Robot reset fails the transaction"), Pool.ResetSlots({ 0 }, {}, { 0.25f }, Error));
	TestEqual(TEXT("a rejected reset does not advance the episode"), Pool.EpisodeIndices()[0], uint64(0));
	TestTrue(TEXT("a rejected reset does not change Robot state"),
		FMath::IsNearlyEqual(Probe->Position, PositionBeforeFailedReset));
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ENV-POOL-002: rejected reset does not commit episode or Robot state"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLEnvironmentPoolTerrainAtlasResetTest,
	"UERL.Integration.Worker.EnvironmentPool.AC_UE_INTEGRATION_ENV_POOL_004.TerrainAtlasReset",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLEnvironmentPoolTerrainAtlasResetTest::RunTest(const FString& Parameters)
{
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.SetTransactional(false)
			.SetEditor(false));
	TSharedRef<IUERLEnvironmentFactory> EnvironmentFactory = UERLIsolatedGrid::MakeFactory();
	FUERLProviderConfig EnvironmentConfigInput;
	EnvironmentConfigInput.Scalars.Add(UERLIsolatedGrid::SpacingX, 5.0);
	EnvironmentConfigInput.Scalars.Add(UERLIsolatedGrid::SpacingY, 5.0);
	EnvironmentConfigInput.Scalars.Add(UERLIsolatedGrid::Columns, 1.0);
	FUERLProviderConfig EnvironmentConfig;
	FString Error;
	TestTrue(TEXT("isolated-grid terrain config validates"), EnvironmentFactory->ValidateConfig(
		EnvironmentConfigInput, EnvironmentConfig, Error));

	TSharedRef<FResetProbe> Probe = MakeShared<FResetProbe>();
	TSharedRef<IUERLRobotFactory> RobotFactory = MakeShared<FProbeRobotFactory>(Probe);
	FUERLProviderConfig RobotConfig;
	FUERLResetBinding& Binding = RobotConfig.ResetBindings.AddDefaulted_GetRef();
	Binding.Index = 0;
	Binding.Name = TEXT("probe.position");
	Binding.TargetType = TEXT("joint_position");

	FUERLTerrainConfig Terrain;
	Terrain.NumLevels = 2;
	Terrain.CellSize[0] = 4.0;
	Terrain.CellSize[1] = 4.0;
	for (int32 Level = 0; Level < Terrain.NumLevels; ++Level)
	{
		FUERLTerrainTierConfig& Tier = Terrain.Tiers.AddDefaulted_GetRef();
		Tier.Level = Level;
		Tier.Primitive = EUERLTerrainPrimitive::Plane;
		Tier.Params = MakeShared<FJsonObject>();
	}

	FUERLEnvironmentPool Pool;
	TestTrue(TEXT("pool creates one isolated procedural Slot"), Pool.Create(
		*Scene->GetWorld(), 1,
		EnvironmentFactory, EnvironmentConfig,
		RobotFactory, RobotConfig,
		{}, Terrain, Error));
	if (Probe->Slots.Num() != 1 || Probe->Slots[0].EnvironmentActors.Num() != 2)
	{
		AddError(Error);
		return false;
	}
	const TWeakObjectPtr<AActor> AtlasOwner = Probe->Slots[0].EnvironmentActors.Last();
	TestTrue(TEXT("procedural Slot exposes a live atlas owner"), AtlasOwner.IsValid());

	TestTrue(TEXT("initialization selects the first pre-generated level"), Pool.InitializeSlots({ 0 }, Error));
	const FVector Level0Origin = Probe->LastOrigin;
	TestTrue(TEXT("reset selects the second pre-generated level"),
		Pool.ResetSlots({ 0 }, { 1 }, { 0.25f }, Error));
	TestTrue(TEXT("terrain level reset changes only the selected origin"),
		FMath::IsNearlyEqual(Probe->LastOrigin.X - Level0Origin.X, 400.0)
			&& FMath::IsNearlyEqual(Probe->LastOrigin.Y, Level0Origin.Y)
			&& FMath::IsNearlyEqual(Probe->LastOrigin.Z, Level0Origin.Z));
	TestTrue(TEXT("terrain level reset keeps the static atlas owner"),
		AtlasOwner.IsValid() && Probe->Slots[0].EnvironmentActors.Last() == AtlasOwner);
	AddInfo(TEXT("[VERIFY] AC-UE-INTEGRATION-ENV-POOL-004: terrain reset selects a cached level without rebuilding the atlas"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLEnvironmentPoolSharedWorldTest,
	"UERL.Unit.Worker.EnvironmentPool.AC_UE_UNIT_ENV_POOL_005.SharedWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLEnvironmentPoolSharedWorldTest::RunTest(const FString& Parameters)
{
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld& World = *Scene->GetWorld();
	AActor* GroundOwner = World.SpawnActor<AActor>();
	UBoxComponent* Ground = NewObject<UBoxComponent>(GroundOwner);
	GroundOwner->SetRootComponent(Ground);
	Ground->SetBoxExtent(FVector(1000.0, 1000.0, 50.0));
	Ground->SetWorldLocation(FVector(0.0, 0.0, -50.0));
	Ground->SetCollisionObjectType(ECC_WorldStatic);
	Ground->SetCollisionResponseToAllChannels(ECR_Block);
	Ground->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Ground->RegisterComponent();

	TSharedRef<IUERLEnvironmentFactory> EnvironmentFactory = UERLSharedMap::MakeFactory();
	FUERLProviderConfig EnvironmentInput;
	EnvironmentInput.Scalars.Add(UERLSharedMap::SpacingX, 1.0);
	EnvironmentInput.Scalars.Add(UERLSharedMap::SpacingY, 1.0);
	EnvironmentInput.Scalars.Add(UERLSharedMap::Columns, 2.0);
	FUERLProviderConfig EnvironmentConfig;
	FString Error;
	if (!EnvironmentFactory->ValidateConfig(EnvironmentInput, EnvironmentConfig, Error))
	{
		AddError(Error);
		return false;
	}
	TSharedRef<FSharedSlotProbe> Probe = MakeShared<FSharedSlotProbe>();
	TSharedRef<IUERLRobotFactory> RobotFactory = MakeShared<FSharedProbeRobotFactory>(Probe);
	FUERLProviderConfig RobotConfig;
	FUERLTerrainConfig EmptyTerrain;
	FUERLEnvironmentPool Pool;
	TestTrue(TEXT("EnvironmentPool accepts a shared map with no per-Slot actor ownership"), Pool.Create(
		World, 2,
		EnvironmentFactory, EnvironmentConfig,
		RobotFactory, RobotConfig,
		{}, EmptyTerrain, Error));
	TestEqual(TEXT("Robot receives both shared-world Slot contexts"), Probe->Slots.Num(), 2);
	for (const FUERLSlotContext& Slot : Probe->Slots)
	{
		TestTrue(TEXT("shared-world Slot has no Environment-owned actors"), Slot.EnvironmentActors.IsEmpty());
		TestEqual(TEXT("shared-world Slot receives a SharedWorld profile"),
			Slot.CollisionProfile.Scope(), EUERLEnvironmentCollisionScope::SharedWorld);
	}
	TestTrue(TEXT("shared-world Slots initialize through the normal reset lifecycle"),
		Pool.InitializeSlots({}, Error));
	TestEqual(TEXT("Initialize resets every shared-world Robot row"), Probe->ResetRows, 2);
	TestTrue(TEXT("shared-world sparse reset uses the normal pool path"),
		Pool.ResetSlots({ 1 }, {}, {}, Error));
	TestEqual(TEXT("sparse reset reaches only the selected shared-world Robot row"), Probe->ResetRows, 1);
	FUERLBatchBinding EmptyBinding;
	float StateData[2] = {};
	FUERLNamedStateWriter Writer(EmptyBinding, { StateData, 2, 1 });
	TestTrue(TEXT("state collection succeeds"), Pool.CollectState({ 0, 1 }, Writer, Error));
	TestEqual(TEXT("state collection reaches every selected shared-world Robot row"),
		Probe->CollectedRows, 2);

	FUERLTerrainConfig ProceduralTerrain;
	ProceduralTerrain.NumLevels = 1;
	ProceduralTerrain.CellSize[0] = 4.0;
	ProceduralTerrain.CellSize[1] = 4.0;
	FUERLTerrainTierConfig& Plane = ProceduralTerrain.Tiers.AddDefaulted_GetRef();
	Plane.Level = 0;
	Plane.Primitive = EUERLTerrainPrimitive::Plane;
	Plane.Params = MakeShared<FJsonObject>();
	Ground->SetBoxExtent(FVector(25.0, 25.0, 50.0));
	Ground->SetWorldLocation(FVector(150.0, 150.0, -50.0));
	TestFalse(TEXT("shared-world procedural terrain rejects WorldStatic outside the spawn rays"), Pool.Create(
		World, 2,
		EnvironmentFactory, EnvironmentConfig,
		RobotFactory, RobotConfig,
		{}, ProceduralTerrain, Error));
	Ground->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Error.Reset();
	const bool bCreatedProcedural = Pool.Create(
		World, 2,
		EnvironmentFactory, EnvironmentConfig,
		RobotFactory, RobotConfig,
		{}, ProceduralTerrain, Error);
	TestTrue(TEXT("shared-world Environment accepts procedural terrain without overlapping collision"),
		bCreatedProcedural);
	if (!bCreatedProcedural)
	{
		AddError(Error);
	}
	TestTrue(TEXT("shared procedural terrain leaves the pool created"), Pool.IsCreated());
	AddInfo(TEXT("[VERIFY] AC-UE-UNIT-ENV-POOL-005: SharedWorld completes authored and procedural lifecycle"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLEnvironmentPoolSharedGroundFrictionTest,
	"UERL.Integration.Worker.EnvironmentPool.AC_UE_INTEGRATION_ENV_POOL_006.SharedGroundFrictionStaysPerSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLEnvironmentPoolSharedGroundFrictionTest::RunTest(const FString& Parameters)
{
	FModuleManager::Get().LoadModuleChecked(TEXT("UERLRobot"));
	TUniquePtr<FPreviewScene> Scene = MakeUnique<FPreviewScene>(
		FPreviewScene::ConstructionValues()
			.SetCreateDefaultLighting(false)
			.SetCreatePhysicsScene(true)
			.SetTransactional(false)
			.SetEditor(false));
	UWorld& World = *Scene->GetWorld();
	AActor* GroundOwner = World.SpawnActor<AActor>();
	UBoxComponent* Ground = NewObject<UBoxComponent>(GroundOwner);
	GroundOwner->SetRootComponent(Ground);
	Ground->SetBoxExtent(FVector(1000.0, 1000.0, 50.0));
	Ground->SetWorldLocation(FVector(0.0, 0.0, -50.0));
	Ground->SetCollisionObjectType(ECC_WorldStatic);
	Ground->SetCollisionResponseToAllChannels(ECR_Block);
	Ground->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	Ground->RegisterComponent();

	FString Error;
	TSharedRef<IUERLEnvironmentFactory> EnvironmentFactory = UERLSharedMap::MakeFactory();
	FUERLProviderConfig EnvironmentInput;
	EnvironmentInput.Scalars.Add(UERLSharedMap::SpacingX, 3.0);
	EnvironmentInput.Scalars.Add(UERLSharedMap::SpacingY, 3.0);
	EnvironmentInput.Scalars.Add(UERLSharedMap::Columns, 2.0);
	FUERLProviderConfig EnvironmentConfig;
	const TSharedPtr<IUERLRobotFactory> RobotFactory = FUERLRobotRegistry::Get().Resolve(
		FName(TEXT("uerl.robot.skeletal_mesh")), Error);
	FUERLProviderConfig RobotInput;
	RobotInput.AssetPath = TEXT("/Game/Robots/CartPole/SKM_CartPole.SKM_CartPole");
	FUERLProviderConfig RobotConfig;
	if (!EnvironmentFactory->ValidateConfig(EnvironmentInput, EnvironmentConfig, Error)
		|| !RobotFactory.IsValid()
		|| !RobotFactory->ValidateConfig(RobotInput, RobotConfig, Error))
	{
		AddError(Error);
		return false;
	}
	FUERLEnvironmentPool Pool;
	FUERLTerrainConfig EmptyTerrain;
	if (!Pool.Create(
			World, 2, EnvironmentFactory, EnvironmentConfig,
			RobotFactory.ToSharedRef(), RobotConfig, {}, EmptyTerrain, Error)
		|| !Pool.InitializeSlots({}, Error))
	{
		AddError(Error);
		return false;
	}

	// Static >= dynamic so Chaos' max(dynamic, static) static rule keeps the declared value.
	const FVector SlotFriction[] = { FVector(0.9, 0.6, 0.0), FVector(1.05, 0.8, 0.0) };
	FUERLEventBatch Event;
	Event.Kind = EUERLEventKind::GroundFriction;
	Event.SlotIds = { 0, 1 };
	Event.Values = { SlotFriction[0], SlotFriction[1] };
	TestTrue(TEXT("per-Slot ground friction applies on one shared ground"), Pool.ApplyEvent(Event, Error));

	TArray<USkeletalMeshComponent*> Robots;
	for (TActorIterator<AActor> It(&World); It; ++It)
	{
		if (USkeletalMeshComponent* Candidate = It->FindComponentByClass<USkeletalMeshComponent>())
		{
			Robots.Add(Candidate);
		}
	}
	// SharedMap places Slot 0 first along +X.
	Robots.Sort([](const USkeletalMeshComponent& Left, const USkeletalMeshComponent& Right)
	{
		return Left.GetComponentLocation().X < Right.GetComponentLocation().X;
	});
	TestEqual(TEXT("both Slot robots spawned on the shared ground"), Robots.Num(), 2);
	UPhysicalMaterial* GroundMaterial = Ground->GetBodyInstance()->GetSimplePhysicalMaterial();
	const Chaos::FChaosPhysicsMaterial* GroundChaos = GroundMaterial->GetPhysicsMaterial().Get();
	for (int32 SlotId = 0; SlotId < Robots.Num() && SlotId < 2; ++SlotId)
	{
		UPhysicalMaterial* RobotMaterial = Robots[SlotId]->GetPhysicsMaterialOverride();
		TestNotNull(TEXT("each Slot robot carries its own friction material"), RobotMaterial);
		if (!RobotMaterial || !GroundChaos)
		{
			continue;
		}
		const Chaos::FChaosPhysicsMaterial* RobotChaos = RobotMaterial->GetPhysicsMaterial().Get();
		// Same combination as FPBDCollisionConstraints::UpdateConstraintMaterialProperties.
		const Chaos::FChaosPhysicsMaterial::ECombineMode Mode = Chaos::FChaosPhysicsMaterial::ChooseCombineMode(
			RobotChaos->FrictionCombineMode, GroundChaos->FrictionCombineMode);
		const double Dynamic = Chaos::FChaosPhysicsMaterial::CombineHelper(
			RobotChaos->Friction, GroundChaos->Friction, Mode);
		const double Static = Chaos::FChaosPhysicsMaterial::CombineHelper(
			FMath::Max(RobotChaos->Friction, RobotChaos->StaticFriction),
			FMath::Max(GroundChaos->Friction, GroundChaos->StaticFriction), Mode);
		AddInfo(FString::Printf(TEXT("[VERIFY] Slot %d contact friction static=%.4f dynamic=%.4f"), SlotId, Static, Dynamic));
		TestTrue(TEXT("contact static friction equals the declared Slot value"),
			FMath::IsNearlyEqual(Static, SlotFriction[SlotId].X, 1.0e-5));
		TestTrue(TEXT("contact dynamic friction equals the declared Slot value"),
			FMath::IsNearlyEqual(Dynamic, SlotFriction[SlotId].Y, 1.0e-5));
	}
	Pool.Destroy();
	AddInfo(TEXT("[VERIFY] AC-UE-INTEGRATION-ENV-POOL-006: SharedWorld Slots keep distinct Chaos contact friction on one ground"));
	return true;
}

#endif
