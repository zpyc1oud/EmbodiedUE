#include "Misc/AutomationTest.h"

#include "Engine/Engine.h"
#include "Factories/Factory.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"
#include "Misc/DataValidation.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "UObject/Package.h"
#include "UERLPolicyArtifactFactory.h"
#include "UERLPolicyEditorChecks.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	bool ResolveEditorArtifact(FString& OutPath)
	{
		const TArray<FString> Candidates = {
			FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT(".."), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("controller"),
				TEXT("cartpole_controller.uerlpol2"))),
			FPaths::ConvertRelativePathToFull(FPaths::Combine(
				FPaths::ProjectDir(), TEXT("tests"), TEXT("parity"), TEXT("cases"), TEXT("controller"),
				TEXT("cartpole_controller.uerlpol2"))),
		};
		for (const FString& Candidate : Candidates)
		{
			if (FPaths::FileExists(Candidate))
			{
				OutPath = Candidate;
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyEditorFactoryTest,
	"UERL.Unit.Policy.Editor.AC_UE_UNIT_COMPONENT_004.FactoryAndReimport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyEditorFactoryTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactFactory* Factory = NewObject<UUERLPolicyArtifactFactory>(GetTransientPackageAsObject());
	TestTrue(TEXT("factory accepts .uerlpol2"), Factory->FactoryCanImport(TEXT("policy.uerlpol2")));
	TestFalse(TEXT("factory rejects unrelated files"), Factory->FactoryCanImport(TEXT("policy.json")));
	FString SourcePath;
	TestTrue(TEXT("resolve editor artifact source"), ResolveEditorArtifact(SourcePath));
	if (SourcePath.IsEmpty())
	{
		return false;
	}
	bool bCanceled = false;
	UObject* ImportedObject = Factory->FactoryCreateFile(
		UUERLPolicyArtifactAsset::StaticClass(),
		GetTransientPackageAsObject(),
		TEXT("TransientPolicyAsset"),
		RF_Transient,
		SourcePath,
		nullptr,
		GWarn,
		bCanceled);
	UUERLPolicyArtifactAsset* Asset = Cast<UUERLPolicyArtifactAsset>(ImportedObject);
	TestNotNull(TEXT("factory creates policy asset"), Asset);
	if (!Asset)
	{
		return false;
	}
	const TArray<uint8> Before = Asset->ArtifactBytes;
	TestTrue(TEXT("factory stores source bytes"), !Before.IsEmpty());
#if WITH_EDITORONLY_DATA
	TestEqual(TEXT("factory records source path"), Asset->SourceFilePath, SourcePath);
#endif
	FUERLPolicyArtifactReimportHandler Handler;
	TArray<FString> ReimportPaths;
	if (!TestTrue(TEXT("reimport handler recognizes imported asset"), Handler.CanReimport(Asset, ReimportPaths))
		|| !TestEqual(TEXT("reimport path count"), ReimportPaths.Num(), 1))
	{
		return false;
	}
	TestEqual(TEXT("reimport keeps source path"), ReimportPaths[0], SourcePath);
	TestEqual(TEXT("reimport succeeds"), Handler.Reimport(Asset), EReimportResult::Succeeded);
	TestTrue(TEXT("reimport keeps bytes bitwise"), Asset->ArtifactBytes == Before);
	AddInfo(TEXT("[VERIFY] AC_UE_UNIT_COMPONENT_004: Editor factory imports raw bytes and transactional reimport preserves the asset object"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyEditorPackagingReadinessTest,
	"UERL.Unit.Policy.Editor.AC_UE_EDITOR_CHECK_001.PackagingReadinessDecision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyEditorPackagingReadinessTest::RunTest(const FString& Parameters)
{
	FString Message;
	TestTrue(TEXT("a project with a game module is packaging-ready"),
		UERLPolicyEditorChecks::EvaluatePackagingReadiness(true, false, Message));
	TestTrue(TEXT("prebuilt game binaries satisfy a Blueprint-only project"),
		UERLPolicyEditorChecks::EvaluatePackagingReadiness(false, true, Message));
	TestFalse(TEXT("Blueprint-only without binaries fails the check"),
		UERLPolicyEditorChecks::EvaluatePackagingReadiness(false, false, Message));
	TestTrue(TEXT("failure message offers the C++ class remedy"),
		Message.Contains(TEXT("New C++ Class")));
	TestTrue(TEXT("failure message offers the install script"),
		Message.Contains(TEXT("install_into_project.py")));
	TestTrue(TEXT("failure message does not treat editor plugin binaries as a packaged game"),
		Message.Contains(TEXT("does not replace a C++ Game Target")));

	TestTrue(TEXT("the host project itself has a game module"),
		FModuleManager::Get().ModuleExists(FApp::GetProjectName()));
	FString FoundBinary;
	FString PackagingMessage;
	TestTrue(TEXT("the host project passes the packaging readiness wiring"),
		UERLPolicyEditorChecks::EvaluatePackagingReadiness(
			FModuleManager::Get().ModuleExists(FApp::GetProjectName()),
			UERLPolicyEditorChecks::HasPrebuiltGameBinaries(FoundBinary),
			PackagingMessage));
	TestNotNull(TEXT("UERL.CheckProject console command is registered"),
		IConsoleManager::Get().FindConsoleObject(TEXT("UERL.CheckProject")));
	AddInfo(TEXT("[VERIFY] AC_UE_EDITOR_CHECK_001/002: readiness decision and host wiring"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyEditorArtifactValidationTest,
	"UERL.Unit.Policy.Editor.AC_UE_EDITOR_CHECK_010.ArtifactDataValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyEditorArtifactValidationTest::RunTest(const FString& Parameters)
{
	UUERLPolicyArtifactAsset* EmptyAsset = NewObject<UUERLPolicyArtifactAsset>(GetTransientPackageAsObject());
	FDataValidationContext EmptyContext;
	TestEqual(TEXT("asset without bytes is invalid"),
		EmptyAsset->IsDataValid(EmptyContext), EDataValidationResult::Invalid);

	FString SourcePath;
	TestTrue(TEXT("resolve editor artifact source"), ResolveEditorArtifact(SourcePath));
	TArray<uint8> Bytes;
	TestTrue(TEXT("artifact source bytes load"), FFileHelper::LoadFileToArray(Bytes, *SourcePath));
	UUERLPolicyArtifactAsset* NoMeshAsset = NewObject<UUERLPolicyArtifactAsset>(GetTransientPackageAsObject());
	FString ImportError;
	TestTrue(TEXT("bytes import succeeds"), NoMeshAsset->SetImportedBytes(Bytes, ImportError));
	FDataValidationContext NoMeshContext;
	TestEqual(TEXT("asset with bytes but no mesh is invalid"),
		NoMeshAsset->IsDataValid(NoMeshContext), EDataValidationResult::Invalid);
	FString DeploymentError;
	TestFalse(TEXT("deployment validation reports the missing mesh"),
		NoMeshAsset->ValidateForDeployment(DeploymentError));
	TestTrue(TEXT("missing-mesh message offers the assignment remedy"),
		DeploymentError.Contains(TEXT("assign the deployment SkeletalMesh")));
	TestTrue(TEXT("missing-mesh message offers the migration remedy"),
		DeploymentError.Contains(TEXT("migrate the mesh assets")));

	UUERLPolicyArtifactAsset* DemoAsset = LoadObject<UUERLPolicyArtifactAsset>(
		nullptr,
		TEXT("/UERLEngine/Policies/PhantomXContinuousTerrain116.PhantomXContinuousTerrain116"));
	TestNotNull(TEXT("shipped PhantomX policy asset loads"), DemoAsset);
	if (DemoAsset)
	{
		FDataValidationContext DemoContext;
		TestEqual(TEXT("shipped artifact is valid in this project"),
			DemoAsset->IsDataValid(DemoContext), EDataValidationResult::Valid);
	}
	AddInfo(TEXT("[VERIFY] AC_UE_EDITOR_CHECK_010: IsDataValid catches missing bytes/mesh and accepts the shipped artifact"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyEditorArtifactIssueCollectionTest,
	"UERL.Unit.Policy.Editor.AC_UE_EDITOR_CHECK_011.ArtifactIssueCollection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyEditorArtifactIssueCollectionTest::RunTest(const FString& Parameters)
{
	TArray<FUERLProjectCheckIssue> Issues;
	UERLPolicyEditorChecks::CollectArtifactIssues(Issues);
	TestEqual(TEXT("all shipped artifacts in this project pass the integrity check"), Issues.Num(), 0);
	AddInfo(TEXT("[VERIFY] AC_UE_EDITOR_CHECK_011: shipped artifacts report no integrity issues here"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FUERLPolicyEditorDeployPhysicsCheckTest,
	"UERL.Unit.Policy.Editor.AC_UE_EDITOR_CHECK_020.DeployPhysicsGateCatchesBrokenSettings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FUERLPolicyEditorDeployPhysicsCheckTest::RunTest(const FString& Parameters)
{
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	TestNotNull(TEXT("editor world available"), World);
	if (!World)
	{
		return false;
	}
	UPhysicsSettings* Settings = GetMutableDefault<UPhysicsSettings>();
	TestNotNull(TEXT("physics settings available"), Settings);
	if (!Settings)
	{
		return false;
	}
	// Pin the deploy baseline for the duration of this test; other suites may
	// leave Worker lockstep values behind, so ambient state is not trusted.
	struct FPhysicsRestoreGuard
	{
		UPhysicsSettings* S = GetMutableDefault<UPhysicsSettings>();
		bool A = S->bTickPhysicsAsync, B = S->bSubstepping, C = S->bSubsteppingAsync;
		float H = S->MaxSubstepDeltaTime;
		int32 N = S->MaxSubsteps;
		~FPhysicsRestoreGuard()
		{
			S->bTickPhysicsAsync = A; S->bSubstepping = B; S->bSubsteppingAsync = C;
			S->MaxSubstepDeltaTime = H; S->MaxSubsteps = N;
		}
	} Restore;
	Settings->bTickPhysicsAsync = false;
	Settings->bSubstepping = true;
	Settings->bSubsteppingAsync = false;
	Settings->MaxSubstepDeltaTime = 0.005f;
	Settings->MaxSubsteps = 7;

	TArray<FUERLProjectCheckIssue> Issues;
	UERLPolicyEditorChecks::CollectDeployPhysicsIssues(*World, Issues);
	TestEqual(TEXT("deploy baseline settings satisfy the shipped artifact"), Issues.Num(), 0);

	Settings->bSubstepping = false;
	Issues.Reset();
	UERLPolicyEditorChecks::CollectDeployPhysicsIssues(*World, Issues);
	TestTrue(TEXT("breaking synchronous substepping is reported"), Issues.Num() > 0);
	if (Issues.Num() > 0)
	{
		TestTrue(TEXT("the failure names the ini section"),
			Issues[0].Message.Contains(TEXT("[/Script/Engine.PhysicsSettings]")));
		TestTrue(TEXT("the failure offers the substepping ini key"),
			Issues[0].Message.Contains(TEXT("bSubstepping")));
		TestTrue(TEXT("the failure offers the substep delta ini key"),
			Issues[0].Message.Contains(TEXT("MaxSubstepDeltaTime")));
		TestTrue(TEXT("the failure offers the substeps ini key"),
			Issues[0].Message.Contains(TEXT("MaxSubsteps")));
	}
	AddInfo(TEXT("[VERIFY] AC_UE_EDITOR_CHECK_020/021: deploy physics check passes here and catches broken settings with ini remedies"));
	return true;
}

#endif
