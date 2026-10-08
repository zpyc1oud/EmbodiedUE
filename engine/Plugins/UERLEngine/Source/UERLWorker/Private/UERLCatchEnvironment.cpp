#include "UERLCatchEnvironment.h"

#include "UERLBatchBinding.h"
#include "UERLTargetCaptureComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Kismet/GameplayStatics.h"
#include "PhysicsEngine/BodyInstance.h"

namespace UERLCatch
{
	const FName EnvironmentId(TEXT("uerl.environment.catch"));
	const FName TargetPositionField(TEXT("environment.pursuit_target_position"));
	const FName TargetCaptureField(TEXT("environment.target_capture"));
}

namespace
{
	const FName TargetX(TEXT("environment.target_x_m"));
	const FName TargetY(TEXT("environment.target_y_m"));
	const FName TargetSpeed(TEXT("environment.target_speed_mps"));
	const FName TargetRadius(TEXT("environment.target_path_radius_m"));
	const FName PlayerTarget(TEXT("environment.use_player_target"));

	class FCatchEnvironment final : public IUERLEnvironment
	{
	public:
		explicit FCatchEnvironment(const FUERLProviderConfig& Config)
			: StartLocation(Config.Scalars[TargetX] * 100.0, Config.Scalars[TargetY] * 100.0, 85.0)
			, Speed(Config.Scalars[TargetSpeed]), Radius(Config.Scalars[TargetRadius])
			, bPlayerTarget(Config.Scalars[PlayerTarget] == 1.0) {}

