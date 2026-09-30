#include "UERLPolicyEditorChecks.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Interfaces/IPluginManager.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "Misc/Paths.h"
#include "UERLPhysicsSnapshot.h"
#include "UERLPolicyArtifactAsset.h"
#include "UERLPolicyPhysicsGate.h"

namespace
{
	void CollectArtifactAssets(TArray<UUERLPolicyArtifactAsset*>& OutAssets)
	{
		FAssetRegistryModule& AssetRegistryModule =
			FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		FARFilter Filter;
		Filter.ClassPaths.Add(UUERLPolicyArtifactAsset::StaticClass()->GetClassPathName());
		Filter.bRecursiveClasses = true;
		TArray<FAssetData> AssetData;
		AssetRegistryModule.Get().GetAssets(Filter, AssetData);
		for (const FAssetData& Data : AssetData)
		{
			if (UUERLPolicyArtifactAsset* Asset = Cast<UUERLPolicyArtifactAsset>(Data.GetAsset()))
			{
				OutAssets.Add(Asset);
			}
		}
	}
}

bool UERLPolicyEditorChecks::EvaluatePackagingReadiness(
	bool bHasGameModule,
	bool bHasPrebuiltGameBinaries,
	FString& OutMessage)
{
	if (bHasGameModule || bHasPrebuiltGameBinaries)
	{
		OutMessage.Reset();
		return true;
	}
	OutMessage = TEXT(
		"this is a Blueprint-only project and no prebuilt UnrealGame UERL binaries exist; "
		"packaging with an installed engine will fail at startup with 'UERLInterface could not be found'. "
		"Add any empty C++ class via Tools > New C++ Class, or run scripts/install_into_project.py, "
		"so UAT links the plugin into the game target. "
		"scripts/package_plugin.py only distributes editor binaries and does not replace a C++ Game Target");
	return false;
}

bool UERLPolicyEditorChecks::HasPrebuiltGameBinaries(FString& OutFoundPath)
{
	const FString ProjectBinary = FPaths::Combine(
		FPaths::ProjectDir(), TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealGame-UERLInterface.dll"));
	if (FPaths::FileExists(ProjectBinary))
	{
		OutFoundPath = ProjectBinary;
		return true;
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("UERLEngine"));
	if (Plugin.IsValid())
	{
		const FString PluginBinary = FPaths::Combine(
			Plugin->GetBaseDir(), TEXT("Binaries"), TEXT("Win64"), TEXT("UnrealGame-UERLInterface.dll"));
		if (FPaths::FileExists(PluginBinary))
		{
			OutFoundPath = PluginBinary;
			return true;
		}
	}
	OutFoundPath.Reset();
	return false;
}

int32 UERLPolicyEditorChecks::CollectArtifactIssues(TArray<FUERLProjectCheckIssue>& OutIssues)
{
	TArray<UUERLPolicyArtifactAsset*> Assets;
	CollectArtifactAssets(Assets);
	for (const UUERLPolicyArtifactAsset* Asset : Assets)
	{
		FString Error;
		if (!Asset->ValidateForDeployment(Error))
		{
			FUERLProjectCheckIssue& Issue = OutIssues.AddDefaulted_GetRef();
			Issue.Category = FName(TEXT("Artifact"));
			Issue.Message = FString::Printf(TEXT("%s: %s"), *Asset->GetPathName(), *Error);
		}
	}
	return OutIssues.Num();
}

int32 UERLPolicyEditorChecks::CollectDeployPhysicsIssues(UWorld& World, TArray<FUERLProjectCheckIssue>& OutIssues)
{
	TArray<UUERLPolicyArtifactAsset*> Assets;
	CollectArtifactAssets(Assets);
	if (Assets.IsEmpty())
	{
		return 0;
	}
	// The editor check evaluates project settings; the live Chaos solver only
	// exists at Play time and remains verified by the runtime gate at StartPolicy.
	const UPhysicsSettings* Settings = UPhysicsSettings::Get();
	if (!Settings)
	{
		FUERLProjectCheckIssue& Issue = OutIssues.AddDefaulted_GetRef();
		Issue.Category = FName(TEXT("DeployPhysics"));
		Issue.Message = TEXT("UPhysicsSettings is unavailable");
		return 1;
	}
	FUERLPhysicsSnapshot Snapshot;
	Snapshot.SolverPath = FName(TEXT("Chaos"));
	Snapshot.bHasSolver = true;
	Snapshot.bTickPhysicsAsync = Settings->bTickPhysicsAsync;
	Snapshot.bSubsteppingAsync = Settings->bSubsteppingAsync;
	Snapshot.bSubstepping = Settings->bSubstepping;
	Snapshot.MaxSubstepDeltaTime = Settings->MaxSubstepDeltaTime;
	Snapshot.MaxSubsteps = Settings->MaxSubsteps;
	Snapshot.MinPhysicsDeltaTime = Settings->MinPhysicsDeltaTime;
	Snapshot.MaxPhysicsDeltaTime = Settings->MaxPhysicsDeltaTime;
	Snapshot.WorldTimeDilation = World.GetWorldSettings()
		? World.GetWorldSettings()->GetEffectiveTimeDilation()
		: 1.0;
	// Owner dilation is a per-instance runtime property; deployment owners
	// default to 1.0 and the runtime gate re-checks each owner at StartPolicy.
	Snapshot.OwnerTimeDilation = 1.0;
	Snapshot.bOwnerTimeDilationKnown = true;
	Snapshot.GravityZ = Settings->DefaultGravityZ;
	for (const UUERLPolicyArtifactAsset* Asset : Assets)
	{
		FUERLPolicyArtifact Parsed;
		FString Error;
		if (!Asset->LoadArtifact(Parsed, Error))
		{
			continue; // bytes issues are reported by the artifact check
		}
		const FUERLPhysicsGateReport Gate = CheckDeployPhysicsRequirements(Snapshot, Parsed.Timing());
		if (Gate.bPassed)
		{
			continue;
		}
		FUERLProjectCheckIssue& Issue = OutIssues.AddDefaulted_GetRef();
		Issue.Category = FName(TEXT("DeployPhysics"));
		Issue.Message = FString::Printf(
			TEXT("%s: %s. Fix in Config/DefaultEngine.ini under [/Script/Engine.PhysicsSettings]: "
				"bTickPhysicsAsync=False, bSubstepping=True, bSubsteppingAsync=False, "
				"MaxSubstepDeltaTime <= artifact physics_dt, MaxSubsteps covering artifact DtMax. "
				"Worker training processes lock physics via launch arguments and are unaffected"),
			*Asset->GetPathName(), *Gate.ToString());
	}
	return OutIssues.Num();
}
