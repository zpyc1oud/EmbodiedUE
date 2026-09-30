#pragma once

#include "CoreMinimal.h"
#include "UERLPolicyArtifact.h"
#include "UERLPolicyArtifactAsset.generated.h"

class USkeletalMesh;

/** Editor-visible, derived information for a cooked policy asset. */
USTRUCT(BlueprintType)
struct UERLPOLICY_API FUERLPolicyArtifactSummary
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	FName TaskId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	FName RobotId;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	double PhysicsDt = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	int32 DecimationMin = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	int32 DecimationMax = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	TArray<FName> CommandChannels;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	TArray<int32> CommandWidths;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	int32 OnnxBytes = 0;

	void Reset();
	void ReadFrom(const FUERLPolicyArtifact& Artifact);
};

/** Runtime policy asset: the UERLPOL2 bytes plus the mesh selected for deployment. */
UCLASS(BlueprintType)
class UERLPOLICY_API UUERLPolicyArtifactAsset final : public UObject
{
	GENERATED_BODY()

public:
	/** The only policy model payload retained by the asset. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Policy")
	TArray<uint8> ArtifactBytes;

	/** Explicit mesh reference tracked by the cooker and validated at StartPolicy. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Robot")
	TObjectPtr<USkeletalMesh> RobotMesh = nullptr;

	/** Parsed display-only metadata; it is never used as a second policy source. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Policy")
	FUERLPolicyArtifactSummary Summary;

#if WITH_EDITORONLY_DATA
	/** Source path used only by the Editor reimport handler. */
	UPROPERTY()
	FString SourceFilePath;
#endif

	/** Parse the stored bytes through the one public artifact parser. */
	bool LoadArtifact(FUERLPolicyArtifact& OutArtifact, FString& OutError) const;

	/** Replace imported bytes and their derived summary as one operation. */
	bool SetImportedBytes(TConstArrayView<uint8> Bytes, FString& OutError);

	/** Validate the selected mesh against the artifact's reflected plans. */
	bool ValidateRobotMesh(FString& OutError) const;

	/** Validate bytes and mesh together with editor-actionable error text. */
	bool ValidateForDeployment(FString& OutError) const;

#if WITH_EDITOR
	/** Save/cook-time data validation: bytes parse and RobotMesh resolves. */
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
#endif
};