		bool CreateSlots(UWorld& World, int32 NumSlots, const FUERLSlotCollisionPlan& CollisionPlan,
			const FUERLTerrainConfig& TerrainConfig, TArray<FUERLSlotContext>& OutSlots, FString& OutError) override
		{
			DestroySlots();
			OutSlots.Reset();
			if (NumSlots != 1 || TerrainConfig.NumLevels != 0)
			{
				OutError = TEXT("Catch requires one Slot and its arena instead of procedural terrain tiers");
				return false;
			}
			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			UStaticMesh* Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
			if (!Cube || !Sphere)
			{
				OutError = TEXT("Catch arena requires the installed Engine basic shapes");
				return false;
			}
			AActor* Floor = World.SpawnActor<AActor>();
			if (!Floor) { OutError = TEXT("failed to create Catch floor"); return false; }
			OwnedActors.Add(Floor);
			UStaticMeshComponent* FloorMesh = NewObject<UStaticMeshComponent>(Floor);
			Floor->SetRootComponent(FloorMesh);
			Floor->AddInstanceComponent(FloorMesh);
			FloorMesh->SetStaticMesh(Cube);
			FloorMesh->SetMobility(EComponentMobility::Static);
			FloorMesh->SetWorldLocation(FVector(0.0, 0.0, -5.0));
			FloorMesh->SetWorldScale3D(FVector(40.0, 40.0, 0.1));
			FloorMesh->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
			FloorMesh->SetCollisionObjectType(ECC_WorldStatic);
			FloorMesh->SetCollisionResponseToAllChannels(ECR_Block);
			FloorMesh->RegisterComponent();

			AActor* Target = bPlayerTarget ? UGameplayStatics::GetPlayerPawn(&World, 0) : World.SpawnActor<AActor>();
			if (!Target)
			{
				OutError = TEXT("Catch target is unavailable; player mode requires a possessed Pawn");
				DestroySlots();
				return false;
			}
			TargetActor = Target;
			OriginalTargetTransform = Target->GetActorTransform();
			UPrimitiveComponent* TargetBody = nullptr;
			if (bPlayerTarget)
			{
				TargetBody = Cast<UPrimitiveComponent>(Target->GetRootComponent());
			}
			else
			{
				OwnedActors.Add(Target);
				UCapsuleComponent* Capsule = NewObject<UCapsuleComponent>(Target);
				Capsule->SetMobility(EComponentMobility::Movable);
				Capsule->SetCapsuleSize(25.0f, 85.0f);
				Target->SetRootComponent(Capsule);
				Target->AddInstanceComponent(Capsule);
				Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
				Capsule->SetCollisionResponseToAllChannels(ECR_Block);
				// The flat-arena proxy has a prescribed height. Exclude its supporting
				// floor from horizontal sweeps, which can start in floor contact.
				// Robot and obstacle sweeps still block and produce capture events.
				Capsule->IgnoreActorWhenMoving(Floor, true);
				Capsule->RegisterComponent();
				TargetBody = Capsule;
				// Project-owned primitive human proxy: head, torso and four limbs.
				AddVisual(*Target, *Capsule, *Sphere, FVector(0, 0, 63), FVector(0.32));
				AddVisual(*Target, *Capsule, *Cube, FVector(0, 0, 22), FVector(0.28, 0.40, 0.55));
				AddVisual(*Target, *Capsule, *Cube, FVector(0, -12, -40), FVector(0.13, 0.13, 0.75));
				AddVisual(*Target, *Capsule, *Cube, FVector(0, 12, -40), FVector(0.13, 0.13, 0.75));
				AddVisual(*Target, *Capsule, *Cube, FVector(0, -28, 18), FVector(0.12, 0.12, 0.60));
				AddVisual(*Target, *Capsule, *Cube, FVector(0, 28, 18), FVector(0.12, 0.12, 0.60));
			}
			if (!TargetBody || !TargetBody->IsCollisionEnabled())
			{
				OutError = TEXT("Catch target must have a collidable primitive root");
				DestroySlots();
				return false;
			}
			TargetComponent = TargetBody;
			OriginalObjectType = TargetBody->GetCollisionObjectType();
			OriginalResponses = TargetBody->GetCollisionResponseToChannels();
			OriginalNotify = TargetBody->BodyInstance.bNotifyRigidBodyCollision;
			TargetBody->SetCollisionObjectType(ECC_PhysicsBody);
			TargetBody->SetCollisionResponseToChannel(ECC_PhysicsBody, ECR_Block);
			TargetBody->SetNotifyRigidBodyCollision(true);
			Target->SetActorLocation(StartLocation, false, nullptr, ETeleportType::TeleportPhysics);
			UUERLTargetCaptureComponent* Listener = NewObject<UUERLTargetCaptureComponent>(Target);
			Target->AddInstanceComponent(Listener);
			Listener->RegisterComponent();
			Capture = Listener;
			if (ADirectionalLight* Light = World.SpawnActor<ADirectionalLight>(
				ADirectionalLight::StaticClass(), FVector::ZeroVector, FRotator(-45.0, -30.0, 0.0)))
			{
				OwnedActors.Add(Light);
			}
			FUERLSlotContext& Slot = OutSlots.AddDefaulted_GetRef();
			Slot.SlotId = 0;
			Slot.Origin = FVector::ZeroVector;
			Slot.GroundHeight = 0.0;
			Slot.GroundNormal = FVector::UpVector;
			Slot.CollisionProfile = CollisionPlan.Profile(0);
			Slot.EnvironmentActors.Add(Floor);
			Slot.EnvironmentActors.Add(Target);
			Slot.TerrainQueryActors.Add(Floor);
			Slot.GroundComponents.Add(FloorMesh);
			return true;
		}

