#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"
#include "EditorReimportHandler.h"
#include "UERLPolicyArtifactAsset.h"
#include "UERLPolicyArtifactFactory.generated.h"

UCLASS()
class UERLPOLICYEDITOR_API UUERLPolicyArtifactFactory final : public UFactory
{
	GENERATED_BODY()

public:
	UUERLPolicyArtifactFactory();

	virtual bool FactoryCanImport(const FString& Filename) override;
	virtual UObject* FactoryCreateFile(
		UClass* InClass,
		UObject* InParent,
		FName InName,
		EObjectFlags Flags,
		const FString& Filename,
		const TCHAR* Parms,
		FFeedbackContext* Warn,
		bool& bOutOperationCanceled) override;
};

/** Reimport source handler; parse failure leaves the existing asset untouched. */
class FUERLPolicyArtifactReimportHandler final : public FReimportHandler
{
public:
	virtual bool CanReimport(UObject* Obj, TArray<FString>& OutFilenames) override;
	virtual void SetReimportPaths(UObject* Obj, const TArray<FString>& NewReimportPaths) override;
	virtual EReimportResult::Type Reimport(UObject* Obj) override;
};
