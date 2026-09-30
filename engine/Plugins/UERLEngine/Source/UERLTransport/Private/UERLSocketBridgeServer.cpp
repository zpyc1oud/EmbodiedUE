#include "UERLBridgeServer.h"

#include "UERLBatchLayout.h"
#include "UERLJson.h"
#include "UERLProtocol.h"
#include "UERLTransportAdapter.h"
#include "UERLTransportLog.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/Archive.h"

namespace
{
	using namespace UERLProtocol;
	using namespace UERLBatchLayout;

	enum class ESessionState : uint8
	{
		Connected,
		Negotiated,
		WaitingReady,
		Ready,
		Closing,
		Failed,
	};

	const TCHAR* StateName(ESessionState State)
	{
		switch (State)
		{
		case ESessionState::Connected: return TEXT("connected");
		case ESessionState::Negotiated: return TEXT("negotiated");
		case ESessionState::WaitingReady: return TEXT("waiting_ready_ack");
		case ESessionState::Ready: return TEXT("ready");
		case ESessionState::Closing: return TEXT("closing");
		default: return TEXT("failed");
		}
	}

	bool CheckKeys(const TSharedPtr<FJsonObject>& Object, const TArray<FString>& Required,
		const TArray<FString>& Optional, FString& OutError)
	{
		if (!Object.IsValid()) { OutError = TEXT("required JSON object is missing"); return false; }
		for (const FString& Key : Required)
		{
			if (!Object->HasField(Key)) { OutError = FString::Printf(TEXT("missing required key '%s'"), *Key); return false; }
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

	bool ReadInt(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key, int32 Min, int32 Max,
		int32& OutValue, FString& OutError)
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
			if (!FChar::IsDigit(Character)) { OutError = FString::Printf(TEXT("'%s' must be canonical u64dec"), Key); return false; }
		}
		const uint64 Value = FCString::Strtoui64(*Text, nullptr, 10);
		if (LexToString(Value) != Text) { OutError = FString::Printf(TEXT("'%s' is outside uint64"), Key); return false; }
		OutValue = Value;
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
		if (!CheckKeys(Object,
			{ TEXT("level"), TEXT("primitive"), TEXT("seed"), TEXT("platform_width"), TEXT("params") },
			{}, OutError))
		{
			return false;
		}
		FString Primitive;
		const TSharedPtr<FJsonObject>* Params = nullptr;
		if (!ReadInt(Object, TEXT("level"), 0, 65535, Out.Level, OutError)
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
		if (!CheckKeys(Object,
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
		if (!ReadInt(Object, TEXT("num_levels"), 1, 65535, Out.NumLevels, OutError)
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
			if (!Value.IsValid() || Value->Type != EJson::Number || !FMath::IsFinite(Value->AsNumber()) || Value->AsNumber() <= 0.0)
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
			if (!Value.IsValid() || Value->Type != EJson::Object || !ParseTerrainTier(Value->AsObject(), Tier, OutError))
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

	bool ParseActuatorProjection(const TArray<TSharedPtr<FJsonValue>>& Values,
		TArray<FUERLActuatorConfig>& Out, FString& OutError)
	{
		Out.Reset();
		for (int32 Position = 0; Position < Values.Num(); ++Position)
		{
			const TSharedPtr<FJsonObject> Item = Values[Position].IsValid() && Values[Position]->Type == EJson::Object
				? Values[Position]->AsObject() : nullptr;
			if (!CheckKeys(Item,
				{ TEXT("index"), TEXT("joint_index"), TEXT("joint"), TEXT("coordinate"),
				  TEXT("coordinate_type"), TEXT("unit"), TEXT("target_mode"), TEXT("stiffness"),
				  TEXT("damping"), TEXT("effort_limit"), TEXT("default_pos") }, {}, OutError))
			{
				return false;
			}
			FUERLActuatorConfig& Binding = Out.AddDefaulted_GetRef();
			FString JointName;
			if (!ReadInt(Item, TEXT("index"), 0, 65535, Binding.Index, OutError)
				|| !ReadInt(Item, TEXT("joint_index"), 0, 65535, Binding.JointIndex, OutError)
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
			// FName has no writable string reference; read the field once more for clarity.
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

	bool ParseResetProjection(const TArray<TSharedPtr<FJsonValue>>& Values,
		TArray<FUERLResetBinding>& Out, FString& OutError)
	{
		Out.Reset();
		for (int32 Position = 0; Position < Values.Num(); ++Position)
		{
			const TSharedPtr<FJsonObject> Item = Values[Position].IsValid() && Values[Position]->Type == EJson::Object
				? Values[Position]->AsObject() : nullptr;
			if (!CheckKeys(Item,
				{ TEXT("index"), TEXT("name"), TEXT("target_type"), TEXT("joint_index"), TEXT("component_index"),
				  TEXT("body_index"), TEXT("coordinate"), TEXT("unit") }, {}, OutError))
			{
				return false;
			}
			FUERLResetBinding& Binding = Out.AddDefaulted_GetRef();
			FString Name;
			if (!ReadInt(Item, TEXT("index"), 0, 65535, Binding.Index, OutError)
				|| !Item->TryGetStringField(TEXT("name"), Name)
				|| !Item->TryGetStringField(TEXT("target_type"), Binding.TargetType)
				|| !ReadInt(Item, TEXT("joint_index"), -1, 65535, Binding.JointIndex, OutError)
				|| !ReadInt(Item, TEXT("component_index"), 0, 65535, Binding.ComponentIndex, OutError)
				|| !ReadInt(Item, TEXT("body_index"), -1, 65535, Binding.BodyIndex, OutError)
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

	bool ParseProvider(const TSharedPtr<FJsonObject>& Object, FName& OutId,
		FUERLProviderConfig& OutConfig, bool bAllowTerrain, bool bAllowAssetPath,
		bool bAllowRobotProjection, FString& OutError)
	{
		TArray<FString> Optional = { TEXT("extensions") };
		if (bAllowTerrain) { Optional.Add(TEXT("terrain")); }
		if (bAllowAssetPath) { Optional.Add(TEXT("asset_path")); }
		if (bAllowRobotProjection)
		{
			Optional.Add(TEXT("actuators"));
			Optional.Add(TEXT("reset_bindings"));
		}
		if (!CheckKeys(Object, { TEXT("id"), TEXT("scalars") }, Optional, OutError))
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
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number || !FMath::IsFinite(Pair.Value->AsNumber()))
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

	bool ParseWorkerConfig(const TSharedPtr<FJsonObject>& Object, FUERLWorkerProjection& Out,
		FString& OutError)
	{
		if (!CheckKeys(Object,
			{ TEXT("num_slots"), TEXT("physics_dt"), TEXT("decimation"), TEXT("run_seed"),
			  TEXT("world_map"), TEXT("environment"), TEXT("robot") }, { TEXT("extensions") }, OutError))
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

	TSharedRef<FJsonObject> ProviderJson(FName Id, const FUERLProviderConfig& Config)
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("id"), Id.ToString());
		TSharedRef<FJsonObject> Scalars = MakeShared<FJsonObject>();
		for (const TPair<FName, double>& Pair : Config.Scalars) { Scalars->SetNumberField(Pair.Key.ToString(), Pair.Value); }
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
				TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
				TArray<TSharedPtr<FJsonValue>> Position;
				Position.Add(MakeShared<FJsonValueNumber>(Frame.PositionMetres.X));
				Position.Add(MakeShared<FJsonValueNumber>(Frame.PositionMetres.Y));
				Position.Add(MakeShared<FJsonValueNumber>(Frame.PositionMetres.Z));
				Result->SetArrayField(TEXT("position_metres"), Position);
				TArray<TSharedPtr<FJsonValue>> Rotation;
				Rotation.Add(MakeShared<FJsonValueNumber>(Frame.Rotation.X));
				Rotation.Add(MakeShared<FJsonValueNumber>(Frame.Rotation.Y));
				Rotation.Add(MakeShared<FJsonValueNumber>(Frame.Rotation.Z));
				Rotation.Add(MakeShared<FJsonValueNumber>(Frame.Rotation.W));
				Result->SetArrayField(TEXT("rotation_xyzw"), Rotation);
				return Result;
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
		if (!CheckKeys(Extensions,
			{ TEXT("body_index"), TEXT("body_name"), TEXT("observation_type") },
			{ TEXT("joint_index"), TEXT("joint_name"), TEXT("target"), TEXT("target_kind"), TEXT("coordinate"), TEXT("coordinate_type") }, OutError))
		{
			return false;
		}
		FString BodyName, ObservationType;
		if (!Extensions->TryGetStringField(TEXT("body_name"), BodyName) || BodyName.IsEmpty()
			|| !Extensions->TryGetStringField(TEXT("observation_type"), ObservationType)
			|| !ParseObservationType(ObservationType, Out.Observation.Type))
		{
			OutError = TEXT("observation extensions contain an unknown body or observation type");
			return false;
		}
		Out.Observation.BodyName = FName(*BodyName);
		if (!ReadInt(Extensions, TEXT("body_index"), 0, 65535, Out.Observation.BodyIndex, OutError))
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
			if (!ReadInt(Extensions, TEXT("joint_index"), 0, 65535, Out.Observation.JointIndex, OutError)
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
		if (!CheckKeys(Extensions, { TEXT("columns") }, {}, OutError)) { return false; }
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
			if (!CheckKeys(Column,
				{ TEXT("index"), TEXT("joint"), TEXT("joint_index"), TEXT("coordinate_type"),
				  TEXT("unit"), TEXT("target_mode") }, {}, OutError))
			{
				return false;
			}
			FUERLActuatorColumnDescriptor& Item = Out.ActuatorColumns.AddDefaulted_GetRef();
			FString Joint, CoordinateType;
			if (!ReadInt(Column, TEXT("index"), 0, 65535, Item.Index, OutError)
				|| !Column->TryGetStringField(TEXT("joint"), Joint)
				|| !ReadInt(Column, TEXT("joint_index"), 0, 65535, Item.JointIndex, OutError)
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
		if (!CheckKeys(Object,
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

	bool ParseFields(const TArray<TSharedPtr<FJsonValue>>& Values, TArray<FUERLFieldDescriptor>& Out,
		bool bAction, FString& OutError)
	{
		Out.Reset();
		TSet<FName> Names;
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			FUERLFieldDescriptor Field;
			if (!Value.IsValid() || Value->Type != EJson::Object || !ParseField(Value->AsObject(), Field, bAction, OutError)
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

	bool SameField(const FUERLFieldDescriptor& A, const FUERLFieldDescriptor& B)
	{
		return A.Name == B.Name && A.DType == B.DType && A.Shape == B.Shape && A.Unit == B.Unit
			&& A.CoordinateFrame == B.CoordinateFrame && A.Semantic == B.Semantic && A.Source == B.Source
			&& A.Observation == B.Observation && A.ActuatorColumns == B.ActuatorColumns;
	}

	bool SelectFields(const TArray<FUERLFieldDescriptor>& Requested, const TArray<FUERLFieldDescriptor>& Available,
		TArray<FUERLBatchFieldBinding>& Out, int32& OutWidth, bool bAllowObservationNameAlias, FString& OutError)
	{
		Out.Reset();
		OutWidth = 0;
		for (const FUERLFieldDescriptor& Field : Requested)
		{
			const FUERLFieldDescriptor* Offered = Available.FindByPredicate(
				[&Field](const FUERLFieldDescriptor& Candidate) { return Candidate.Name == Field.Name; });
			if (!Offered && bAllowObservationNameAlias && Field.Observation.Type != EUERLObservationType::None)
			{
				Offered = Available.FindByPredicate(
					[&Field](const FUERLFieldDescriptor& Candidate)
					{
						return Candidate.Observation == Field.Observation
							&& Candidate.DType == Field.DType
							&& Candidate.Shape == Field.Shape
							&& Candidate.Width == Field.Width
							&& Candidate.Unit == Field.Unit
							&& Candidate.CoordinateFrame == Field.CoordinateFrame
							&& Candidate.Semantic == Field.Semantic
							&& Candidate.Source == Field.Source;
					});
			}
			const bool bSameMetadata = Offered && Field.DType == Offered->DType
				&& Field.Shape == Offered->Shape && Field.Width == Offered->Width
				&& Field.Unit == Offered->Unit && Field.CoordinateFrame == Offered->CoordinateFrame
				&& Field.Semantic == Offered->Semantic && Field.Source == Offered->Source
				&& Field.Observation == Offered->Observation
				&& Field.ActuatorColumns == Offered->ActuatorColumns;
			if (!Offered || (!bAllowObservationNameAlias && !SameField(Field, *Offered))
				|| (bAllowObservationNameAlias && !bSameMetadata))
			{
				OutError = FString::Printf(TEXT("requested field '%s' is unavailable or metadata differs"), *Field.Name.ToString());
				return false;
			}
			FUERLBatchFieldBinding& Binding = Out.AddDefaulted_GetRef();
			Binding.Field = Field;
			Binding.Column = OutWidth;
			OutWidth += Offered->Width;
		}
		return true;
	}

	bool PackRequestedFields(const TArray<FUERLFieldDescriptor>& Requested,
		TArray<FUERLBatchFieldBinding>& Out, int32& OutWidth, FString& OutError)
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

class FUERLBridgeServer::FImpl final : public FRunnable
{
public:
	~FImpl() override { Shutdown(); }

	bool Start(
		const FUERLBridgeServerConfig& InConfig,
		const FUERLBridgeCapabilities& InCapabilities,
		IBridgeRequestHandler& InHandler,
		FString& OutError)
	{
		if (Thread) { OutError = TEXT("SocketBridge is already running"); return false; }
		if (!InConfig.IsValid()) { OutError = TEXT("invalid SocketBridge bootstrap configuration"); return false; }
		Config = InConfig;
		Capabilities = InCapabilities;
		Handler = &InHandler;
		State = ESessionState::Connected;
		Session = FGuid();
		ExpectedSequence = 1;
		StepCount = 0;
		SelectedSchema = FUERLBatchSchema();
		bDescribePrepared = false;
		PreparedProjection = FUERLWorkerProjection();
		PreparedActionFields.Reset();
		PreparedStateFields.Reset();
		PreparedTopology = FUERLRobotTopology();
		Layouts = FSet();
		Actions.Reset();
		States.Reset();
		Faults.Reset();
		Episodes.Reset();
		if (!Config.PerformancePath.IsEmpty())
		{
			PerformanceLog.Reset(IFileManager::Get().CreateFileWriter(
				*Config.PerformancePath, FILEWRITE_Append));
			if (!PerformanceLog)
			{
				OutError = FString::Printf(TEXT("failed to open performance log '%s'"), *Config.PerformancePath);
				return false;
			}
		}
		Transport = MakeUERLSocketTransport();
		if (!Transport || !Transport->Listen(Config.Port, OutError)) { CloseTransport(); return false; }
		UE_LOG(LogUERLTransport, Display, TEXT("[FLOW] SocketBridge listening 127.0.0.1:%d"), Config.Port);
		bStopRequested = false;
		bGracefulShutdown = false;
		bTerminalErrorResponseSent = false;
		Thread = FRunnableThread::Create(this, TEXT("UERLSocketBridge"), 0, TPri_AboveNormal);
		if (!Thread) { OutError = TEXT("failed to create SocketBridge IO thread"); CloseTransport(); return false; }
		return true;
	}

	virtual void Stop() override { RequestStop(); }

	void Shutdown()
	{
		RequestStop();
		if (Thread)
		{
			FRunnableThread* Completed = Thread;
			Thread = nullptr;
			Completed->WaitForCompletion();
			delete Completed;
		}
		CloseTransport();
		if (PerformanceLog)
		{
			PerformanceLog->Flush();
			PerformanceLog.Reset();
		}
		Handler = nullptr;
	}

	bool IsRunning() const { return Thread != nullptr && !bStopRequested; }

	virtual uint32 Run() override
	{
		FString AcceptError;
		if (!Transport || !Transport->Accept(Config.HandshakeTimeoutMs, bStopRequested, AcceptError))
		{
			if (!bStopRequested) { UE_LOG(LogUERLTransport, Error, TEXT("[VERIFY] SocketBridge accept failed: %s"), *AcceptError); }
			CloseTransport();
			if (!bStopRequested && Handler) { Handler->NotifyBridgeDisconnected(); }
			return 1;
		}
		UE_LOG(LogUERLTransport, Display, TEXT("[FLOW] SocketBridge client connected"));

		while (!bStopRequested && State != ESessionState::Failed && !bGracefulShutdown)
		{
			FFrameHeader Header;
			TArray<uint8> Payload;
			FString Error;
			if (!ReceiveFrame(Header, Payload, Error))
			{
				if (!bStopRequested) { UE_LOG(LogUERLTransport, Error, TEXT("[VERIFY] SocketBridge receive failed: %s"), *Error); }
				break;
			}
			if (!ProcessFrame(Header, Payload)) { break; }
		}

		if (bTerminalErrorResponseSent && !bStopRequested && Transport)
		{
			Transport->WaitForPeerClose(FMath::Min(Config.RequestTimeoutMs, 100u), bStopRequested);
		}
		CloseTransport();
		if (Handler)
		{
			if (bGracefulShutdown) { Handler->NotifyBridgeShutdownComplete(); }
			else { Handler->NotifyBridgeDisconnected(); }
		}
		return bGracefulShutdown || bStopRequested ? 0 : 1;
	}

private:
	void RequestStop()
	{
		bStopRequested = true;
		if (Transport) { Transport->Interrupt(); }
	}

	void CloseTransport()
	{
		if (Transport) { Transport->Close(); Transport.Reset(); }
	}

	bool ReceiveFrame(FFrameHeader& OutHeader, TArray<uint8>& OutPayload, FString& OutError)
	{
		uint8 HeaderBytes[FrameHeaderSize];
		const uint32 Timeout = State == ESessionState::Connected ? Config.HandshakeTimeoutMs : Config.RequestTimeoutMs;
		ActiveDeadlineSeconds = FPlatformTime::Seconds() + Timeout / 1000.0;
		if (!Transport || !Transport->ReadExact(HeaderBytes, FrameHeaderSize, ActiveDeadlineSeconds, bStopRequested))
		{
			OutError = TEXT("deadline, disconnect, or partial header");
			return false;
		}
		if (!FFrameHeader::Decode(HeaderBytes, OutHeader, OutError)) { return false; }
		if (State == ESessionState::Connected && IsUuidV4(OutHeader.Session)) { Session = OutHeader.Session; }
		FString Code;
		if (!ValidateHeaderBeforePayload(OutHeader, Code, OutError))
		{
			Fail(Code, OutError, OutHeader.Sequence);
			return false;
		}
		OutPayload.SetNumUninitialized(OutHeader.PayloadLength);
		if (OutHeader.PayloadLength > 0
			&& (!Transport || !Transport->ReadExact(
				OutPayload.GetData(), OutPayload.Num(), ActiveDeadlineSeconds, bStopRequested)))
		{
			OutError = TEXT("deadline, disconnect, or partial payload");
			return false;
		}
		return true;
	}

	bool ValidateHeaderBeforePayload(const FFrameHeader& Header, FString& OutCode, FString& OutError) const
	{
		EMessage Expected = EMessage::Hello;
		uint64 ExpectedLayout = 0;
		switch (State)
		{
		case ESessionState::Connected: Expected = EMessage::Hello; break;
		case ESessionState::Negotiated:
			Expected = Header.Message == EMessage::Shutdown ? EMessage::Shutdown : EMessage::Initialize;
			break;
		case ESessionState::WaitingReady:
			Expected = Header.Message == EMessage::Shutdown ? EMessage::Shutdown : EMessage::Ready;
			break;
		case ESessionState::Ready:
			Expected = Header.Message;
			if (Header.Message == EMessage::Step) { ExpectedLayout = Layouts.StepAction.LayoutId; }
			else if (Header.Message == EMessage::Reset) { ExpectedLayout = Layouts.ResetRequest.LayoutId; }
			else if (Header.Message != EMessage::Shutdown && Header.Message != EMessage::Event)
			{
				OutCode = TEXT("STATE_VIOLATION");
				OutError = TEXT("message is not legal in the Ready state");
				return false;
			}
			break;
		default:
			OutCode = TEXT("STATE_VIOLATION");
			OutError = TEXT("Session does not accept another request");
			return false;
		}
		if (!ValidateEnvelope(Header, Expected, ExpectedLayout, OutCode, OutError)) { return false; }
		if (Header.Message == EMessage::Step && Header.PayloadLength != Layouts.StepAction.PayloadLength)
		{
			OutCode = TEXT("PAYLOAD_INVALID");
			OutError = TEXT("Step payload length mismatch");
			return false;
		}
		if (Header.Message == EMessage::Reset && Header.PayloadLength != Layouts.ResetRequest.PayloadLength)
		{
			OutCode = TEXT("PAYLOAD_INVALID");
			OutError = TEXT("Reset payload length mismatch");
			return false;
		}
		return true;
	}

	bool SendFrame(EMessage Message, uint16 Flags, uint64 Sequence, uint64 LayoutId, const TArray<uint8>& Payload)
	{
		FFrameHeader Header;
		Header.ProtocolMajor = State == ESessionState::Connected ? 0 : Major;
		Header.ProtocolMinor = State == ESessionState::Connected ? 0 : Minor;
		Header.Message = Message;
		Header.Flags = Flags;
		Header.PayloadLength = Payload.Num();
		Header.Sequence = Sequence;
		Header.Session = Session;
		Header.LayoutId = LayoutId;
		uint8 HeaderBytes[FrameHeaderSize];
		Header.Encode(HeaderBytes);
		return Transport
			&& Transport->WriteExact(HeaderBytes, FrameHeaderSize, ActiveDeadlineSeconds, bStopRequested)
			&& (Payload.IsEmpty() || Transport->WriteExact(
				Payload.GetData(), Payload.Num(), ActiveDeadlineSeconds, bStopRequested));
	}

	uint32 RemainingTimeoutMs() const
	{
		const double RemainingSeconds = ActiveDeadlineSeconds - FPlatformTime::Seconds();
		if (RemainingSeconds <= 0.0) { return 0; }
		return static_cast<uint32>(FMath::Clamp(
			FMath::CeilToDouble(RemainingSeconds * 1000.0), 1.0, static_cast<double>(MAX_uint32)));
	}

	void SubmitWorker(FUERLBridgeRequest& Request) const
	{
		Request.TimeoutMs = RemainingTimeoutMs();
		if (Request.TimeoutMs == 0)
		{
			Request.Fail(UERLBridgeError::TransportFailure, TEXT("request transaction deadline expired"));
			return;
		}
		Handler->SubmitAndWait(Request);
	}

	bool Fail(const FString& Code, const FString& Message, uint64 Sequence)
	{
		TSharedRef<FJsonObject> Error = MakeShared<FJsonObject>();
		Error->SetStringField(TEXT("code"), Code);
		Error->SetStringField(TEXT("phase"), StateName(State));
		Error->SetStringField(TEXT("message"), Message.Left(512));
		Error->SetObjectField(TEXT("details"), MakeShared<FJsonObject>());
		bTerminalErrorResponseSent = SendFrame(
			EMessage::Error, Response | UERLProtocol::Error, Sequence, 0, SerializeJson(Error));
		State = ESessionState::Failed;
		UE_LOG(LogUERLTransport, Error, TEXT("[VERIFY] SocketBridge %s: %s"), *Code, *Message);
		return false;
	}

	bool ValidateEnvelope(const FFrameHeader& Header, EMessage Expected, uint64 ExpectedLayout,
		FString& OutCode, FString& OutError) const
	{
		if (Header.Flags != Request || Header.Message != Expected)
		{
			OutCode = TEXT("STATE_VIOLATION");
			OutError = TEXT("message is not legal in the current protocol state");
			return false;
		}
		if (Header.Sequence != ExpectedSequence)
		{
			OutCode = TEXT("SEQUENCE_MISMATCH");
			OutError = FString::Printf(TEXT("sequence mismatch got=%llu expected=%llu"), Header.Sequence, ExpectedSequence);
			return false;
		}
		if (State == ESessionState::Connected)
		{
			if (Header.ProtocolMajor != 0 || Header.ProtocolMinor != 0)
			{
				OutCode = TEXT("VERSION_UNSUPPORTED");
				OutError = TEXT("Hello requires bootstrap protocol version 0.0");
				return false;
			}
			if (!IsUuidV4(Header.Session))
			{
				OutCode = TEXT("SESSION_MISMATCH");
				OutError = TEXT("Hello requires the client Session UUID v4");
				return false;
			}
		}
		else
		{
			if (Header.ProtocolMajor != Major || Header.ProtocolMinor != Minor)
			{
				OutCode = TEXT("VERSION_UNSUPPORTED");
				OutError = TEXT("protocol version mismatch");
				return false;
			}
			if (Header.Session != Session)
			{
				OutCode = TEXT("SESSION_MISMATCH");
				OutError = TEXT("Session UUID mismatch");
				return false;
			}
		}
		if (Header.LayoutId != ExpectedLayout)
		{
			OutCode = TEXT("PAYLOAD_INVALID");
			OutError = TEXT("layout_id mismatch");
			return false;
		}
		return true;
	}

	bool ProcessFrame(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		if (State == ESessionState::Connected)
		{
			Session = Header.Session;
			return ProcessHello(Header, Payload);
		}
		if (State == ESessionState::Negotiated)
		{
			if (Header.Message == EMessage::Shutdown) { return ProcessShutdown(Header, Payload); }
			return ProcessInitialize(Header, Payload);
		}
		if (State == ESessionState::WaitingReady)
		{
			if (Header.Message == EMessage::Shutdown) { return ProcessShutdown(Header, Payload); }
			return ProcessReady(Header, Payload);
		}
		if (State == ESessionState::Ready)
		{
			if (Header.Message == EMessage::Step) { return ProcessStep(Header, Payload); }
			if (Header.Message == EMessage::Reset) { return ProcessReset(Header, Payload); }
			if (Header.Message == EMessage::Event) { return ProcessEvent(Header, Payload); }
			if (Header.Message == EMessage::Shutdown) { return ProcessShutdown(Header, Payload); }
		}
		return Fail(TEXT("STATE_VIOLATION"), TEXT("message is not legal in the current protocol state"), Header.Sequence);
	}

	bool ProcessHello(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Hello, 0, Code, Error)) { return Fail(Code, Error, Header.Sequence); }
		TSharedPtr<FJsonObject> Json;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !CheckKeys(Json, { TEXT("supported_protocols"), TEXT("client_name"), TEXT("client_version") }, { TEXT("extensions") }, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		const TArray<TSharedPtr<FJsonValue>>* Protocols = nullptr;
		FString ClientName;
		FString ClientVersion;
		if (!Json->TryGetArrayField(TEXT("supported_protocols"), Protocols) || !Protocols
			|| !Json->TryGetStringField(TEXT("client_name"), ClientName) || ClientName.IsEmpty()
			|| !Json->TryGetStringField(TEXT("client_version"), ClientVersion) || ClientVersion.IsEmpty())
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Hello DTO types are invalid"), Header.Sequence);
		}
		bool bSupported = false;
		for (const TSharedPtr<FJsonValue>& Value : *Protocols)
		{
			const TSharedPtr<FJsonObject> Version = Value.IsValid() && Value->Type == EJson::Object
				? Value->AsObject() : nullptr;
			int32 RequestedMajor = 0, MinMinor = 0, MaxMinor = 0;
			if (!CheckKeys(Version, { TEXT("major"), TEXT("min_minor"), TEXT("max_minor") }, {}, Error)
				|| !ReadInt(Version, TEXT("major"), 0, 65535, RequestedMajor, Error)
				|| !ReadInt(Version, TEXT("min_minor"), 0, 65535, MinMinor, Error)
				|| !ReadInt(Version, TEXT("max_minor"), 0, 65535, MaxMinor, Error))
			{
				return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
			}
			bSupported |= RequestedMajor == Major && MinMinor <= Minor && Minor <= MaxMinor;
		}
		if (!bSupported) { return Fail(TEXT("VERSION_UNSUPPORTED"), TEXT("no common protocol version"), Header.Sequence); }

		State = ESessionState::Negotiated;
		++ExpectedSequence;
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetObjectField(TEXT("selected_protocol"), ProtocolVersionJson());
		ResponseJson->SetObjectField(TEXT("build_identity"), BuildIdentityJson());
		TSharedRef<FJsonObject> CapabilitiesJson = MakeShared<FJsonObject>();
		CapabilitiesJson->SetBoolField(TEXT("socket_data_plane"), true);
		CapabilitiesJson->SetBoolField(TEXT("headless"), Capabilities.bHeadless);
		CapabilitiesJson->SetBoolField(TEXT("viewport"), Capabilities.bViewport);
		CapabilitiesJson->SetStringField(TEXT("presentation_mode"), Capabilities.ActivePresentationMode);
		ResponseJson->SetObjectField(TEXT("capabilities"), CapabilitiesJson);
		TSharedRef<FJsonObject> Limits = MakeShared<FJsonObject>();
		Limits->SetNumberField(TEXT("max_control_payload_bytes"), MaxControlPayloadBytes);
		Limits->SetNumberField(TEXT("max_data_payload_bytes"), MaxDataPayloadBytes);
		Limits->SetNumberField(TEXT("max_slots"), 65536);
		ResponseJson->SetObjectField(TEXT("limits"), Limits);
		return SendFrame(EMessage::Hello, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
	}

	bool ProcessInitialize(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Initialize, 0, Code, Error)) { return Fail(Code, Error, Header.Sequence); }
		TSharedPtr<FJsonObject> Json;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !CheckKeys(Json,
				{ TEXT("task_id"), TEXT("task_version"), TEXT("worker_config"), TEXT("worker_config_hash"),
				  TEXT("state_requirements"), TEXT("action_schema"), TEXT("phase") }, { TEXT("extensions") }, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		FString TaskId, TaskVersion, ConfigHash, Phase;
		const TSharedPtr<FJsonObject>* WorkerConfig = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* StateRequirements = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* ActionSchema = nullptr;
		if (!Json->TryGetStringField(TEXT("task_id"), TaskId) || TaskId.IsEmpty()
			|| !Json->TryGetStringField(TEXT("task_version"), TaskVersion) || TaskVersion.IsEmpty()
			|| !Json->TryGetObjectField(TEXT("worker_config"), WorkerConfig) || !WorkerConfig
			|| !Json->TryGetStringField(TEXT("worker_config_hash"), ConfigHash) || !IsHex32(ConfigHash)
			|| !Json->TryGetArrayField(TEXT("state_requirements"), StateRequirements) || !StateRequirements
			|| !Json->TryGetArrayField(TEXT("action_schema"), ActionSchema) || !ActionSchema
			|| !Json->TryGetStringField(TEXT("phase"), Phase))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Initialize DTO types are invalid"), Header.Sequence);
		}
		if (Phase != TEXT("describe") && Phase != TEXT("commit"))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Initialize phase must be describe or commit"), Header.Sequence);
		}
		const bool bDescribe = Phase == TEXT("describe");
		FString ComputedHash;
		if (!UERLJson::CanonicalSha256(*WorkerConfig, ComputedHash, Error) || ComputedHash != ConfigHash)
		{
			return Fail(TEXT("CONFIG_REJECTED"), TEXT("worker_config_hash mismatch"), Header.Sequence);
		}

		FUERLWorkerProjection Projection;
		TArray<FUERLFieldDescriptor> RequestedStates;
		TArray<FUERLFieldDescriptor> RequestedActions;
		if (!ParseWorkerConfig(*WorkerConfig, Projection, Error)) { return Fail(TEXT("CONFIG_REJECTED"), Error, Header.Sequence); }
		if (!bDescribe && (!ParseFields(*StateRequirements, RequestedStates, false, Error)
			|| !ParseFields(*ActionSchema, RequestedActions, true, Error)))
		{
			return Fail(TEXT("SCHEMA_MISMATCH"), Error, Header.Sequence);
		}

		FUERLBridgeRequest Prepare;
		if (bDescribe)
		{
			if (bDescribePrepared)
			{
				return Fail(TEXT("STATE_VIOLATION"), TEXT("Initialize describe was already completed"), Header.Sequence);
			}
			Prepare.Type = EUERLBridgeRequestType::PrepareInitialize;
			Prepare.Sequence = Header.Sequence;
			Prepare.Projection = Projection;
			SubmitWorker(Prepare);
			if (!Prepare.bOk) { return Fail(WorkerErrorCode(Prepare.ErrorCode), Prepare.ErrorMessage, Header.Sequence); }
			bDescribePrepared = true;
			PreparedProjection = Prepare.Projection;
			if (!WorkerIdentityHash(PreparedProjection, PreparedIdentityHash, Error))
			{
				return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
			}
			PreparedActionFields = Prepare.AvailableActionFields;
			PreparedStateFields = Prepare.AvailableStateFields;
			PreparedTopology = Prepare.AvailableTopology;
			TSharedRef<FJsonObject> EffectiveConfig = WorkerConfigJson(Prepare.Projection);
			FString EffectiveHash;
			if (!UERLJson::CanonicalSha256(EffectiveConfig, EffectiveHash, Error))
			{
				return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
			}
			TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
			ResponseJson->SetStringField(TEXT("phase"), TEXT("describe"));
			ResponseJson->SetObjectField(TEXT("selected_protocol"), ProtocolVersionJson());
			ResponseJson->SetObjectField(TEXT("build_identity"), BuildIdentityJson());
			ResponseJson->SetObjectField(TEXT("effective_worker_config"), EffectiveConfig);
			ResponseJson->SetStringField(TEXT("effective_worker_config_hash"), EffectiveHash);
			ResponseJson->SetField(TEXT("available_state_schema"), DescriptorArray(PreparedStateFields));
			ResponseJson->SetField(TEXT("available_action_schema"), DescriptorArray(PreparedActionFields));
			ResponseJson->SetObjectField(TEXT("topology"), TopologyJson(PreparedTopology));
			ResponseJson->SetStringField(TEXT("seed_derivation_version"), TEXT("uerl.seed.v1"));
			++ExpectedSequence;
			return SendFrame(EMessage::Initialize, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
		}

		SelectedSchema = FUERLBatchSchema();
		if (bDescribePrepared)
		{
			FString CommitIdentityHash;
			if (!WorkerIdentityHash(Projection, CommitIdentityHash, Error)
				|| CommitIdentityHash != PreparedIdentityHash)
			{
				return Fail(TEXT("CONFIG_REJECTED"), TEXT("Initialize commit does not match the described Worker"), Header.Sequence);
			}
			if (!PackRequestedFields(RequestedActions, SelectedSchema.ActionFields, SelectedSchema.ActionWidth, Error)
				|| !PackRequestedFields(RequestedStates, SelectedSchema.StateFields, SelectedSchema.StateWidth, Error))
			{
				FUERLBridgeRequest Abort;
				Abort.Type = EUERLBridgeRequestType::AbortInitialize;
				SubmitWorker(Abort);
				bDescribePrepared = false;
				return Fail(TEXT("SCHEMA_MISMATCH"), Error, Header.Sequence);
			}
		}
		else
		{
			return Fail(TEXT("STATE_VIOLATION"), TEXT("Initialize commit requires a preceding describe"), Header.Sequence);
		}
		SelectedSchema.ResetWidth = Projection.RobotConfig.ResetBindings.Num();
		Projection.ActionWidth = SelectedSchema.ActionWidth;
		Projection.StateWidth = SelectedSchema.StateWidth;
		if (!Compile(Projection.NumSlots, SelectedSchema, Layouts, Error))
		{
			FUERLBridgeRequest Abort;
			Abort.Type = EUERLBridgeRequestType::AbortInitialize;
			SubmitWorker(Abort);
			return Fail(TEXT("SCHEMA_MISMATCH"), Error, Header.Sequence);
		}

		Actions.SetNumZeroed(Projection.NumSlots * Projection.ActionWidth);
		States.SetNumZeroed(Projection.NumSlots * Projection.StateWidth);
		Faults.SetNumZeroed(Projection.NumSlots);
		FUERLBridgeRequest Commit;
		Commit.Type = EUERLBridgeRequestType::CommitInitialize;
		Commit.Sequence = Header.Sequence;
		Commit.Projection = Projection;
		Commit.SelectedSchema = SelectedSchema;
		Commit.States = { States.GetData(), Projection.NumSlots, Projection.StateWidth };
		Commit.Faults = { Faults.GetData(), Projection.NumSlots };
		SubmitWorker(Commit);
		if (!Commit.bOk) { return Fail(WorkerErrorCode(Commit.ErrorCode), Commit.ErrorMessage, Header.Sequence); }
		Episodes = Commit.EpisodeIndices;
		EffectiveProjection = Commit.Projection;
		EffectiveProjection.ActionWidth = SelectedSchema.ActionWidth;
		EffectiveProjection.StateWidth = SelectedSchema.StateWidth;

		TSharedRef<FJsonObject> EffectiveConfig = WorkerConfigJson(EffectiveProjection);
		FString EffectiveHash;
		if (!UERLJson::CanonicalSha256(EffectiveConfig, EffectiveHash, Error)) { return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence); }
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetStringField(TEXT("phase"), TEXT("commit"));
		ResponseJson->SetObjectField(TEXT("selected_protocol"), ProtocolVersionJson());
		ResponseJson->SetObjectField(TEXT("build_identity"), BuildIdentityJson());
		ResponseJson->SetObjectField(TEXT("effective_worker_config"), EffectiveConfig);
		ResponseJson->SetStringField(TEXT("effective_worker_config_hash"), EffectiveHash);
		const TArray<FUERLFieldDescriptor>& AvailableActions = Commit.AvailableActionFields;
		const TArray<FUERLFieldDescriptor>& AvailableStates = Commit.AvailableStateFields;
		const FUERLRobotTopology& AvailableTopology = Commit.AvailableTopology;
		ResponseJson->SetField(TEXT("available_state_schema"), DescriptorArray(AvailableStates));
		ResponseJson->SetField(TEXT("available_action_schema"), DescriptorArray(AvailableActions));
		ResponseJson->SetObjectField(TEXT("topology"), TopologyJson(AvailableTopology));
		TSharedRef<FJsonObject> Selected = MakeShared<FJsonObject>();
		Selected->SetField(TEXT("state"), FieldArray(SelectedSchema.StateFields));
		Selected->SetField(TEXT("action"), FieldArray(SelectedSchema.ActionFields));
		FString StateSchemaJson, ActionSchemaJson, StateSchemaHash, ActionSchemaHash;
		UERLJson::Canonicalize(Selected->Values[TEXT("state")], StateSchemaJson, Error);
		UERLJson::Canonicalize(Selected->Values[TEXT("action")], ActionSchemaJson, Error);
		UERLJson::Sha256Utf8(StateSchemaJson, StateSchemaHash);
		UERLJson::Sha256Utf8(ActionSchemaJson, ActionSchemaHash);
		Selected->SetStringField(TEXT("state_schema_hash"), StateSchemaHash);
		Selected->SetStringField(TEXT("action_schema_hash"), ActionSchemaHash);
		ResponseJson->SetObjectField(TEXT("selected_schemas"), Selected);
		TArray<TSharedPtr<FJsonValue>> LayoutValues;
		for (const FLayout* Layout : { &Layouts.InitialState, &Layouts.StepAction, &Layouts.StepResult,
			&Layouts.ResetRequest, &Layouts.ResetResult })
		{
			LayoutValues.Add(MakeShared<FJsonValueObject>(Layout->ToJson()));
		}
		ResponseJson->SetArrayField(TEXT("layouts"), LayoutValues);
		ResponseJson->SetStringField(TEXT("seed_derivation_version"), TEXT("uerl.seed.v1"));

		TArray<uint8> InitialPayload;
		if (!EncodeState(Layouts.InitialState, { States.GetData(), Projection.NumSlots, Projection.StateWidth },
			{ Faults.GetData(), Projection.NumSlots }, Episodes, nullptr, InitialPayload, Error))
		{
			return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
		}
		State = ESessionState::WaitingReady;
		++ExpectedSequence;
		bDescribePrepared = false;
		return SendFrame(EMessage::Initialize, Response | MoreFrames, Header.Sequence, 0, SerializeJson(ResponseJson))
			&& SendFrame(EMessage::InitialState, Response, Header.Sequence, Layouts.InitialState.LayoutId, InitialPayload);
	}

	bool ProcessReady(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Ready, 0, Code, Error)) { return Fail(Code, Error, Header.Sequence); }
		TSharedPtr<FJsonObject> Json;
		FString ManifestHash;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !CheckKeys(Json, { TEXT("manifest_hash") }, { TEXT("extensions") }, Error)
			|| !Json->TryGetStringField(TEXT("manifest_hash"), ManifestHash) || !IsHex32(ManifestHash))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error.IsEmpty() ? TEXT("invalid manifest_hash") : Error, Header.Sequence);
		}
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Ready;
		RequestDto.Sequence = Header.Sequence;
		RequestDto.ManifestHash = ManifestHash;
		SubmitWorker(RequestDto);
		if (!RequestDto.bOk) { return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence); }
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetBoolField(TEXT("ready"), true);
		State = ESessionState::Ready;
		++ExpectedSequence;
		return SendFrame(EMessage::Ready, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
	}

	bool ProcessStep(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		const double ServerStartSeconds = FPlatformTime::Seconds();
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Step, Layouts.StepAction.LayoutId, Code, Error))
		{
			return Fail(Code, Error, Header.Sequence);
		}
		if (Header.PayloadLength != Layouts.StepAction.PayloadLength)
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Step payload length mismatch"), Header.Sequence);
		}
		const double DecodeStartSeconds = FPlatformTime::Seconds();
		if (!DecodeActions(Layouts.StepAction, Payload,
			{ Actions.GetData(), EffectiveProjection.NumSlots, EffectiveProjection.ActionWidth }, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		int32 StepDecimation = 0;
		if (!DecodeStepDecimation(Layouts.StepAction, Payload, StepDecimation, Error)
			|| StepDecimation < EffectiveProjection.DecimationMin
			|| StepDecimation > EffectiveProjection.DecimationMax)
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error.IsEmpty() ? TEXT("Step decimation is outside the configured range") : Error, Header.Sequence);
		}
		const double ActionDecodeSeconds = FPlatformTime::Seconds() - DecodeStartSeconds;
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Step;
		RequestDto.Sequence = Header.Sequence;
		RequestDto.StepDecimation = StepDecimation;
		RequestDto.Actions = { Actions.GetData(), EffectiveProjection.NumSlots, EffectiveProjection.ActionWidth };
		RequestDto.States = { States.GetData(), EffectiveProjection.NumSlots, EffectiveProjection.StateWidth };
		RequestDto.Faults = { Faults.GetData(), EffectiveProjection.NumSlots };
		RequestDto.StepTiming.ActionDecodeSeconds = ActionDecodeSeconds;
		const double WorkerStartSeconds = FPlatformTime::Seconds();
		SubmitWorker(RequestDto);
		const double WorkerWaitSeconds = FPlatformTime::Seconds() - WorkerStartSeconds;
		if (!RequestDto.bOk) { return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence); }
		TArray<uint8> Result;
		const double EncodeStartSeconds = FPlatformTime::Seconds();
		if (!EncodeState(Layouts.StepResult, RequestDto.States, RequestDto.Faults, Episodes, nullptr, Result, Error))
		{
			return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
		}
		const double ResponseEncodeSeconds = FPlatformTime::Seconds() - EncodeStartSeconds;
		++StepCount;
		++ExpectedSequence;
		const double SendStartSeconds = FPlatformTime::Seconds();
		const bool bSent = SendFrame(EMessage::Step, Response, Header.Sequence, Layouts.StepResult.LayoutId, Result);
		const double ResponseSendSeconds = FPlatformTime::Seconds() - SendStartSeconds;
		const double ServerTotalSeconds = FPlatformTime::Seconds() - ServerStartSeconds;
		if (bSent)
		{
			WriteStepTiming(Header.Sequence, RequestDto.StepTiming, WorkerWaitSeconds,
				ResponseEncodeSeconds, ResponseSendSeconds, ServerTotalSeconds);
		}
		return bSent;
	}

	void WriteStepTiming(
		uint64 Sequence,
		const FUERLStepTiming& Timing,
		double WorkerWaitSeconds,
		double ResponseEncodeSeconds,
		double ResponseSendSeconds,
		double ServerTotalSeconds)
	{
		if (!PerformanceLog) { return; }
		const double AccountedWorkerSeconds = Timing.QueueWaitSeconds + Timing.ActionApplySeconds
			+ Timing.PhysicsFrameSeconds + Timing.ContactSampleSeconds + Timing.StateCollectSeconds;
		TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("request_type"), TEXT("step"));
		Record->SetStringField(TEXT("sequence"), LexToString(Sequence));
		Record->SetNumberField(TEXT("action_decode_s"), Timing.ActionDecodeSeconds);
		Record->SetNumberField(TEXT("worker_wait_s"), WorkerWaitSeconds);
		Record->SetNumberField(TEXT("worker_queue_wait_s"), Timing.QueueWaitSeconds);
		Record->SetNumberField(TEXT("action_apply_s"), Timing.ActionApplySeconds);
		Record->SetNumberField(TEXT("action_apply_count"), Timing.ActionApplyCount);
		Record->SetNumberField(TEXT("physics_frame_s"), Timing.PhysicsFrameSeconds);
		Record->SetNumberField(TEXT("contact_sample_s"), Timing.ContactSampleSeconds);
		Record->SetNumberField(TEXT("state_collect_s"), Timing.StateCollectSeconds);
		Record->SetNumberField(TEXT("worker_other_s"),
			FMath::Max(0.0, WorkerWaitSeconds - AccountedWorkerSeconds));
		Record->SetNumberField(TEXT("response_encode_s"), ResponseEncodeSeconds);
		Record->SetNumberField(TEXT("response_send_s"), ResponseSendSeconds);
		Record->SetNumberField(TEXT("server_total_s"), ServerTotalSeconds);
		TArray<uint8> Line = SerializeJson(Record);
		Line.Add(static_cast<uint8>('\n'));
		PerformanceLog->Serialize(Line.GetData(), Line.Num());
	}

	void WriteResetTiming(
		uint64 Sequence,
		int32 ResetRows,
		const FUERLResetTiming& Timing,
		double WorkerWaitSeconds,
		double ResponseEncodeSeconds,
		double ResponseSendSeconds,
		double ServerTotalSeconds)
	{
		if (!PerformanceLog) { return; }
		const double AccountedWorkerSeconds = Timing.QueueWaitSeconds + Timing.ResetSlotsSeconds
			+ Timing.StateCollectSeconds + Timing.ValidateSlotsSeconds + Timing.SafetySeconds
			+ Timing.EpisodeCopySeconds;
		TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("request_type"), TEXT("reset"));
		Record->SetStringField(TEXT("sequence"), LexToString(Sequence));
		Record->SetNumberField(TEXT("reset_rows"), ResetRows);
		Record->SetNumberField(TEXT("request_decode_s"), Timing.RequestDecodeSeconds);
		Record->SetNumberField(TEXT("worker_wait_s"), WorkerWaitSeconds);
		Record->SetNumberField(TEXT("worker_queue_wait_s"), Timing.QueueWaitSeconds);
		Record->SetNumberField(TEXT("reset_slots_s"), Timing.ResetSlotsSeconds);
		Record->SetNumberField(TEXT("state_collect_s"), Timing.StateCollectSeconds);
		Record->SetNumberField(TEXT("validate_slots_s"), Timing.ValidateSlotsSeconds);
		Record->SetNumberField(TEXT("safety_s"), Timing.SafetySeconds);
		Record->SetNumberField(TEXT("episode_copy_s"), Timing.EpisodeCopySeconds);
		Record->SetNumberField(TEXT("worker_other_s"),
			WorkerWaitSeconds - AccountedWorkerSeconds);
		Record->SetNumberField(TEXT("response_encode_s"), ResponseEncodeSeconds);
		Record->SetNumberField(TEXT("response_send_s"), ResponseSendSeconds);
		Record->SetNumberField(TEXT("server_total_s"), ServerTotalSeconds);
		TArray<uint8> Line = SerializeJson(Record);
		Line.Add(static_cast<uint8>('\n'));
		PerformanceLog->Serialize(Line.GetData(), Line.Num());
	}

	bool ProcessReset(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		const double ServerStartSeconds = FPlatformTime::Seconds();
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Reset, Layouts.ResetRequest.LayoutId, Code, Error))
		{
			return Fail(Code, Error, Header.Sequence);
		}
		if (Header.PayloadLength != Layouts.ResetRequest.PayloadLength)
		{
			return Fail(TEXT("PAYLOAD_INVALID"), TEXT("Reset payload length mismatch"), Header.Sequence);
		}
		const double DecodeStartSeconds = FPlatformTime::Seconds();
		TArray<int32> ResetSlots;
		if (!DecodeResetMask(Layouts.ResetRequest, Payload, EffectiveProjection.NumSlots, ResetSlots, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		TArray<uint16> TerrainLevels;
		if (!DecodeTerrainLevels(Layouts.ResetRequest, Payload, EffectiveProjection.NumSlots, TerrainLevels, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		TArray<float> ResetValues;
		if (!DecodeResetValues(Layouts.ResetRequest, Payload, EffectiveProjection.NumSlots, ResetValues, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		const double RequestDecodeSeconds = FPlatformTime::Seconds() - DecodeStartSeconds;
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Reset;
		RequestDto.Sequence = Header.Sequence;
		RequestDto.ResetSlots = ResetSlots;
		RequestDto.TerrainLevels = TerrainLevels;
		RequestDto.ResetValues = MoveTemp(ResetValues);
		RequestDto.States = { States.GetData(), EffectiveProjection.NumSlots, EffectiveProjection.StateWidth };
		RequestDto.Faults = { Faults.GetData(), EffectiveProjection.NumSlots };
		RequestDto.ResetTiming.RequestDecodeSeconds = RequestDecodeSeconds;
		const double WorkerStartSeconds = FPlatformTime::Seconds();
		SubmitWorker(RequestDto);
		const double WorkerWaitSeconds = FPlatformTime::Seconds() - WorkerStartSeconds;
		if (!RequestDto.bOk) { return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence); }
		Episodes = RequestDto.EpisodeIndices;
		TArray<uint8> Result;
		const double EncodeStartSeconds = FPlatformTime::Seconds();
		if (!EncodeState(Layouts.ResetResult, RequestDto.States, RequestDto.Faults, Episodes, &ResetSlots, Result, Error))
		{
			return Fail(TEXT("WORKER_FATAL"), Error, Header.Sequence);
		}
		const double ResponseEncodeSeconds = FPlatformTime::Seconds() - EncodeStartSeconds;
		++ExpectedSequence;
		const double SendStartSeconds = FPlatformTime::Seconds();
		const bool bSent = SendFrame(EMessage::Reset, Response, Header.Sequence, Layouts.ResetResult.LayoutId, Result);
		const double ResponseSendSeconds = FPlatformTime::Seconds() - SendStartSeconds;
		const double ServerTotalSeconds = FPlatformTime::Seconds() - ServerStartSeconds;
		if (bSent)
		{
			WriteResetTiming(Header.Sequence, ResetSlots.Num(), RequestDto.ResetTiming, WorkerWaitSeconds,
				ResponseEncodeSeconds, ResponseSendSeconds, ServerTotalSeconds);
		}
		return bSent;
	}

	bool ProcessEvent(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Event, 0, Code, Error))
		{
			return Fail(Code, Error, Header.Sequence);
		}
		TSharedPtr<FJsonObject> Json;
		FUERLEventBatch Event;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !ParseEvent(Json, EffectiveProjection.NumSlots, Event, Error))
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error, Header.Sequence);
		}
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Event;
		RequestDto.Sequence = Header.Sequence;
		RequestDto.Event = MoveTemp(Event);
		SubmitWorker(RequestDto);
		if (!RequestDto.bOk)
		{
			return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence);
		}
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetBoolField(TEXT("applied"), true);
		ResponseJson->SetStringField(TEXT("kind"), UERLEventKindName(RequestDto.Event.Kind));
		if (RequestDto.Event.Kind == EUERLEventKind::TerrainTierParams)
		{
			TArray<TSharedPtr<FJsonValue>> TerrainLevels;
			for (int32 Row = 0; Row < RequestDto.Event.SlotIds.Num(); ++Row)
			{
				TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
				Item->SetNumberField(TEXT("slot_id"), RequestDto.Event.SlotIds[Row]);
				Item->SetNumberField(TEXT("level"), RequestDto.Event.AppliedTerrainLevels[Row]);
				TerrainLevels.Add(MakeShared<FJsonValueObject>(Item));
			}
			ResponseJson->SetArrayField(TEXT("terrain_levels"), TerrainLevels);
		}
		++ExpectedSequence;
		return SendFrame(EMessage::Event, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
	}

	bool ProcessShutdown(const FFrameHeader& Header, const TArray<uint8>& Payload)
	{
		FString Code, Error;
		if (!ValidateEnvelope(Header, EMessage::Shutdown, 0, Code, Error)) { return Fail(Code, Error, Header.Sequence); }
		TSharedPtr<FJsonObject> Json;
		FString Reason;
		if (!UERLJson::ParseStrictObject(Payload, Json, Error)
			|| !CheckKeys(Json, { TEXT("reason") }, { TEXT("extensions") }, Error)
			|| !Json->TryGetStringField(TEXT("reason"), Reason) || Reason.IsEmpty())
		{
			return Fail(TEXT("PAYLOAD_INVALID"), Error.IsEmpty() ? TEXT("Shutdown reason is required") : Error, Header.Sequence);
		}
		State = ESessionState::Closing;
		FUERLBridgeRequest RequestDto;
		RequestDto.Type = EUERLBridgeRequestType::Shutdown;
		RequestDto.Sequence = Header.Sequence;
		SubmitWorker(RequestDto);
		if (!RequestDto.bOk) { return Fail(WorkerErrorCode(RequestDto.ErrorCode), RequestDto.ErrorMessage, Header.Sequence); }
		Episodes = RequestDto.EpisodeIndices;
		TSharedRef<FJsonObject> ResponseJson = MakeShared<FJsonObject>();
		ResponseJson->SetStringField(TEXT("final_step_count"), LexToString(StepCount));
		TArray<TSharedPtr<FJsonValue>> EpisodeValues;
		for (uint64 Episode : Episodes) { EpisodeValues.Add(MakeShared<FJsonValueString>(LexToString(Episode))); }
		ResponseJson->SetArrayField(TEXT("final_episode_indices"), EpisodeValues);
		if (PerformanceLog) { PerformanceLog->Flush(); }
		const bool bSent = SendFrame(EMessage::Shutdown, Response, Header.Sequence, 0, SerializeJson(ResponseJson));
		bGracefulShutdown = bSent;
		return false;
	}

	FUERLBridgeServerConfig Config;
	FUERLBridgeCapabilities Capabilities;
	IBridgeRequestHandler* Handler = nullptr;
	FRunnableThread* Thread = nullptr;
	TUniquePtr<IUERLTransport> Transport;
	TUniquePtr<FArchive> PerformanceLog;
	FThreadSafeBool bStopRequested{ false };
	bool bGracefulShutdown = false;
	bool bTerminalErrorResponseSent = false;
	ESessionState State = ESessionState::Connected;
	FGuid Session;
	uint64 ExpectedSequence = 1;
	uint64 StepCount = 0;
	bool bDescribePrepared = false;
	FString PreparedIdentityHash;
	FUERLWorkerProjection PreparedProjection;
	TArray<FUERLFieldDescriptor> PreparedActionFields;
	TArray<FUERLFieldDescriptor> PreparedStateFields;
	FUERLRobotTopology PreparedTopology;
	FUERLWorkerProjection EffectiveProjection;
	FUERLBatchSchema SelectedSchema;
	FSet Layouts;
	TArray<float> Actions;
	TArray<float> States;
	TArray<uint8> Faults;
	TArray<uint64> Episodes;
	double ActiveDeadlineSeconds = 0.0;
};

FUERLBridgeServer::FUERLBridgeServer() : Impl(MakeUnique<FImpl>()) {}
FUERLBridgeServer::~FUERLBridgeServer() = default;

bool FUERLBridgeServer::Start(
	const FUERLBridgeServerConfig& Config,
	const FUERLBridgeCapabilities& Capabilities,
	IBridgeRequestHandler& Handler,
	FString& OutError)
{
	return Impl->Start(Config, Capabilities, Handler, OutError);
}

void FUERLBridgeServer::Stop() { Impl->Shutdown(); }
bool FUERLBridgeServer::IsRunning() const { return Impl->IsRunning(); }