		bool BindRobot(IUERLRobot& Robot, FString& OutError) override
		{
			AActor* Actor = Robot.GetSlotActor(0);
			if (!Actor || !Capture.IsValid() || !TargetComponent.IsValid())
			{
				OutError = TEXT("Catch cannot bind its controlled Robot and target");
				return false;
			}
			Capture->Bind(*TargetComponent.Get(), *Actor);
			return true;
		}
		void BeginControlWindow() override { if (Capture.IsValid()) { Capture->BeginControlWindow(); } }
		void EndControlWindow() override { if (Capture.IsValid()) { Capture->EndControlWindow(); } }
		void AdvancePhysicsFrame(double PhysicsDt) override
		{
			if (bPlayerTarget || !TargetActor.IsValid() || !Capture.IsValid() || Capture->HasCaptured()) { return; }
			Phase += PhysicsDt * Speed / Radius;
			const FVector Position = StartLocation + FVector(
				Radius * 100.0 * (FMath::Cos(Phase) - 1.0), Radius * 100.0 * FMath::Sin(Phase), 0.0);
			TargetActor->SetActorLocation(Position, true);
		}
		bool ResolveGroundFrame(int32 SlotId, uint16 TerrainLevel, FVector& OutOrigin,
			double& OutGroundHeight, FVector& OutGroundNormal, FString& OutError) const override
		{
			if (SlotId != 0 || TerrainLevel != 0) { OutError = TEXT("Catch ground Slot or tier is invalid"); return false; }
			OutOrigin = FVector::ZeroVector;
			OutGroundHeight = 0.0;
			OutGroundNormal = FVector::UpVector;
			return true;
		}
		bool ResetSlots(const FUERLResetBatch& Reset, FString& OutError) override
		{
			if (!TargetActor.IsValid() || !Capture.IsValid()) { OutError = TEXT("Catch reset target is missing"); return false; }
			for (const FUERLResetRow& Row : Reset.Rows)
			{
				if (Row.SlotId != 0) { OutError = TEXT("Catch reset Slot is invalid"); return false; }
				Capture->ResetCapture();
				Phase = 0.0;
				TargetActor->SetActorLocation(StartLocation, false, nullptr, ETeleportType::TeleportPhysics);
			}
			return true;
		}
		void CollectState(const TArray<int32>& Slots, FUERLNamedStateWriter& Writer) const override
		{
			const FVector Position = TargetActor.IsValid() ? TargetActor->GetActorLocation() / 100.0 : FVector::ZeroVector;
			const float Values[] = { static_cast<float>(Position.X), static_cast<float>(Position.Y), static_cast<float>(Position.Z) };
			const float Captured[] = { Capture.IsValid() && Capture->HasCaptured() ? 1.0f : 0.0f };
			for (int32 Slot : Slots)
			{
				Writer.WriteVector(Slot, UERLCatch::TargetPositionField, MakeArrayView(Values));
				Writer.WriteVector(Slot, UERLCatch::TargetCaptureField, MakeArrayView(Captured));
			}
		}
		EUERLSlotFaultCode ValidateSlot(int32 SlotId, FString& OutReason) const override
		{
			if (SlotId != 0 || !TargetActor.IsValid() || !Capture.IsValid() || !Capture->HasValidBinding())
			{
				OutReason = TEXT("Catch target or controlled Robot is missing");
				return EUERLSlotFaultCode::MissingObject;
			}
			return EUERLSlotFaultCode::None;
		}
		void DestroySlots() override
		{
			if (Capture.IsValid()) { Capture->Unbind(); Capture->DestroyComponent(); }
			Capture.Reset();
			if (bPlayerTarget && TargetComponent.IsValid())
			{
				TargetComponent->SetCollisionObjectType(OriginalObjectType);
				TargetComponent->SetCollisionResponseToChannels(OriginalResponses);
				TargetComponent->SetNotifyRigidBodyCollision(OriginalNotify);
				if (TargetActor.IsValid()) { TargetActor->SetActorTransform(OriginalTargetTransform); }
			}
			for (const TWeakObjectPtr<AActor>& Actor : OwnedActors) { if (Actor.IsValid()) { Actor->Destroy(); } }
			OwnedActors.Reset();
			TargetActor.Reset();
			TargetComponent.Reset();
			Phase = 0.0;
		}

