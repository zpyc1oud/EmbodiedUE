#include "UERLSocketBridgeCodec.h"

#include "UERLJson.h"
#include "UERLProtocol.h"
#include "UERLTransportAdapter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace UERLSocketBridgeCodecInternal
{
	using namespace UERLProtocol;
	using namespace UERLBatchLayout;

	bool ReadFinite(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, double& OutValue, FString& OutError)
	{
		if (!Object->TryGetNumberField(Key, OutValue) || !FMath::IsFinite(OutValue))
		{
			OutError = FString::Printf(TEXT("'%s' must be a finite number"), Key);
			return false;
		}
		return true;
	}

	bool ReadDecimationRange(const TSharedPtr<FJsonObject>& Object, int32& OutMin, int32& OutMax, FString& OutError)
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(TEXT("decimation"), Values) || !Values || Values->Num() != 2)
		{
			OutError = TEXT("'decimation' must be an ordered two-element array");
			return false;
		}
		double MinNumber = 0.0;
		double MaxNumber = 0.0;
		if ((*Values)[0].IsValid() && (*Values)[0]->Type == EJson::Number) { MinNumber = (*Values)[0]->AsNumber(); }
		else { OutError = TEXT("'decimation' endpoints must be integral int32 values"); return false; }
		if ((*Values)[1].IsValid() && (*Values)[1]->Type == EJson::Number) { MaxNumber = (*Values)[1]->AsNumber(); }
		else { OutError = TEXT("'decimation' endpoints must be integral int32 values"); return false; }
		if (!FMath::IsFinite(MinNumber) || !FMath::IsFinite(MaxNumber)
			|| MinNumber != FMath::FloorToDouble(MinNumber)
			|| MaxNumber != FMath::FloorToDouble(MaxNumber)
			|| MinNumber < 1.0 || MaxNumber < MinNumber
			|| MaxNumber > static_cast<double>(TNumericLimits<int32>::Max()))
		{
			OutError = TEXT("'decimation' endpoints must be an ordered positive int32 range");
			return false;
		}
		OutMin = static_cast<int32>(MinNumber);
		OutMax = static_cast<int32>(MaxNumber);
		return true;
	}

	bool ReadU64(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, uint64& OutValue, FString& OutError)
	{
		FString Text;
		if (!Object->TryGetStringField(Key, Text) || Text.IsEmpty()
			|| (Text.Len() > 1 && Text[0] == TEXT('0')))
		{
			OutError = FString::Printf(TEXT("'%s' must be canonical u64dec"), Key);
			return false;
		}
		for (TCHAR Character : Text)
		{
			if (!FChar::IsDigit(Character))
			{
				OutError = FString::Printf(TEXT("'%s' must be canonical u64dec"), Key);
				return false;
			}
		}
		const uint64 Value = FCString::Strtoui64(*Text, nullptr, 10);
		if (LexToString(Value) != Text) { OutError = FString::Printf(TEXT("'%s' is outside uint64"), Key); return false; }
		OutValue = Value;
		return true;
	}

	bool IsFieldName(const FString& Name)
	{
		if (Name.IsEmpty() || Name[0] < TEXT('a') || Name[0] > TEXT('z')) { return false; }
		for (TCHAR Character : Name)
		{
			if (!((Character >= TEXT('a') && Character <= TEXT('z'))
				|| FChar::IsDigit(Character) || Character == TEXT('_') || Character == TEXT('.'))) { return false; }
		}
		return true;
	}

	bool ParseTerrainTier(const TSharedPtr<FJsonObject>& Object, FUERLTerrainTierConfig& Out, FString& OutError)
	{
		if (!UERLSocketBridgeCodec::CheckKeys(Object,
			{ TEXT("level"), TEXT("primitive"), TEXT("seed"), TEXT("platform_width"), TEXT("params") },
			{}, OutError))
		{
			return false;
		}
		FString Primitive;
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!UERLSocketBridgeCodec::ReadInt(Object, TEXT("level"), 0, 65535, Out.Level, OutError)
			|| !ReadU64(Object, TEXT("seed"), Out.Seed, OutError)
			|| !ReadFinite(Object, TEXT("platform_width"), Out.PlatformWidth, OutError) || Out.PlatformWidth < 0.0
			|| !Object->TryGetStringField(TEXT("primitive"), Primitive)
			|| !Object->TryGetObjectField(TEXT("params"), Params) || !Params || !Params->IsValid())
		{
			if (OutError.IsEmpty()) { OutError = TEXT("terrain tier contains invalid values"); }
			return false;
		}
		if (Primitive == TEXT("plane")) { Out.Primitive = EUERLTerrainPrimitive::Plane; }
		else if (Primitive == TEXT("heightfield")) { Out.Primitive = EUERLTerrainPrimitive::Heightfield; }
		else if (Primitive == TEXT("boxes")) { Out.Primitive = EUERLTerrainPrimitive::Boxes; }
		else { OutError = TEXT("unsupported terrain primitive"); return false; }
		Out.Params = *Params;
		return true;
	}

	bool ParseTerrain(const TSharedPtr<FJsonObject>& Object, FUERLTerrainConfig& Out, FString& OutError)
	{
		if (!UERLSocketBridgeCodec::CheckKeys(Object,
			{ TEXT("num_levels"), TEXT("cell_size"), TEXT("border_width"), TEXT("tiers") },
			{ TEXT("physics_collision") }, OutError))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* CellSize = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Tiers = nullptr;
		Out.bPhysicsCollision = true;
		if (Object->HasField(TEXT("physics_collision")))
		{
			if (!Object->TryGetBoolField(TEXT("physics_collision"), Out.bPhysicsCollision))
			{
				OutError = TEXT("terrain physics_collision must be a boolean");
				return false;
			}
		}
		if (!UERLSocketBridgeCodec::ReadInt(Object, TEXT("num_levels"), 1, 65535, Out.NumLevels, OutError)
			|| !ReadFinite(Object, TEXT("border_width"), Out.BorderWidth, OutError) || Out.BorderWidth < 0.0
			|| !Object->TryGetArrayField(TEXT("cell_size"), CellSize) || !CellSize || CellSize->Num() != 2
			|| !Object->TryGetArrayField(TEXT("tiers"), Tiers) || !Tiers)
		{
			if (OutError.IsEmpty()) { OutError = TEXT("terrain contains invalid values"); }
			return false;
		}
		for (int32 Axis = 0; Axis < 2; ++Axis)
		{
			const TSharedPtr<FJsonValue>& Value = (*CellSize)[Axis];
			if (!Value.IsValid() || Value->Type != EJson::Number
				|| !FMath::IsFinite(Value->AsNumber()) || Value->AsNumber() <= 0.0)
			{
				OutError = TEXT("terrain cell_size entries must be finite positive numbers");
				return false;
			}
			Out.CellSize[Axis] = Value->AsNumber();
		}
		if (Tiers->Num() != Out.NumLevels)
		{
			OutError = TEXT("terrain tiers length must equal num_levels");
			return false;
		}
		Out.Tiers.Reset();
		Out.Tiers.Reserve(Tiers->Num());
		for (int32 Index = 0; Index < Tiers->Num(); ++Index)
		{
			const TSharedPtr<FJsonValue>& Value = (*Tiers)[Index];
			FUERLTerrainTierConfig Tier;
			if (!Value.IsValid() || Value->Type != EJson::Object
				|| !ParseTerrainTier(Value->AsObject(), Tier, OutError))
			{
				return false;
			}
			if (Tier.Level != Index)
			{
				OutError = TEXT("terrain tier levels must start at 0 and increase contiguously");
				return false;
			}
			Out.Tiers.Add(MoveTemp(Tier));
		}
		return true;
	}

	bool ParseActuatorProjection(
		const TArray<TSharedPtr<FJsonValue>>& Values,
		TArray<FUERLActuatorConfig>& Out,
		FString& OutError)
	{
		Out.Reset();
		for (int32 Position = 0; Position < Values.Num(); ++Position)
		{
			const TSharedPtr<FJsonObject> Item = Values[Position].IsValid() && Values[Position]->Type == EJson::Object
				? Values[Position]->AsObject() : nullptr;
			if (!UERLSocketBridgeCodec::CheckKeys(Item,
				{ TEXT("index"), TEXT("joint_index"), TEXT("joint"), TEXT("coordinate"),
				  TEXT("coordinate_type"), TEXT("unit"), TEXT("target_mode"), TEXT("stiffness"),
				  TEXT("damping"), TEXT("effort_limit"), TEXT("default_pos") }, {}, OutError))
			{
				return false;
			}
			FUERLActuatorConfig& Binding = Out.AddDefaulted_GetRef();
			FString JointName;
			if (!UERLSocketBridgeCodec::ReadInt(Item, TEXT("index"), 0, 65535, Binding.Index, OutError)
				|| !UERLSocketBridgeCodec::ReadInt(Item, TEXT("joint_index"), 0, 65535, Binding.JointIndex, OutError)
				|| !Item->TryGetStringField(TEXT("joint"), JointName)
				|| !Item->TryGetStringField(TEXT("coordinate"), Binding.Coordinate)
				|| !Item->TryGetStringField(TEXT("coordinate_type"), Binding.CoordinateType)
				|| !Item->TryGetStringField(TEXT("unit"), Binding.Unit)
				|| !Item->TryGetStringField(TEXT("target_mode"), Binding.TargetMode)
				|| !ReadFinite(Item, TEXT("stiffness"), Binding.Stiffness, OutError)
				|| !ReadFinite(Item, TEXT("damping"), Binding.Damping, OutError)
				|| !ReadFinite(Item, TEXT("effort_limit"), Binding.EffortLimit, OutError)
				|| !ReadFinite(Item, TEXT("default_pos"), Binding.DefaultPosition, OutError))
			{
				if (OutError.IsEmpty()) { OutError = TEXT("actuator projection contains invalid values"); }
				return false;
			}
			if (JointName.IsEmpty())
			{
				OutError = TEXT("actuator projection joint must be a non-empty string");
				return false;
			}
			Binding.JointName = FName(*JointName);
			if (Binding.Index != Position || !Binding.IsValid())
			{
				OutError = TEXT("actuator projection indices or values are invalid");
				return false;
			}
		}
		return true;
	}

	bool ParseResetProjection(
		const TArray<TSharedPtr<FJsonValue>>& Values,
		TArray<FUERLResetBinding>& Out,
		FString& OutError)
	{
		Out.Reset();
		for (int32 Position = 0; Position < Values.Num(); ++Position)
		{
			const TSharedPtr<FJsonObject> Item = Values[Position].IsValid() && Values[Position]->Type == EJson::Object
				? Values[Position]->AsObject() : nullptr;
			if (!UERLSocketBridgeCodec::CheckKeys(Item,
				{ TEXT("index"), TEXT("name"), TEXT("target_type"), TEXT("joint_index"), TEXT("component_index"),
				  TEXT("body_index"), TEXT("coordinate"), TEXT("unit") }, {}, OutError))
			{
				return false;
			}
			FUERLResetBinding& Binding = Out.AddDefaulted_GetRef();
			FString Name;
			if (!UERLSocketBridgeCodec::ReadInt(Item, TEXT("index"), 0, 65535, Binding.Index, OutError)
				|| !Item->TryGetStringField(TEXT("name"), Name)
				|| !Item->TryGetStringField(TEXT("target_type"), Binding.TargetType)
				|| !UERLSocketBridgeCodec::ReadInt(Item, TEXT("joint_index"), -1, 65535, Binding.JointIndex, OutError)
				|| !UERLSocketBridgeCodec::ReadInt(
					Item, TEXT("component_index"), 0, 65535, Binding.ComponentIndex, OutError)
				|| !UERLSocketBridgeCodec::ReadInt(Item, TEXT("body_index"), -1, 65535, Binding.BodyIndex, OutError)
				|| !Item->TryGetStringField(TEXT("coordinate"), Binding.Coordinate)
				|| !Item->TryGetStringField(TEXT("unit"), Binding.Unit))
			{
				if (OutError.IsEmpty()) { OutError = TEXT("reset projection contains invalid values"); }
				return false;
			}
			Binding.Name = FName(*Name);
			if (Binding.Index != Position || !Binding.IsValid())
			{
				OutError = TEXT("reset projection indices or values are invalid");
				return false;
			}
		}
		return true;
	}

	bool ParseProvider(
		const TSharedPtr<FJsonObject>& Object,
		FName& OutId,
		FUERLProviderConfig& OutConfig,
		bool bAllowTerrain,
		bool bAllowAssetPath,
		bool bAllowRobotProjection,
		FString& OutError)
	{
		TArray<FString> Optional = { TEXT("extensions") };
		if (bAllowTerrain) { Optional.Add(TEXT("terrain")); }
		if (bAllowAssetPath) { Optional.Add(TEXT("asset_path")); }
		if (bAllowRobotProjection)
		{
			Optional.Add(TEXT("actuators"));
			Optional.Add(TEXT("reset_bindings"));
		}
		if (!UERLSocketBridgeCodec::CheckKeys(Object, { TEXT("id"), TEXT("scalars") }, Optional, OutError))
		{
			return false;
		}
		FString Id;
		const TSharedPtr<FJsonObject>* Scalars = nullptr;
		if (!Object->TryGetStringField(TEXT("id"), Id) || Id.IsEmpty()
			|| !Object->TryGetObjectField(TEXT("scalars"), Scalars) || !Scalars || !Scalars->IsValid())
		{
			OutError = TEXT("provider id/scalars types are invalid");
			return false;
		}
		OutId = FName(*Id);
		OutConfig = FUERLProviderConfig();
		if (Object->HasField(TEXT("asset_path")))
		{
			if (!bAllowAssetPath || !Object->TryGetStringField(TEXT("asset_path"), OutConfig.AssetPath)
				|| !FUERLProviderConfig::IsValidAssetPath(OutConfig.AssetPath))
			{
				OutError = TEXT("robot asset_path must be a UE object path beginning with '/' without backslashes");
				return false;
			}
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*Scalars)->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number
				|| !FMath::IsFinite(Pair.Value->AsNumber()))
			{
				OutError = FString::Printf(TEXT("provider scalar '%s' must be finite"), *Pair.Key);
				return false;
			}
			OutConfig.Scalars.Add(FName(*Pair.Key), Pair.Value->AsNumber());
		}
		if (bAllowRobotProjection)
		{
			const TArray<TSharedPtr<FJsonValue>>* Actuators = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* ResetBindings = nullptr;
			const bool bHasActuators = Object->HasField(TEXT("actuators"));
			const bool bHasResetBindings = Object->HasField(TEXT("reset_bindings"));
			if (bHasActuators != bHasResetBindings
				|| (bHasActuators && (!Object->TryGetArrayField(TEXT("actuators"), Actuators) || !Actuators
					|| !Object->TryGetArrayField(TEXT("reset_bindings"), ResetBindings) || !ResetBindings
					|| !ParseActuatorProjection(*Actuators, OutConfig.Actuators, OutError)
					|| !ParseResetProjection(*ResetBindings, OutConfig.ResetBindings, OutError))))
			{
				if (OutError.IsEmpty()) { OutError = TEXT("robot indexed projection is invalid"); }
				return false;
			}
		}
		return true;
	}

	TSharedRef<FJsonObject> ProviderJson(FName Id, const FUERLProviderConfig& Config)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("id"), Id.ToString());
		TSharedRef<FJsonObject> Scalars = MakeShared<FJsonObject>();
		for (const TPair<FName, double>& Pair : Config.Scalars)
		{
			Scalars->SetNumberField(Pair.Key.ToString(), Pair.Value);
		}
		Result->SetObjectField(TEXT("scalars"), Scalars);
		if (!Config.Actuators.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> Actuators;
			for (const FUERLActuatorConfig& Binding : Config.Actuators)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Binding.Index);
				Item->SetNumberField(TEXT("joint_index"), Binding.JointIndex);
				Item->SetStringField(TEXT("joint"), Binding.JointName.ToString());
				Item->SetStringField(TEXT("coordinate"), Binding.Coordinate);
				Item->SetStringField(TEXT("coordinate_type"), Binding.CoordinateType);
				Item->SetStringField(TEXT("unit"), Binding.Unit);
				Item->SetStringField(TEXT("target_mode"), Binding.TargetMode);
				Item->SetNumberField(TEXT("stiffness"), Binding.Stiffness);
				Item->SetNumberField(TEXT("damping"), Binding.Damping);
				Item->SetNumberField(TEXT("effort_limit"), Binding.EffortLimit);
				Item->SetNumberField(TEXT("default_pos"), Binding.DefaultPosition);
				Actuators.Add(MakeShared<FJsonValueObject>(Item));
			}
			Result->SetArrayField(TEXT("actuators"), Actuators);
		}
		if (!Config.ResetBindings.IsEmpty())
		{
			TArray<TSharedPtr<FJsonValue>> ResetBindings;
			for (const FUERLResetBinding& Binding : Config.ResetBindings)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("index"), Binding.Index);
				Item->SetStringField(TEXT("name"), Binding.Name.ToString());
				Item->SetStringField(TEXT("target_type"), Binding.TargetType);
				Item->SetNumberField(TEXT("joint_index"), Binding.JointIndex);
				Item->SetNumberField(TEXT("component_index"), Binding.ComponentIndex);
				Item->SetNumberField(TEXT("body_index"), Binding.BodyIndex);
				Item->SetStringField(TEXT("coordinate"), Binding.Coordinate);
				Item->SetStringField(TEXT("unit"), Binding.Unit);
				ResetBindings.Add(MakeShared<FJsonValueObject>(Item));
			}
			Result->SetArrayField(TEXT("reset_bindings"), ResetBindings);
		}
		if (!Config.AssetPath.IsEmpty())
		{
			Result->SetStringField(TEXT("asset_path"), Config.AssetPath);
		}
		return Result;
	}

	TSharedRef<FJsonObject> TerrainJson(const FUERLTerrainConfig& Terrain)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("num_levels"), Terrain.NumLevels);
		TArray<TSharedPtr<FJsonValue>> CellSize;
		CellSize.Add(MakeShared<FJsonValueNumber>(Terrain.CellSize[0]));
		CellSize.Add(MakeShared<FJsonValueNumber>(Terrain.CellSize[1]));
		Result->SetArrayField(TEXT("cell_size"), CellSize);
		Result->SetNumberField(TEXT("border_width"), Terrain.BorderWidth);
		Result->SetBoolField(TEXT("physics_collision"), Terrain.bPhysicsCollision);
		TArray<TSharedPtr<FJsonValue>> Tiers;
		for (const FUERLTerrainTierConfig& Tier : Terrain.Tiers)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetNumberField(TEXT("level"), Tier.Level);
			const TCHAR* Primitive =
				Tier.Primitive == EUERLTerrainPrimitive::Heightfield ? TEXT("heightfield")
				: Tier.Primitive == EUERLTerrainPrimitive::Boxes ? TEXT("boxes")
				: TEXT("plane");
			Item->SetStringField(TEXT("primitive"), Primitive);
			Item->SetStringField(TEXT("seed"), LexToString(Tier.Seed));
			Item->SetNumberField(TEXT("platform_width"), Tier.PlatformWidth);
			Item->SetObjectField(TEXT("params"), Tier.Params);
			Tiers.Add(MakeShared<FJsonValueObject>(Item));
		}
		Result->SetArrayField(TEXT("tiers"), Tiers);
		return Result;
	}

	bool ParseObservationType(const FString& Text, EUERLObservationType& OutType)
	{
		if (Text == TEXT("joint_position")) { OutType = EUERLObservationType::JointPosition; return true; }
		if (Text == TEXT("joint_velocity")) { OutType = EUERLObservationType::JointVelocity; return true; }
		if (Text == TEXT("body_pose")) { OutType = EUERLObservationType::BodyPose; return true; }
		if (Text == TEXT("body_linear_velocity")) { OutType = EUERLObservationType::BodyLinearVelocity; return true; }
		if (Text == TEXT("body_angular_velocity")) { OutType = EUERLObservationType::BodyAngularVelocity; return true; }
		if (Text == TEXT("ground_clearance")) { OutType = EUERLObservationType::GroundClearance; return true; }
		if (Text == TEXT("contact")) { OutType = EUERLObservationType::Contact; return true; }
		if (Text == TEXT("contact_force")) { OutType = EUERLObservationType::ContactForce; return true; }
		if (Text == TEXT("terrain_height")) { OutType = EUERLObservationType::TerrainHeight; return true; }
		return false;
	}

	bool ParseObservationExtensions(const TSharedPtr<FJsonObject>& Object, FUERLFieldDescriptor& Out, FString& OutError)
	{
		const TSharedPtr<FJsonValue>* ExtensionsValue = Object->Values.Find(TEXT("extensions"));
		if (!ExtensionsValue)
		{
			return true;
		}
		if (!ExtensionsValue->IsValid() || (*ExtensionsValue)->Type != EJson::Object)
		{
			OutError = TEXT("field extensions must be an object");
			return false;
		}
		const TSharedPtr<FJsonObject> Extensions = (*ExtensionsValue)->AsObject();
		if (!UERLSocketBridgeCodec::CheckKeys(Extensions,
			{ TEXT("body_index"), TEXT("body_name"), TEXT("observation_type") },
			{ TEXT("joint_index"), TEXT("joint_name"), TEXT("target"), TEXT("target_kind"),
			  TEXT("coordinate"), TEXT("coordinate_type") }, OutError))
		{
			return false;
		}
		FString BodyName;
		FString ObservationType;
		if (!Extensions->TryGetStringField(TEXT("body_name"), BodyName) || BodyName.IsEmpty()
			|| !Extensions->TryGetStringField(TEXT("observation_type"), ObservationType)
			|| !ParseObservationType(ObservationType, Out.Observation.Type))
		{
			OutError = TEXT("observation extensions contain an unknown body or observation type");
			return false;
		}
		Out.Observation.BodyName = FName(*BodyName);
		if (!UERLSocketBridgeCodec::ReadInt(Extensions, TEXT("body_index"), 0, 65535, Out.Observation.BodyIndex, OutError))
		{
			return false;
		}
		const bool bJointObservation = Out.Observation.Type == EUERLObservationType::JointPosition
			|| Out.Observation.Type == EUERLObservationType::JointVelocity;
		if (bJointObservation != Extensions->HasField(TEXT("joint_index"))
			|| bJointObservation != Extensions->HasField(TEXT("joint_name")))
		{
			OutError = TEXT("joint observations require joint_index and joint_name; body observations must not provide them");
			return false;
		}
		if (bJointObservation)
		{
			FString JointName;
			if (!UERLSocketBridgeCodec::ReadInt(
					Extensions, TEXT("joint_index"), 0, 65535, Out.Observation.JointIndex, OutError)
				|| !Extensions->TryGetStringField(TEXT("joint_name"), JointName) || JointName.IsEmpty())
			{
				if (OutError.IsEmpty())
				{
					OutError = TEXT("joint observations require a non-empty joint_name");
				}
				return false;
			}
			Out.Observation.JointName = FName(*JointName);
		}
		return Out.Observation.IsValid();
	}

	bool ParseActuatorExtensions(const TSharedPtr<FJsonObject>& Object, FUERLFieldDescriptor& Out, FString& OutError)
	{
		const TSharedPtr<FJsonValue>* ExtensionsValue = Object->Values.Find(TEXT("extensions"));
		if (!ExtensionsValue)
		{
			Out.ActuatorColumns.Reset();
			return true;
		}
		if (!ExtensionsValue->IsValid() || (*ExtensionsValue)->Type != EJson::Object)
		{
			OutError = TEXT("action field extensions must be an object");
			return false;
		}
		const TSharedPtr<FJsonObject> Extensions = (*ExtensionsValue)->AsObject();
		if (!UERLSocketBridgeCodec::CheckKeys(Extensions, { TEXT("columns") }, {}, OutError)) { return false; }
		const TArray<TSharedPtr<FJsonValue>>* Columns = nullptr;
		if (!Extensions->TryGetArrayField(TEXT("columns"), Columns) || !Columns)
		{
			OutError = TEXT("action field columns must be an array");
			return false;
		}
		Out.ActuatorColumns.Reset();
		for (int32 Position = 0; Position < Columns->Num(); ++Position)
		{
			const TSharedPtr<FJsonObject> Column = (*Columns)[Position].IsValid()
				&& (*Columns)[Position]->Type == EJson::Object ? (*Columns)[Position]->AsObject() : nullptr;
			if (!UERLSocketBridgeCodec::CheckKeys(Column,
				{ TEXT("index"), TEXT("joint"), TEXT("joint_index"), TEXT("coordinate_type"),
				  TEXT("unit"), TEXT("target_mode") }, {}, OutError))
			{
				return false;
			}
			FUERLActuatorColumnDescriptor& Item = Out.ActuatorColumns.AddDefaulted_GetRef();
			FString Joint;
			FString CoordinateType;
			if (!UERLSocketBridgeCodec::ReadInt(Column, TEXT("index"), 0, 65535, Item.Index, OutError)
				|| !Column->TryGetStringField(TEXT("joint"), Joint)
				|| !UERLSocketBridgeCodec::ReadInt(Column, TEXT("joint_index"), 0, 65535, Item.JointIndex, OutError)
				|| !Column->TryGetStringField(TEXT("coordinate_type"), CoordinateType)
				|| !Column->TryGetStringField(TEXT("unit"), Item.Unit)
				|| !Column->TryGetStringField(TEXT("target_mode"), Item.TargetMode))
			{
				if (OutError.IsEmpty()) { OutError = TEXT("action column metadata is invalid"); }
				return false;
			}
			Item.JointName = FName(*Joint);
			Item.CoordinateType = MoveTemp(CoordinateType);
			if (Item.Index != Position || !Item.IsValid())
			{
				OutError = TEXT("action column metadata indices or values are invalid");
				return false;
			}
		}
		return Out.ActuatorColumns.Num() == Out.Width;
	}

	bool ParseField(const TSharedPtr<FJsonObject>& Object, FUERLFieldDescriptor& Out, bool bAction, FString& OutError)
	{
		if (!UERLSocketBridgeCodec::CheckKeys(Object,
			{ TEXT("name"), TEXT("dtype"), TEXT("shape"), TEXT("unit"), TEXT("frame"), TEXT("semantic"), TEXT("source") },
			{ TEXT("extensions") }, OutError)) { return false; }
		FString Name;
		const TArray<TSharedPtr<FJsonValue>>* Shape = nullptr;
		if (!Object->TryGetStringField(TEXT("name"), Name) || !IsFieldName(Name)
			|| !Object->TryGetStringField(TEXT("dtype"), Out.DType) || Out.DType != TEXT("float32")
			|| !Object->TryGetArrayField(TEXT("shape"), Shape) || !Shape
			|| !Object->TryGetStringField(TEXT("unit"), Out.Unit) || Out.Unit.IsEmpty()
			|| !Object->TryGetStringField(TEXT("frame"), Out.CoordinateFrame) || Out.CoordinateFrame.IsEmpty()
			|| !Object->TryGetStringField(TEXT("semantic"), Out.Semantic) || Out.Semantic.IsEmpty()
			|| !Object->TryGetStringField(TEXT("source"), Out.Source) || Out.Source.IsEmpty())
		{
			OutError = TEXT("field descriptor is incomplete or invalid");
			return false;
		}
		Out.Name = FName(*Name);
		Out.Shape.Reset();
		Out.Width = 1;
		for (const TSharedPtr<FJsonValue>& Dimension : *Shape)
		{
			if (!Dimension.IsValid() || Dimension->Type != EJson::Number || !FMath::IsFinite(Dimension->AsNumber())
				|| Dimension->AsNumber() != FMath::FloorToDouble(Dimension->AsNumber())
				|| Dimension->AsNumber() < 1 || Dimension->AsNumber() > 65536)
			{
				OutError = TEXT("field shape dimensions must be positive integers");
				return false;
			}
			Out.Shape.Add(static_cast<int32>(Dimension->AsNumber()));
			if (Out.Width > 1048576 / Out.Shape.Last())
			{
				OutError = TEXT("field shape exceeds the MVP0 element limit");
				return false;
			}
			Out.Width *= Out.Shape.Last();
		}
		if (bAction)
		{
			return ParseActuatorExtensions(Object, Out, OutError) && Out.IsValid();
		}
		if (Out.Source == TEXT("uerl.robot"))
		{
			return ParseObservationExtensions(Object, Out, OutError) && Out.IsValid();
		}
		const TSharedPtr<FJsonObject>* Extensions = nullptr;
		if (Object->TryGetObjectField(TEXT("extensions"), Extensions)
			&& Extensions && Extensions->IsValid() && !(*Extensions)->Values.IsEmpty())
		{
			OutError = TEXT("non-Robot State fields do not accept extension metadata");
			return false;
		}
		return Out.IsValid();
	}
}

