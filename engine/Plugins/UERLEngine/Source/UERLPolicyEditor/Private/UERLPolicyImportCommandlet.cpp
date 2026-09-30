#include "UERLPolicyImportCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Misc/FileHelper.h"
#include "Misc/FeedbackContext.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/SoftObjectPath.h"
#include "UERLPolicyArtifactAsset.h"

DEFINE_LOG_CATEGORY_STATIC(LogUERLPolicyImportCommandlet, Log, All);

namespace
{
	struct FPolicyAssetPath
	{
		FString PackageName;
		FString ObjectName;

		FString ObjectPath() const
		{
			return PackageName + TEXT(".") + ObjectName;
		}
	};

	bool ResolvePolicyAssetPath(const FString& Input, FPolicyAssetPath& OutPath, FString& OutError)
	{
		if (Input.IsEmpty())
		{
			OutError = TEXT("-asset must not be empty");
			return false;
		}
		OutPath.PackageName = Input.Contains(TEXT("."))
			? FPackageName::ObjectPathToPackageName(Input)
			: Input;
		OutPath.ObjectName = Input.Contains(TEXT("."))
			? FPackageName::ObjectPathToObjectName(Input)
			: FPackageName::GetShortName(Input);

		FText PackageReason;
		if (!FPackageName::IsValidLongPackageName(OutPath.PackageName, false, &PackageReason))
		{
			OutError = FString::Printf(
				TEXT("-asset package '%s' is invalid: %s"),
				*OutPath.PackageName,
				*PackageReason.ToString());
			return false;
		}
		if (!OutPath.PackageName.StartsWith(TEXT("/Game/")))
		{
			OutError = FString::Printf(
				TEXT("-asset must target project Content under /Game/, got '%s'"),
				*OutPath.PackageName);
			return false;
		}
		if (OutPath.ObjectName.IsEmpty())
		{
			OutError = FString::Printf(TEXT("-asset has no object name: '%s'"), *Input);
			return false;
		}
		return true;
	}

	USkeletalMesh* LoadRobotMesh(const FString& Input, FString& OutError)
	{
		if (Input.IsEmpty())
		{
			OutError = TEXT("-robotmesh must not be empty");
			return nullptr;
		}
		UObject* Loaded = FSoftObjectPath(Input).TryLoad();
		if (!Loaded && !Input.Contains(TEXT(".")))
		{
			const FString ExplicitObjectPath = Input + TEXT(".") + FPackageName::GetShortName(Input);
			Loaded = FSoftObjectPath(ExplicitObjectPath).TryLoad();
		}
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Loaded);
		if (!Mesh)
		{
			OutError = FString::Printf(
				TEXT("could not load SkeletalMesh '%s'; pass a package or object path such as /Game/Robots/PhantomX/SK_PhantomX"),
				*Input);
		}
		return Mesh;
	}

	void PrintUsage()
	{
		UE_LOG(
			LogUERLPolicyImportCommandlet,
			Display,
			TEXT("Usage: UnrealEditor.exe <Project.uproject> -run=UERLPolicyImport "
			"-artifact=<file.uerlpol2> -asset=/Game/UERLEngine/Policies/Policy "
			"-robotmesh=/Game/Robots/PhantomX/SK_PhantomX [-replaceexisting]"));
		UE_LOG(
			LogUERLPolicyImportCommandlet,
			Display,
			TEXT("-asset accepts /Game/Path/Asset or /Game/Path/Asset.Asset; "
			"existing assets require -replaceexisting"));
	}
}

UUERLPolicyImportCommandlet::UUERLPolicyImportCommandlet(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	HelpDescription = TEXT("Import one UERL .uerlpol2 artifact, bind its RobotMesh, validate it, and save a UE asset.");
	HelpUsage = TEXT("-artifact=<file> -asset=/Game/Package/Asset -robotmesh=/Game/Robots/Mesh [-replaceexisting]");
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = true;
	ShowProgress = false;
	FastExit = true;
	UseCommandletResultAsExitCode = true;
}