	private:
		static void AddVisual(AActor& Owner, USceneComponent& Root, UStaticMesh& Mesh,
			const FVector& Position, const FVector& Scale)
		{
			UStaticMeshComponent* Part = NewObject<UStaticMeshComponent>(&Owner);
			Owner.AddInstanceComponent(Part);
			Part->SetupAttachment(&Root);
			Part->SetStaticMesh(&Mesh);
			Part->SetMobility(EComponentMobility::Movable);
			Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Part->SetRelativeLocation(Position);
			Part->SetRelativeScale3D(Scale);
			Part->RegisterComponent();
		}
		FVector StartLocation;
		double Speed;
		double Radius;
		bool bPlayerTarget;
		double Phase = 0.0;
		TArray<TWeakObjectPtr<AActor>> OwnedActors;
		TWeakObjectPtr<AActor> TargetActor;
		TWeakObjectPtr<UPrimitiveComponent> TargetComponent;
		TWeakObjectPtr<UUERLTargetCaptureComponent> Capture;
		FTransform OriginalTargetTransform;
		ECollisionChannel OriginalObjectType = ECC_Pawn;
		FCollisionResponseContainer OriginalResponses;
		bool OriginalNotify = false;
	};

	class FCatchFactory final : public IUERLEnvironmentFactory
	{
	public:
		FCatchFactory()
		{
			Descriptor.Id = UERLCatch::EnvironmentId;
			Descriptor.Version = 1;
			Descriptor.CollisionScope = EUERLEnvironmentCollisionScope::SharedWorld;
			FUERLFieldDescriptor Position;
			Position.Name = UERLCatch::TargetPositionField;
			Position.Shape = { 3 }; Position.Width = 3;
			Position.Unit = TEXT("m"); Position.CoordinateFrame = TEXT("slot/local");
			Position.Semantic = TEXT("pursuit_target_position"); Position.Source = TEXT("uerl.environment");
			Descriptor.StateFields.Add(Position);
			FUERLFieldDescriptor Captured;
			Captured.Name = UERLCatch::TargetCaptureField;
			Captured.Shape = { 1 }; Captured.Width = 1;
			Captured.Unit = TEXT("1"); Captured.CoordinateFrame = TEXT("none");
			Captured.Semantic = TEXT("target_capture"); Captured.Source = TEXT("uerl.environment");
			Descriptor.StateFields.Add(Captured);
		}
		const FUERLEnvironmentDescriptor& Describe() const override { return Descriptor; }
		bool ValidateConfig(const FUERLProviderConfig& Input, FUERLProviderConfig& OutEffective, FString& OutError) const override
		{
			const TMap<FName, double> Defaults = {
				{TargetX, 1.5}, {TargetY, 0.0}, {TargetSpeed, 0.15}, {TargetRadius, 0.5}, {PlayerTarget, 0.0}
			};
			for (const TPair<FName, double>& Pair : Input.Scalars)
			{
				if (!Defaults.Contains(Pair.Key) || !FMath::IsFinite(Pair.Value))
				{ OutError = TEXT("Catch has an unknown or non-finite scalar"); return false; }
			}
			if (!Input.ResetDistributions.IsEmpty() || !Input.AssetPath.IsEmpty()
				|| !Input.Actuators.IsEmpty() || !Input.ResetBindings.IsEmpty())
			{ OutError = TEXT("Catch accepts only Environment scalars"); return false; }
			OutEffective = Input;
			for (const TPair<FName, double>& Pair : Defaults) { OutEffective.Scalars.FindOrAdd(Pair.Key, Pair.Value); }
			if (OutEffective.Scalars[TargetSpeed] < 0.0 || OutEffective.Scalars[TargetRadius] <= 0.0
				|| (OutEffective.Scalars[PlayerTarget] != 0.0 && OutEffective.Scalars[PlayerTarget] != 1.0)
				|| FMath::Abs(OutEffective.Scalars[TargetX]) + 2.0 * OutEffective.Scalars[TargetRadius] > 15.0
				|| FMath::Abs(OutEffective.Scalars[TargetY]) + OutEffective.Scalars[TargetRadius] > 15.0)
			{ OutError = TEXT("Catch target motion must remain inside its arena with valid speed/radius/player mode"); return false; }
			return true;
		}
		TUniquePtr<IUERLEnvironment> Create(const FUERLProviderConfig& Effective) const override
		{ return MakeUnique<FCatchEnvironment>(Effective); }
	private:
		FUERLEnvironmentDescriptor Descriptor;
	};
}

TSharedRef<IUERLEnvironmentFactory> UERLCatch::MakeFactory() { return MakeShared<FCatchFactory>(); }
