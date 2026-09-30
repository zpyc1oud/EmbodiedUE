#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "UERLPolicyEditorSubsystem.generated.h"

struct IConsoleCommand;

/** One-time and on-demand project health checks for UERL policy deployment. */
UCLASS()
class UERLPOLICYEDITOR_API UUERLPolicyEditorSubsystem final : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** Run packaging/artifact/physics checks and report through the UERL message log. */
	void RunProjectChecks();

private:
	IConsoleCommand* CheckProjectCommand = nullptr;
};