int32 UUERLPolicyImportCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamValues;
	ParseCommandLine(*Params, Tokens, Switches, ParamValues);
	if (Switches.Contains(TEXT("help")) || Switches.Contains(TEXT("?")))
	{
		PrintUsage();
		return 0;
	}

	const FString ArtifactPathParam = ParamValues.FindRef(TEXT("artifact"));
	const FString AssetPathParam = ParamValues.FindRef(TEXT("asset"));
	const FString RobotMeshPathParam = ParamValues.FindRef(TEXT("robotmesh"));
	if (ArtifactPathParam.IsEmpty() || AssetPathParam.IsEmpty() || RobotMeshPathParam.IsEmpty())
	{
		UE_LOG(LogUERLPolicyImportCommandlet, Error, TEXT("Missing -artifact, -asset, or -robotmesh"));
		PrintUsage();
		return 1;
	}

	FPolicyAssetPath AssetPath;
	FString Error;
	if (!ResolvePolicyAssetPath(AssetPathParam, AssetPath, Error))
	{
		UE_LOG(LogUERLPolicyImportCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}

	const FString ArtifactPath = FPaths::ConvertRelativePathToFull(ArtifactPathParam);
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *ArtifactPath))
	{
		UE_LOG(LogUERLPolicyImportCommandlet, Error, TEXT("Could not read artifact '%s'"), *ArtifactPath);
		return 1;
	}

	USkeletalMesh* RobotMesh = LoadRobotMesh(RobotMeshPathParam, Error);
	if (!RobotMesh)
	{
		UE_LOG(LogUERLPolicyImportCommandlet, Error, TEXT("%s"), *Error);
		return 1;
	}

	const bool bReplaceExisting = Switches.Contains(TEXT("replaceexisting"));
	const FString ObjectPath = AssetPath.ObjectPath();
	UUERLPolicyArtifactAsset* Asset = LoadObject<UUERLPolicyArtifactAsset>(nullptr, *ObjectPath);
	UPackage* Package = nullptr;
	bool bCreated = false;
	if (Asset)
	{
		if (!bReplaceExisting)
		{
			UE_LOG(
				LogUERLPolicyImportCommandlet,
				Error,
				TEXT("Asset '%s' already exists; pass -replaceexisting to update it"),
				*ObjectPath);
			return 1;
		}
		Package = Asset->GetOutermost();
	}
	else
	{
		if (FPackageName::DoesPackageExist(AssetPath.PackageName))
		{
			UE_LOG(
				LogUERLPolicyImportCommandlet,
				Error,
				TEXT("Package '%s' exists but does not contain '%s'; choose another -asset path"),
				*AssetPath.PackageName,
				*AssetPath.ObjectName);
			return 1;
		}
		Package = CreatePackage(*AssetPath.PackageName);
		if (!Package)
		{
			UE_LOG(LogUERLPolicyImportCommandlet, Error, TEXT("Could not create package '%s'"), *AssetPath.PackageName);
			return 1;
		}
		Asset = NewObject<UUERLPolicyArtifactAsset>(Package, *AssetPath.ObjectName, RF_Public | RF_Standalone);
		bCreated = true;
	}

	if (!Asset->SetImportedBytes(Bytes, Error))
	{
		UE_LOG(LogUERLPolicyImportCommandlet, Error, TEXT("Artifact parse failed: %s"), *Error);
		return 1;
	}
	Asset->RobotMesh = RobotMesh;
#if WITH_EDITORONLY_DATA
	Asset->SourceFilePath = ArtifactPath;
#endif
	if (!Asset->ValidateForDeployment(Error))
	{
		UE_LOG(LogUERLPolicyImportCommandlet, Error, TEXT("Deployment validation failed: %s"), *Error);
		return 1;
	}

	if (bCreated)
	{
		FAssetRegistryModule::AssetCreated(Asset);
	}
	Package->MarkPackageDirty();
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(
		AssetPath.PackageName,
		FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.Error = GWarn;
	if (!UPackage::SavePackage(Package, Asset, *PackageFilename, SaveArgs))
	{
		UE_LOG(
			LogUERLPolicyImportCommandlet,
			Error,
			TEXT("Could not save policy asset '%s' to '%s'"),
			*ObjectPath,
			*PackageFilename);
		return 1;
	}

	UE_LOG(
		LogUERLPolicyImportCommandlet,
		Display,
		TEXT("[PASS] %s policy asset '%s' task=%s robot=%s mesh=%s source=%s"),
		bCreated ? TEXT("created") : TEXT("updated"),
		*ObjectPath,
		*Asset->Summary.TaskId.ToString(),
		*Asset->Summary.RobotId.ToString(),
		*RobotMesh->GetPathName(),
		*ArtifactPath);
	return 0;
}