namespace UERLSocketBridgeCodec
{
	using namespace UERLSocketBridgeCodecInternal;

	bool CheckKeys(
		const TSharedPtr<FJsonObject>& Object,
		const TArray<FString>& Required,
		const TArray<FString>& Optional,
		FString& OutError)
	{
		if (!Object.IsValid()) { OutError = TEXT("required JSON object is missing"); return false; }
		for (const FString& Key : Required)
		{
			if (!Object->HasField(Key))
			{
				OutError = FString::Printf(TEXT("missing required key '%s'"), *Key);
				return false;
			}
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object->Values)
		{
			if (!Required.Contains(Pair.Key) && !Optional.Contains(Pair.Key))
			{
				OutError = FString::Printf(TEXT("unknown critical key '%s'"), *Pair.Key);
				return false;
			}
		}
		return true;
	}

	bool ReadInt(
		const TSharedPtr<FJsonObject>& Object,
		const TCHAR* Key,
		int32 Min,
		int32 Max,
		int32& OutValue,
		FString& OutError)
	{
		double Number = 0;
		if (!Object->TryGetNumberField(Key, Number) || !FMath::IsFinite(Number)
			|| Number != FMath::FloorToDouble(Number) || Number < Min || Number > Max)
		{
			OutError = FString::Printf(TEXT("'%s' must be an integral number in range"), Key);
			return false;
		}
		OutValue = static_cast<int32>(Number);
		return true;
	}

