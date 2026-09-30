#include "UERLPolicyEditorSubsystem.h"

#include "Engine/Engine.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/IConsoleManager.h"
#include "Logging/MessageLog.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "UERLPolicyEditorChecks.h"
#include "Widgets/Notifications/SNotificationList.h"

DEFINE_LOG_CATEGORY_STATIC(LogUERLPolicyEditor, Log, All);

void UUERLPolicyEditorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	CheckProjectCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("UERL.CheckProject"),
		TEXT("Run the UERL project health checks (packaging readiness, policy artifact integrity, deploy physics settings) and report to the UERL message log"),
		FConsoleCommandDelegate::CreateUObject(this, &UUERLPolicyEditorSubsystem::RunProjectChecks),
		ECVF_Default);
	if (IsRunningCommandlet())
	{
		return;
	}
	FCoreDelegates::OnFEngineLoopInitComplete.AddLambda([this]()
	{
		RunProjectChecks();
	});
}

void UUERLPolicyEditorSubsystem::Deinitialize()
{
	if (CheckProjectCommand)
	{
		IConsoleManager::Get().UnregisterConsoleObject(CheckProjectCommand);
		CheckProjectCommand = nullptr;
	}
	Super::Deinitialize();
}

void UUERLPolicyEditorSubsystem::RunProjectChecks()
{
	TArray<FUERLProjectCheckIssue> Issues;

	FString FoundBinary;
	FString PackagingMessage;
	if (!UERLPolicyEditorChecks::EvaluatePackagingReadiness(
		FModuleManager::Get().ModuleExists(FApp::GetProjectName()),
		UERLPolicyEditorChecks::HasPrebuiltGameBinaries(FoundBinary),
		PackagingMessage))
	{
		FUERLProjectCheckIssue& Issue = Issues.AddDefaulted_GetRef();
		Issue.Category = FName(TEXT("Packaging"));
		Issue.Message = PackagingMessage;
	}

	UERLPolicyEditorChecks::CollectArtifactIssues(Issues);
	if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
	{
		UERLPolicyEditorChecks::CollectDeployPhysicsIssues(*World, Issues);
	}

	FMessageLog Log(TEXT("UERL"));
	if (Issues.IsEmpty())
	{
		UE_LOG(LogUERLPolicyEditor, Display, TEXT("UERL project check: no issues (packaging, artifacts, deploy physics)"));
		Log.Info(FText::FromString(TEXT("UERL project check passed: packaging, artifacts and deploy physics are ready")));
		return;
	}
	for (const FUERLProjectCheckIssue& Issue : Issues)
	{
		UE_LOG(LogUERLPolicyEditor, Warning, TEXT("UERL project check [%s] %s"), *Issue.Category.ToString(), *Issue.Message);
		Log.Warning(FText::FromString(FString::Printf(TEXT("[%s] %s"), *Issue.Category.ToString(), *Issue.Message)));
	}
	Log.Open(EMessageSeverity::Warning, true);
	FNotificationInfo Info(FText::Format(
		NSLOCTEXT("UERL", "ProjectCheckFailed", "UERL project check found {0} issue(s) — see the UERL message log"),
		Issues.Num()));
	Info.bFireAndForget = true;
	Info.ExpireDuration = 6.0f;
	FSlateNotificationManager::Get().AddNotification(Info);
}
