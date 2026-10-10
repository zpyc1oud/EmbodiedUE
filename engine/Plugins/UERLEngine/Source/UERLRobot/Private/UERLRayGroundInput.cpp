#include "UERLRayGroundInput.h"

#include "UERLGroundQuery.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace UERLRayGroundPrivate
{
	struct FParameters
	{
		TArray<FVector2D> Offsets = {FVector2D::ZeroVector};
		double StartHeightM = 1.0;
		double EndDepthM = 2.0;
		FString Alignment = TEXT("yaw");
		FString Output = TEXT("height");
	};

	bool Parse(const FUERLInputSpec& Spec, FParameters& Out, FString& OutError)
	{
		TSharedPtr<FJsonObject> Json;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Spec.ParametersJson), Json) || !Json)
		{
			OutError = TEXT("ray parameters must be an object"); return false;
		}
		const TSet<FString> Allowed = {TEXT("offsets_m"), TEXT("start_height_m"), TEXT("end_depth_m"),
			TEXT("alignment"), TEXT("output")};
		for (const auto& Pair : Json->Values)
		{
			if (!Allowed.Contains(Pair.Key))
			{
				OutError = FString::Printf(TEXT("unknown ray parameter '%s'"), *Pair.Key); return false;
			}
		}
		if (Json->HasField(TEXT("offsets_m")))
		{
			const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
			if (!Json->TryGetArrayField(TEXT("offsets_m"), Rows) || !Rows || Rows->IsEmpty())
			{
				OutError = TEXT("offsets_m must contain XY pairs"); return false;
			}
			Out.Offsets.Reset();
			for (const TSharedPtr<FJsonValue>& Row : *Rows)
			{
				if (!Row || Row->Type != EJson::Array || Row->AsArray().Num() != 2)
				{
					OutError = TEXT("offsets_m entries must contain two numbers"); return false;
				}
				const TArray<TSharedPtr<FJsonValue>>& XY = Row->AsArray();
				if (XY[0]->Type != EJson::Number || XY[1]->Type != EJson::Number
					|| !FMath::IsFinite(XY[0]->AsNumber() * 100.0) || !FMath::IsFinite(XY[1]->AsNumber() * 100.0))
				{
					OutError = TEXT("offsets_m coordinates must be finite numbers"); return false;
				}
				Out.Offsets.Add(FVector2D(XY[0]->AsNumber(), XY[1]->AsNumber()));
			}
		}
		auto Distance = [&Json, &OutError](const TCHAR* Name, double& Value)
		{
			if (Json->HasField(Name) && !Json->TryGetNumberField(Name, Value))
			{
				OutError = FString::Printf(TEXT("%s must be a number"), Name); return false;
			}
			if (!FMath::IsFinite(Value * 100.0) || Value <= 0.0)
			{
				OutError = FString::Printf(TEXT("%s must be finite and positive"), Name); return false;
			}
			return true;
		};
		if (!Distance(TEXT("start_height_m"), Out.StartHeightM) || !Distance(TEXT("end_depth_m"), Out.EndDepthM))
		{ return false; }
		if ((Json->HasField(TEXT("alignment")) && !Json->TryGetStringField(TEXT("alignment"), Out.Alignment))
			|| (Out.Alignment != TEXT("yaw") && Out.Alignment != TEXT("world")))
		{
			OutError = TEXT("alignment must be yaw or world"); return false;
		}
		if ((Json->HasField(TEXT("output")) && !Json->TryGetStringField(TEXT("output"), Out.Output))
			|| (Out.Output != TEXT("height") && Out.Output != TEXT("clearance")))
		{
			OutError = TEXT("output must be height or clearance"); return false;
		}
		return true;
	}

	class FProvider final : public IUERLInputProvider
	{
	public:
		FProvider(FUERLInputSpec InSpec, FParameters InParameters)
			: Spec(MoveTemp(InSpec)), Parameters(MoveTemp(InParameters)) {}
		bool Bind(TConstArrayView<FUERLInputSlotBinding> Bindings, FString& OutError) override
		{
			Release();
			for (const FUERLInputSlotBinding& Binding : Bindings)
			{
				const auto* Reader = Binding.TransformReaders.Find(Spec.Attachment);
				const auto* Actors = Binding.SceneBindings.Find(Spec.SceneBindings[0]);
				if (!Binding.World.IsValid() || !Binding.Slot.CollisionProfile.IsValid()
					|| !Reader || !*Reader || !Actors || Actors->IsEmpty())
				{
					OutError = FString::Printf(TEXT("ray Slot %d requires World, collision profile, attachment '%s' and ground '%s'"),
						Binding.Slot.SlotId, *Spec.Attachment, *Spec.SceneBindings[0]); return false;
				}
				FSlot& Slot = Slots.Add(Binding.Slot.SlotId);
				Slot.World = Binding.World;
				Slot.Reader = *Reader;
				Slot.Actors = *Actors;
				Slot.CollisionProfile = Binding.Slot.CollisionProfile;
				Slot.IgnoredActor = Binding.Robot.IsValid() ? Binding.Robot->GetOwner() : nullptr;
			}
			return true;
		}
		bool Sample(int32 SlotId, const FUERLInputSampleStamp&, TArrayView<float> Values, FString& OutError) override
		{
			FSlot* Slot = Slots.Find(SlotId);
			if (!Slot || !Slot->World.IsValid() || Values.Num() != Parameters.Offsets.Num())
			{
				OutError = TEXT("ray binding expired or output width differs"); return false;
			}
			FTransform Transform;
			if (!Slot->Reader(Transform, OutError)) { return false; }
			if (Transform.ContainsNaN() || !Transform.GetRotation().IsNormalized())
			{
				OutError = TEXT("attachment transform must be finite with a unit rotation"); return false;
			}
			const FVector Position = Transform.GetLocation();
			const FQuat Rotation = Parameters.Alignment == TEXT("yaw")
				? FRotator(0.0, Transform.Rotator().Yaw, 0.0).Quaternion() : FQuat::Identity;
			FUERLGroundQueryContext Query;
			Query.IgnoredActor = Slot->IgnoredActor.Get(); Query.CollisionProfile = Slot->CollisionProfile;
			Query.PermittedActors = &Slot->Actors; Query.ScratchHits = &Slot->Hits;
			Query.Purpose = EUERLTerrainQueryPurpose::ExplicitBinding;
			for (int32 Index = 0; Index < Parameters.Offsets.Num(); ++Index)
			{
				const FVector2D& XY = Parameters.Offsets[Index];
				const FVector Probe = Position + Rotation.RotateVector(FVector(XY.X * 100.0, XY.Y * 100.0, 0.0));
				FHitResult Hit;
				if (!QueryUERLGroundHit(Query, *Slot->World.Get(),
					Probe + FVector::UpVector * (Parameters.StartHeightM * 100.0),
					Probe - FVector::UpVector * (Parameters.EndDepthM * 100.0), TEXT("declared-ray"), Hit, OutError))
				{
					OutError = FString::Printf(TEXT("ray %d: %s"), Index, *OutError); return false;
				}
				const double HeightM = (Hit.ImpactPoint.Z - Position.Z) / 100.0;
				Values[Index] = static_cast<float>(Parameters.Output == TEXT("height") ? HeightM : -HeightM);
			}
			return true;
		}
		bool Reset(TConstArrayView<int32>, FString&) override { return true; }
		void Release() override { Slots.Reset(); }
	private:
		struct FSlot
		{
			TWeakObjectPtr<UWorld> World;
			TWeakObjectPtr<AActor> IgnoredActor;
			TFunction<bool(FTransform&, FString&)> Reader;
			TArray<TWeakObjectPtr<AActor>> Actors;
			FUERLSlotCollisionProfile CollisionProfile;
			TArray<FHitResult> Hits;
		};
		FUERLInputSpec Spec;
		FParameters Parameters;
		TMap<int32, FSlot> Slots;
	};

	class FFactory final : public IUERLInputFactory
	{
	public:
		FString GetId() const override { return TEXT("uerl.ray_ground"); }
		int32 GetVersion() const override { return 1; }
		bool Validate(const FUERLInputSpec& Spec, FUERLInputSpec& OutSpec,
			TArray<FUERLFieldDescriptor>& Fields, FString& OutError) const override
		{
			if (Spec.ProviderId != GetId() || Spec.ProviderVersion != GetVersion() || Spec.Attachment.IsEmpty()
				|| Spec.SceneBindings.Num() > 1)
			{
				OutError = TEXT("ray input requires version 1, an attachment and one logical ground binding"); return false;
			}
			FParameters Parameters;
			if (!Parse(Spec, Parameters, OutError)) { return false; }
			OutSpec = Spec;
			if (OutSpec.SceneBindings.IsEmpty()) { OutSpec.SceneBindings.Add(TEXT("ground")); }
			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const FVector2D& XY : Parameters.Offsets)
			{
				TArray<TSharedPtr<FJsonValue>> Pair = {MakeShared<FJsonValueNumber>(XY.X), MakeShared<FJsonValueNumber>(XY.Y)};
				Rows.Add(MakeShared<FJsonValueArray>(Pair));
			}
			Json->SetArrayField(TEXT("offsets_m"), Rows);
			Json->SetNumberField(TEXT("start_height_m"), Parameters.StartHeightM);
			Json->SetNumberField(TEXT("end_depth_m"), Parameters.EndDepthM);
			Json->SetStringField(TEXT("alignment"), Parameters.Alignment);
			Json->SetStringField(TEXT("output"), Parameters.Output);
			OutSpec.ParametersJson.Reset();
			FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&OutSpec.ParametersJson));
			FUERLFieldDescriptor Field;
			Field.Name = FName(*FString::Printf(TEXT("input.%s.%s"), *Spec.Name, *Parameters.Output));
			Field.Shape = {Parameters.Offsets.Num()}; Field.Width = Parameters.Offsets.Num();
			Field.Unit = TEXT("m"); Field.CoordinateFrame = TEXT("world/z-relative"); Field.Source = GetId();
			Field.Semantic = Parameters.Output == TEXT("height") ? TEXT("terrain_height") : TEXT("ground_clearance");
			Fields.Add(Field);
			return true;
		}
		TUniquePtr<IUERLInputProvider> Create(const FUERLInputSpec& Spec) const override
		{
			FParameters Parameters; FString Error;
			if (!Parse(Spec, Parameters, Error)) { return nullptr; }
			return MakeUnique<FProvider>(Spec, MoveTemp(Parameters));
		}
	};
}

TSharedRef<IUERLInputFactory> MakeUERLRayGroundInputFactory()
{
	return MakeShared<UERLRayGroundPrivate::FFactory>();
}