	bool IsHex32(const FString& Text)
	{
		if (Text.Len() != 64) { return false; }
		for (TCHAR Character : Text)
		{
			if (!((Character >= TEXT('0') && Character <= TEXT('9'))
				|| (Character >= TEXT('a') && Character <= TEXT('f')))) { return false; }
		}
		return true;
	}

	bool IsUuidV4(const FGuid& Session)
	{
		return Session.IsValid() && ((Session.B >> 12) & 0x0f) == 4 && (Session.C >> 30) == 2;
	}

	bool ParseEvent(
		const TSharedPtr<FJsonObject>& Object,
		int32 NumSlots,
		FUERLEventBatch& Out,
		FString& OutError)
	{
		if (!CheckKeys(Object, { TEXT("kind"), TEXT("slot_ids"), TEXT("values") }, {}, OutError))
		{
			return false;
		}
		FString Kind;
		const TArray<TSharedPtr<FJsonValue>>* SlotIds = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetStringField(TEXT("kind"), Kind)
			|| !Object->TryGetArrayField(TEXT("slot_ids"), SlotIds) || !SlotIds
			|| !Object->TryGetArrayField(TEXT("values"), Values) || !Values
			|| SlotIds->Num() == 0 || SlotIds->Num() != Values->Num())
		{
			OutError = TEXT("event kind, Slot ids, and values must be non-empty aligned arrays");
			return false;
		}
		if (Kind == TEXT("ground_friction")) { Out.Kind = EUERLEventKind::GroundFriction; }
		else if (Kind == TEXT("terrain_tier_params")) { Out.Kind = EUERLEventKind::TerrainTierParams; }
		else if (Kind == TEXT("root_push")) { Out.Kind = EUERLEventKind::RootPush; }
		else
		{
			OutError = FString::Printf(TEXT("unsupported event kind '%s'"), *Kind);
			return false;
		}
		const int32 ValueWidth = Out.Kind == EUERLEventKind::GroundFriction
			? 2 : Out.Kind == EUERLEventKind::TerrainTierParams ? 1 : 3;
		Out.SlotIds.Reset();
		Out.Values.Reset();
		Out.SlotIds.Reserve(SlotIds->Num());
		Out.Values.Reserve(Values->Num());
		TSet<int32> Seen;
		for (int32 Row = 0; Row < SlotIds->Num(); ++Row)
		{
			const TSharedPtr<FJsonValue>& SlotValue = (*SlotIds)[Row];
			const TSharedPtr<FJsonValue>& VectorValue = (*Values)[Row];
			if (!SlotValue.IsValid() || SlotValue->Type != EJson::Number
				|| !FMath::IsFinite(SlotValue->AsNumber())
				|| SlotValue->AsNumber() != FMath::FloorToDouble(SlotValue->AsNumber())
				|| SlotValue->AsNumber() < 0.0 || SlotValue->AsNumber() >= NumSlots)
			{
				OutError = TEXT("event Slot ids must be unique integral values in the active batch");
				return false;
			}
			const int32 SlotId = static_cast<int32>(SlotValue->AsNumber());
			if (Seen.Contains(SlotId))
			{
				OutError = TEXT("event Slot ids must not contain duplicates");
				return false;
			}
			Seen.Add(SlotId);
			if (!VectorValue.IsValid() || VectorValue->Type != EJson::Array)
			{
				OutError = TEXT("event values must be numeric arrays");
				return false;
			}
			const TArray<TSharedPtr<FJsonValue>>& Components = VectorValue->AsArray();
			if (Components.Num() != ValueWidth)
			{
				OutError = FString::Printf(TEXT("event values must have width %d"), ValueWidth);
				return false;
			}
			FVector Parsed = FVector::ZeroVector;
			for (int32 Component = 0; Component < Components.Num(); ++Component)
			{
				const TSharedPtr<FJsonValue>& Value = Components[Component];
				if (!Value.IsValid() || Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber()))
				{
					OutError = TEXT("event values must be finite numbers");
					return false;
				}
				Parsed[Component] = Value->AsNumber();
			}
			Out.SlotIds.Add(SlotId);
			Out.Values.Add(Parsed);
		}
		return true;
	}

	bool ParseWorkerConfig(
		const TSharedPtr<FJsonObject>& Object,
		FUERLWorkerProjection& Out,
		FString& OutError)
	{
		if (!CheckKeys(Object,
			{ TEXT("num_slots"), TEXT("physics_dt"), TEXT("decimation"), TEXT("run_seed"),
			  TEXT("world_map"), TEXT("environment"), TEXT("robot") },
			{ TEXT("extensions"), TEXT("input_contract") }, OutError))
		{
			return false;
		}
		int32 NumSlots = 0;
		int32 DecimationMin = 0;
		int32 DecimationMax = 0;
		const TSharedPtr<FJsonObject>* Environment = nullptr;
		const TSharedPtr<FJsonObject>* Robot = nullptr;
		if (!ReadInt(Object, TEXT("num_slots"), 1, 65536, NumSlots, OutError)
			|| !ReadDecimationRange(Object, DecimationMin, DecimationMax, OutError)
			|| !ReadFinite(Object, TEXT("physics_dt"), Out.PhysicsDt, OutError)
			|| Out.PhysicsDt <= 0.0 || Out.PhysicsDt > 1.0
			|| !ReadU64(Object, TEXT("run_seed"), Out.RunSeed, OutError)
			|| !Object->TryGetStringField(TEXT("world_map"), Out.WorldMap)
			|| !FPackageName::IsValidLongPackageName(Out.WorldMap)
			|| !Object->TryGetObjectField(TEXT("environment"), Environment) || !Environment
			|| !Object->TryGetObjectField(TEXT("robot"), Robot) || !Robot)
		{
			if (OutError.IsEmpty()) { OutError = TEXT("worker_config contains invalid values"); }
			return false;
		}
		Out.NumSlots = NumSlots;
		Out.DecimationMin = DecimationMin;
		Out.DecimationMax = DecimationMax;
		Out.CommandWidth = 0;
		if (Object->HasField(TEXT("input_contract")))
		{
			const TSharedPtr<FJsonObject>* Contract = nullptr;
			if (!Object->TryGetObjectField(TEXT("input_contract"), Contract)
				|| !ParseUERLInputContract(*Contract, Out.Inputs, false, OutError))
			{
				return false;
			}
		}
		if (!ParseProvider(*Environment, Out.EnvironmentId, Out.EnvironmentConfig, true, false, false, OutError)
			|| !ParseProvider(*Robot, Out.RobotId, Out.RobotConfig, false, true, true, OutError))
		{
			return false;
		}
		const TSharedPtr<FJsonObject>* Terrain = nullptr;
		if ((*Environment)->TryGetObjectField(TEXT("terrain"), Terrain))
		{
			if (!Terrain || !Terrain->IsValid() || !ParseTerrain(*Terrain, Out.TerrainConfig, OutError))
			{
				if (OutError.IsEmpty()) { OutError = TEXT("environment.terrain is invalid"); }
				return false;
			}
		}
		return true;
	}

	TSharedRef<FJsonObject> WorkerConfigJson(const FUERLWorkerProjection& Projection)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("num_slots"), Projection.NumSlots);
		Result->SetNumberField(TEXT("physics_dt"), Projection.PhysicsDt);
		TArray<TSharedPtr<FJsonValue>> Decimation;
		Decimation.Add(MakeShared<FJsonValueNumber>(Projection.DecimationMin));
		Decimation.Add(MakeShared<FJsonValueNumber>(Projection.DecimationMax));
		Result->SetArrayField(TEXT("decimation"), Decimation);
		Result->SetStringField(TEXT("run_seed"), LexToString(Projection.RunSeed));
		Result->SetStringField(TEXT("world_map"), Projection.WorldMap);
		TSharedRef<FJsonObject> Environment = ProviderJson(Projection.EnvironmentId, Projection.EnvironmentConfig);
		if (Projection.TerrainConfig.NumLevels > 0)
		{
			Environment->SetObjectField(TEXT("terrain"), TerrainJson(Projection.TerrainConfig));
		}
		Result->SetObjectField(TEXT("environment"), Environment);
		Result->SetObjectField(TEXT("robot"), ProviderJson(Projection.RobotId, Projection.RobotConfig));
		if (!Projection.Inputs.IsEmpty())
		{
			Result->SetObjectField(TEXT("input_contract"), WriteUERLInputContract(Projection.Inputs));
		}
		return Result;
	}

	bool WorkerIdentityHash(const FUERLWorkerProjection& Projection, FString& OutHash, FString& OutError)
	{
		TSharedRef<FJsonObject> Identity = WorkerConfigJson(Projection);
		const TSharedPtr<FJsonObject>* Robot = nullptr;
		if (Identity->TryGetObjectField(TEXT("robot"), Robot) && Robot && Robot->IsValid())
		{
			(*Robot)->RemoveField(TEXT("actuators"));
			(*Robot)->RemoveField(TEXT("reset_bindings"));
		}
		return UERLJson::CanonicalSha256(Identity, OutHash, OutError);
	}

	bool ParseFields(
		const TArray<TSharedPtr<FJsonValue>>& Values,
		TArray<FUERLFieldDescriptor>& Out,
		bool bAction,
		FString& OutError)
	{
		Out.Reset();
		TSet<FName> Names;
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			FUERLFieldDescriptor Field;
			if (!Value.IsValid() || Value->Type != EJson::Object
				|| !ParseField(Value->AsObject(), Field, bAction, OutError)
				|| Names.Contains(Field.Name))
			{
				if (OutError.IsEmpty()) { OutError = TEXT("duplicate or invalid field descriptor"); }
				return false;
			}
			Names.Add(Field.Name);
			Out.Add(MoveTemp(Field));
		}
		if (Out.IsEmpty()) { OutError = TEXT("schema must contain at least one field"); return false; }
		return true;
	}

	bool PackRequestedFields(
		const TArray<FUERLFieldDescriptor>& Requested,
		TArray<FUERLBatchFieldBinding>& Out,
		int32& OutWidth,
		FString& OutError)
	{
		Out.Reset();
		OutWidth = 0;
		for (const FUERLFieldDescriptor& Field : Requested)
		{
			if (!Field.IsValid())
			{
				OutError = FString::Printf(TEXT("requested field '%s' is invalid"), *Field.Name.ToString());
				return false;
			}
			FUERLBatchFieldBinding& Binding = Out.AddDefaulted_GetRef();
			Binding.Field = Field;
			Binding.Column = OutWidth;
			OutWidth += Field.Width;
		}
		return !Out.IsEmpty();
	}

	TSharedRef<FJsonValue> DescriptorArray(const TArray<FUERLFieldDescriptor>& Fields)
	{
		TArray<FUERLBatchFieldBinding> Bindings;
		int32 Column = 0;
		for (const FUERLFieldDescriptor& Field : Fields)
		{
			FUERLBatchFieldBinding& Binding = Bindings.AddDefaulted_GetRef();
			Binding.Field = Field;
			Binding.Column = Column;
			Column += Field.Width;
		}
		return FieldArray(Bindings);
	}

	TSharedRef<FJsonObject> ProtocolVersionJson()
	{
		TSharedRef<FJsonObject> Version = MakeShared<FJsonObject>();
		Version->SetNumberField(TEXT("major"), Major);
		Version->SetNumberField(TEXT("minor"), Minor);
		return Version;
	}

	TSharedRef<FJsonObject> BuildIdentityJson()
	{
		TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
		Identity->SetStringField(TEXT("ue_version"), FEngineVersion::Current().ToString());
		Identity->SetStringField(TEXT("project_name"), FApp::GetProjectName());
		FString ProjectVersion = TEXT("0.0.0");
		if (GConfig)
		{
			GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"),
				TEXT("ProjectVersion"), ProjectVersion, GGameIni);
		}
		Identity->SetStringField(TEXT("project_version"), ProjectVersion);
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UERLEngine"));
		Identity->SetStringField(TEXT("plugin_version"), Plugin ? Plugin->GetDescriptor().VersionName : TEXT("unknown"));
		const FString BuildId = FApp::GetBuildVersion();
		Identity->SetStringField(TEXT("build_id"), BuildId.IsEmpty() ? FEngineVersion::Current().ToString() : BuildId);
		return Identity;
	}

	TSharedRef<FJsonObject> TopologyJson(const FUERLRobotTopology& Topology)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> BodyNames;
		TArray<TSharedPtr<FJsonValue>> BodyMotionTypes;
		for (int32 BodyIndex = 0; BodyIndex < Topology.BodyNames.Num(); ++BodyIndex)
		{
			BodyNames.Add(MakeShared<FJsonValueString>(Topology.BodyNames[BodyIndex].ToString()));
			checkf(
				Topology.BodyMotionTypes.IsValidIndex(BodyIndex),
				TEXT("robot topology body motion metadata is incomplete at body index %d"), BodyIndex);
			const EUERLBodyMotionType MotionType = Topology.BodyMotionTypes[BodyIndex];
			BodyMotionTypes.Add(MakeShared<FJsonValueString>(
				MotionType == EUERLBodyMotionType::Kinematic ? TEXT("kinematic") : TEXT("simulated")));
		}
		Result->SetArrayField(TEXT("body_names"), BodyNames);
		Result->SetArrayField(TEXT("body_motion_types"), BodyMotionTypes);
		Result->SetNumberField(TEXT("root_body_index"), Topology.RootBodyIndex);
		Result->SetBoolField(TEXT("fixed_base"), Topology.bFixedBase);
		TArray<TSharedPtr<FJsonValue>> Joints;
		for (const FUERLJointTopology& Joint : Topology.Joints)
		{
			checkf(
				Joint.ChildFrame.bValid && Joint.ParentFrame.bValid,
				TEXT("robot topology joint '%s' is missing reflected constraint frames"),
				*Joint.Name.ToString());
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Joint.Name.ToString());
			Item->SetNumberField(TEXT("parent_body_index"), Joint.ParentBodyIndex);
			Item->SetNumberField(TEXT("child_body_index"), Joint.ChildBodyIndex);
			Item->SetNumberField(TEXT("degrees_of_freedom"), Joint.DegreesOfFreedom);
			Item->SetStringField(TEXT("coordinate"), UERLJointCoordinateName(Joint.Coordinate));
			Item->SetStringField(TEXT("coordinate_type"), UERLJointCoordinateTypeName(Joint.CoordinateType));
			Item->SetStringField(TEXT("unit"), UERLJointCoordinateUnit(Joint.CoordinateType));
			Item->SetNumberField(TEXT("default_position"), Joint.DefaultPosition);
			const auto FrameJson = [](const FUERLConstraintFrame& Frame)
			{
				TSharedRef<FJsonObject> FrameObj = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Position;
				Position.Add(MakeShared<FJsonValueNumber>(Frame.PositionMetres.X));
				Position.Add(MakeShared<FJsonValueNumber>(Frame.PositionMetres.Y));
				Position.Add(MakeShared<FJsonValueNumber>(Frame.PositionMetres.Z));
				FrameObj->SetArrayField(TEXT("position_metres"), Position);
				TArray<TSharedPtr<FJsonValue>> Rotation;
				Rotation.Add(MakeShared<FJsonValueNumber>(Frame.Rotation.X));
				Rotation.Add(MakeShared<FJsonValueNumber>(Frame.Rotation.Y));
				Rotation.Add(MakeShared<FJsonValueNumber>(Frame.Rotation.Z));
				Rotation.Add(MakeShared<FJsonValueNumber>(Frame.Rotation.W));
				FrameObj->SetArrayField(TEXT("rotation_xyzw"), Rotation);
				return FrameObj;
			};
			Item->SetObjectField(TEXT("child_frame"), FrameJson(Joint.ChildFrame));
			Item->SetObjectField(TEXT("parent_frame"), FrameJson(Joint.ParentFrame));
			if (Joint.bHasPositionLimit)
			{
				Item->SetNumberField(TEXT("lower_limit"), Joint.LowerLimit);
				Item->SetNumberField(TEXT("upper_limit"), Joint.UpperLimit);
			}
			else
			{
				Item->SetField(TEXT("lower_limit"), MakeShared<FJsonValueNull>());
				Item->SetField(TEXT("upper_limit"), MakeShared<FJsonValueNull>());
			}
			Joints.Add(MakeShared<FJsonValueObject>(Item));
		}
		Result->SetArrayField(TEXT("joints"), Joints);
		return Result;
	}

	TArray<uint8> SerializeJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Text;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
		FJsonSerializer::Serialize(Object, Writer);
		return UERLJson::ToUtf8(Text);
	}

	FString WorkerErrorCode(int32 Code)
	{
		switch (Code)
		{
		case UERLBridgeError::ConfigRejected:
		case UERLBridgeError::PhysicsInvalid: return TEXT("CONFIG_REJECTED");
		case UERLBridgeError::ProtocolViolation: return TEXT("PAYLOAD_INVALID");
		case UERLBridgeError::TransportFailure: return TEXT("TIMEOUT");
		case UERLBridgeError::WorkerFatal: return TEXT("WORKER_FATAL");
		default: return TEXT("STATE_VIOLATION");
		}
	}
}
