#include "Modules/ModuleManager.h"

#include "EditorReimportHandler.h"
#include "Factories/Factory.h"
#include "Misc/FileHelper.h"
#include "UObject/Package.h"
#include "UERLPolicyArtifactFactory.h"

UUERLPolicyArtifactFactory::UUERLPolicyArtifactFactory()
{
	SupportedClass = UUERLPolicyArtifactAsset::StaticClass();
	bEditorImport = true;
	bText = false;
	Formats.Add(TEXT("uerlpol2;UERL policy artifact"));
}

bool UUERLPolicyArtifactFactory::FactoryCanImport(const FString& Filename)
{
	return FPaths::GetExtension(Filename, false) == TEXT("uerlpol2");
}

UObject* UUERLPolicyArtifactFactory::FactoryCreateFile(
	UClass* InClass,
	UObject* InParent,
	FName InName,
	EObjectFlags Flags,
	const FString& Filename,
	const TCHAR* Parms,
	FFeedbackContext* Warn,
	bool& bOutOperationCanceled)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Filename))
	{
		if (Warn)
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("Could not read UERL policy artifact '%s'"), *Filename);
		}
		return nullptr;
	}
	UUERLPolicyArtifactAsset* Asset = NewObject<UUERLPolicyArtifactAsset>(InParent, InClass, InName, Flags);
	FString Error;
	if (!Asset->SetImportedBytes(Bytes, Error))
	{
		if (Warn)
		{
			Warn->Logf(ELogVerbosity::Error, TEXT("UERL policy artifact import failed: %s"), *Error);
		}
		Asset->MarkAsGarbage();
		return nullptr;
	}
#if WITH_EDITORONLY_DATA
	Asset->SourceFilePath = Filename;
#endif
	return Asset;
}

bool FUERLPolicyArtifactReimportHandler::CanReimport(UObject* Obj, TArray<FString>& OutFilenames)
{
	UUERLPolicyArtifactAsset* Asset = Cast<UUERLPolicyArtifactAsset>(Obj);
#if WITH_EDITORONLY_DATA
	if (Asset && !Asset->SourceFilePath.IsEmpty() && FPaths::FileExists(Asset->SourceFilePath))
	{
		OutFilenames.Add(Asset->SourceFilePath);
		return true;
	}
#else
	(void)Asset;
#endif
	return false;
}

void FUERLPolicyArtifactReimportHandler::SetReimportPaths(
	UObject* Obj,
	const TArray<FString>& NewReimportPaths)
{
#if WITH_EDITORONLY_DATA
	if (UUERLPolicyArtifactAsset* Asset = Cast<UUERLPolicyArtifactAsset>(Obj))
	{
		if (NewReimportPaths.Num() > 0)
		{
			Asset->SourceFilePath = NewReimportPaths[0];
		}
	}
#else
	(void)Obj;
	(void)NewReimportPaths;
#endif
}

EReimportResult::Type FUERLPolicyArtifactReimportHandler::Reimport(UObject* Obj)
{
	UUERLPolicyArtifactAsset* Asset = Cast<UUERLPolicyArtifactAsset>(Obj);
#if WITH_EDITORONLY_DATA
	if (!Asset || Asset->SourceFilePath.IsEmpty())
	{
		return EReimportResult::Failed;
	}
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Asset->SourceFilePath))
	{
		return EReimportResult::Failed;
	}
	FString Error;
	if (!Asset->SetImportedBytes(Bytes, Error))
	{
		return EReimportResult::Failed;
	}
	Asset->MarkPackageDirty();
	return EReimportResult::Succeeded;
#else
	(void)Asset;
	return EReimportResult::Failed;
#endif
}

class FUERLPolicyEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		ReimportHandler = MakeUnique<FUERLPolicyArtifactReimportHandler>();
	}

	virtual void ShutdownModule() override
	{
		if (ReimportHandler)
		{
			ReimportHandler.Reset();
		}
	}

private:
	TUniquePtr<FUERLPolicyArtifactReimportHandler> ReimportHandler;
};

IMPLEMENT_MODULE(FUERLPolicyEditorModule, UERLPolicyEditor);
