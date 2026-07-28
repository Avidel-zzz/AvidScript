#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptComponent.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadService.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
class FFakeAvidScriptRealCancelWatchHost final
	: public IAvidScriptEditorCSharpLiveReloadWatchHost
{
public:
	virtual bool Start(
		const FString& WorkspaceRoot,
		FOnChangeBatch InOnChangeBatch,
		FString& OutErrorCategory,
		FString& OutErrorMessage) override
	{
		Callback = MoveTemp(InOnChangeBatch);
		bWatching = true;
		return true;
	}

	virtual void Stop() override
	{
		bWatching = false;
		Callback = FOnChangeBatch();
	}

	virtual bool IsWatching() const override
	{
		return bWatching;
	}

	void Emit(const FString& FilePath)
	{
		if (!bWatching || !Callback)
		{
			return;
		}
		FAvidScriptEditorCSharpLiveReloadChangeBatch Batch;
		Batch.FilePaths.Add(FilePath);
		Callback(MoveTemp(Batch));
	}

	bool bWatching = false;
	FOnChangeBatch Callback;
};

struct FAvidScriptObservedRealCancelState
{
	int32 CancelCount = 0;
	bool bDestroyed = false;
	EAvidScriptEditorCSharpAsyncBuildStage LastStage =
		EAvidScriptEditorCSharpAsyncBuildStage::Idle;
};

class FAvidScriptObservedRealCancelJob final
	: public IAvidScriptEditorCSharpAsyncBuildJob
{
public:
	FAvidScriptObservedRealCancelJob(
		TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob> InInner,
		TSharedRef<FAvidScriptObservedRealCancelState> InState)
		: Inner(MoveTemp(InInner))
		, State(MoveTemp(InState))
	{
	}

	virtual ~FAvidScriptObservedRealCancelJob() override
	{
		State->bDestroyed = true;
	}

	virtual bool Start(const FString& ProfilePath) override
	{
		const bool bStarted = Inner->Start(ProfilePath);
		State->LastStage = Inner->GetProgress().Stage;
		return bStarted;
	}

	virtual void Tick() override
	{
		Inner->Tick();
		State->LastStage = Inner->GetProgress().Stage;
	}

	virtual void Cancel() override
	{
		if (bCancelForwarded)
		{
			return;
		}
		bCancelForwarded = true;
		++State->CancelCount;
		Inner->Cancel();
		State->LastStage = Inner->GetProgress().Stage;
	}

	virtual bool IsFinished() const override
	{
		return Inner->IsFinished();
	}

	virtual const FAvidScriptEditorCSharpAsyncBuildProgress&
		GetProgress() const override
	{
		return Inner->GetProgress();
	}

	virtual bool ConsumeResult(
		FAvidScriptEditorCSharpAsyncBuildResult& OutResult) override
	{
		return Inner->ConsumeResult(OutResult);
	}

private:
	TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob> Inner;
	TSharedRef<FAvidScriptObservedRealCancelState> State;
	bool bCancelForwarded = false;
};

FString NormalizeAvidScriptRealCancelPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

bool CreateAvidScriptRealCancelWorld(
	UWorld*& OutWorld,
	AActor*& OutActor,
	UAvidScriptComponent*& OutComponent)
{
	OutWorld = nullptr;
	OutActor = nullptr;
	OutComponent = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptRealCancelWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& Context =
		GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(OutWorld);
	const FURL Url;
	OutWorld->InitializeActorsForPlay(Url);
	OutWorld->BeginPlay();
	OutWorld->SetBegunPlay(true);

	OutActor = OutWorld->SpawnActor<AActor>();
	if (OutActor == nullptr)
	{
		return false;
	}
	OutComponent = NewObject<UAvidScriptComponent>(
		OutActor,
		TEXT("AvidScriptRealCancelComponent"));
	if (OutComponent == nullptr)
	{
		return false;
	}
	OutActor->AddInstanceComponent(OutComponent);
	OutComponent->RegisterComponent();
	return OutComponent->GetRuntimeStats().bRuntimeLoaded;
}

void DestroyAvidScriptRealCancelWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}
	if (World->HasBegunPlay())
	{
		World->EndPlay(EEndPlayReason::Quit);
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

bool ReadAvidScriptProcessId(
	const FString& Path,
	uint32& OutProcessId)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		return false;
	}
	Text.TrimStartAndEndInline();
	const int64 ProcessId = FCString::Atoi64(*Text);
	if (ProcessId <= 0 || ProcessId > MAX_uint32)
	{
		return false;
	}
	OutProcessId = static_cast<uint32>(ProcessId);
	return true;
}

bool WriteAvidScriptRealCancelFixture(
	const FString& TestRoot,
	FString& OutWorkspaceRoot,
	FString& OutProfilePath,
	FString& OutReportPath,
	FString& OutManifestPath,
	FString& OutWasmPath,
	FString& OutDotNetPidPath,
	FString& OutLeafPidPath)
{
	OutWorkspaceRoot = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(TestRoot, TEXT("Workspace")));
	const FString OutputRoot = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(TestRoot, TEXT("Output")));
	const FString ProjectPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(TestRoot, TEXT("CancelableBuild.proj")));
	const FString LeafScriptPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(TestRoot, TEXT("CancelableBuildLeaf.ps1")));
	const FString BuildScriptPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(TestRoot, TEXT("CancelableBuild.ps1")));
	const FString PowerShellPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(
			FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot")),
			TEXT("System32/WindowsPowerShell/v1.0/powershell.exe")));
	OutProfilePath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(TestRoot, TEXT("cancel.csharp-profile.json")));
	OutReportPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(OutputRoot, TEXT("cancel.csharp.report.json")));
	OutManifestPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(OutputRoot, TEXT("cancel.avidscript.json")));
	OutWasmPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(OutputRoot, TEXT("cancel.wasm")));
	OutDotNetPidPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(TestRoot, TEXT("dotnet.pid")));
	OutLeafPidPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(TestRoot, TEXT("leaf.pid")));
	const FString TriggerPath = NormalizeAvidScriptRealCancelPath(
		FPaths::Combine(OutWorkspaceRoot, TEXT("Trigger.cs")));
	if (!FPaths::FileExists(PowerShellPath)
		|| !IFileManager::Get().MakeDirectory(*OutWorkspaceRoot, true)
		|| !IFileManager::Get().MakeDirectory(*OutputRoot, true))
	{
		return false;
	}

	const FString LeafScript = FString::Printf(
		TEXT(R"PS(param([string]$PidPath)
[System.IO.File]::WriteAllText($PidPath, [string]$PID)
Start-Sleep -Seconds 60
)PS"));
	const FString ProjectText = FString::Printf(
		TEXT(R"XML(<Project DefaultTargets="Hold" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Target Name="Hold">
    <Exec Command="&quot;%s&quot; -NoProfile -ExecutionPolicy Bypass -File &quot;%s&quot; -PidPath &quot;%s&quot;" />
  </Target>
</Project>
)XML"),
		*PowerShellPath,
		*LeafScriptPath,
		*OutLeafPidPath);
	const FString BuildScript = FString::Printf(
		TEXT(R"PS(param(
    [string]$DotNetPath = "",
    [string]$OutputRoot = "",
    [string]$Configuration = "Release",
    [string]$SourcePath = "",
    [string]$ProjectPath = "",
    [string]$ModuleId = "",
    [string]$ArtifactStem = "",
    [string]$ReportPath = "",
    [string]$ManifestPath = "",
    [string]$BindingPackagePath = "",
    [string]$RuntimeBindingPackagePath = "",
    [switch]$OmitRuntimeBindingPackage,
    [string]$PreparedBuildReportPath = "",
    [string]$SemanticCacheRoot = "",
    [switch]$DisableSemanticCache
)
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($DotNetPath)) {
    $DotNetPath = Join-Path $env:USERPROFILE ".dotnet\dotnet.exe"
}
$WasmPath = Join-Path $OutputRoot "$ArtifactStem.wasm"
[System.IO.File]::WriteAllText($ReportPath, "candidate-report")
[System.IO.File]::WriteAllText($ManifestPath, "candidate-manifest")
[System.IO.File]::WriteAllText($WasmPath, "candidate-wasm")
$QuotedProjectPath = '"' + $ProjectPath + '"'
$Child = Start-Process -FilePath $DotNetPath -ArgumentList @("msbuild", $QuotedProjectPath, "-nologo", "-v:q") -NoNewWindow -PassThru
[System.IO.File]::WriteAllText("%s", [string]$Child.Id)
Wait-Process -Id $Child.Id
exit $Child.ExitCode
)PS"),
		*OutDotNetPidPath);
	const FString ProfileText = FString::Printf(
		TEXT(R"JSON({
  "schema_version": 1,
  "language": "csharp",
  "source_path": "%s",
  "project_path": "%s",
  "build_script_path": "%s",
  "module_id": "csharp_real_cancel",
  "artifact_stem": "cancel",
  "output_root": "%s",
  "report_path": "%s",
  "manifest_path": "%s",
  "configuration": "Release"
}
)JSON"),
		*FAvidScriptEditorCSharpBuildService::
			GetDefaultActorLifecycleSourcePath(),
		*ProjectPath,
		*BuildScriptPath,
		*OutputRoot,
		*OutReportPath,
		*OutManifestPath);

	return FFileHelper::SaveStringToFile(TEXT("// trigger\n"), *TriggerPath)
		&& FFileHelper::SaveStringToFile(LeafScript, *LeafScriptPath)
		&& FFileHelper::SaveStringToFile(ProjectText, *ProjectPath)
		&& FFileHelper::SaveStringToFile(BuildScript, *BuildScriptPath)
		&& FFileHelper::SaveStringToFile(ProfileText, *OutProfilePath)
		&& FFileHelper::SaveStringToFile(TEXT("committed-report"), *OutReportPath)
		&& FFileHelper::SaveStringToFile(TEXT("committed-manifest"), *OutManifestPath)
		&& FFileHelper::SaveStringToFile(TEXT("committed-wasm"), *OutWasmPath);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpAutoLiveReloadRealCancelTest,
	"AvidScript.Editor.CSharpLiveReload.RealCancelPreservesRuntimeAndArtifacts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpAutoLiveReloadRealCancelTest::RunTest(
	const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptRealCancelPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P45/RealCancel")));
	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	FString WorkspaceRoot;
	FString ProfilePath;
	FString ReportPath;
	FString ManifestPath;
	FString WasmPath;
	FString DotNetPidPath;
	FString LeafPidPath;
	if (!TestTrue(
			TEXT("Real cancel fixture writes"),
			WriteAvidScriptRealCancelFixture(
				TestRoot,
				WorkspaceRoot,
				ProfilePath,
				ReportPath,
				ManifestPath,
				WasmPath,
				DotNetPidPath,
				LeafPidPath)))
	{
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
		return false;
	}

	UWorld* World = nullptr;
	AActor* Actor = nullptr;
	UAvidScriptComponent* Component = nullptr;
	if (!TestTrue(
			TEXT("Real cancel runtime world creates"),
			CreateAvidScriptRealCancelWorld(World, Actor, Component)))
	{
		DestroyAvidScriptRealCancelWorld(World);
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
		return false;
	}

	double Now = 10.0;
	int32 ApplyCount = 0;
	const TSharedRef<FAvidScriptObservedRealCancelState> ObservedState =
		MakeShared<FAvidScriptObservedRealCancelState>();
	FFakeAvidScriptRealCancelWatchHost* WatchHost =
		new FFakeAvidScriptRealCancelWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(WatchHost),
		[ObservedState]()
		{
			return TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob>(
				new FAvidScriptObservedRealCancelJob(
					FAvidScriptEditorCSharpAsyncBuildJobFactory::Create(),
					ObservedState));
		},
		[&ApplyCount](
			const FString& CandidateReportPath,
			AActor* Target,
			FAvidScriptEditorComponentBindingResult& OutResult)
		{
			++ApplyCount;
			return FAvidScriptEditorComponentBindingService::
				ApplyCSharpReportToActor(
					CandidateReportPath,
					Target,
					OutResult);
		},
		[&Now]() { return Now; });
	FAvidScriptEditorCSharpLiveReloadServiceConfig Config;
	Config.WorkspaceRoot = WorkspaceRoot;
	Config.ProfilePath = ProfilePath;
	Config.DebounceSeconds = 0.01;
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(
		TEXT("Real cancel service starts"),
		Service.Start(Config, Actor, StartResult));

	const FString TriggerPath = FPaths::Combine(
		WorkspaceRoot,
		TEXT("Trigger.cs"));
	WatchHost->Emit(TriggerPath);
	Service.Tick();
	Now = 10.02;
	Service.Tick();
	TestEqual(
		TEXT("Real cancel build enters active state"),
		Service.GetLastResult().Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::Building);

	uint32 DotNetProcessId = 0;
	uint32 LeafProcessId = 0;
	const double StartedDeadline = FPlatformTime::Seconds() + 20.0;
	while (FPlatformTime::Seconds() < StartedDeadline)
	{
		Service.Tick();
		if (ReadAvidScriptProcessId(DotNetPidPath, DotNetProcessId)
			&& ReadAvidScriptProcessId(LeafPidPath, LeafProcessId)
			&& FPlatformProcess::IsApplicationRunning(DotNetProcessId)
			&& FPlatformProcess::IsApplicationRunning(LeafProcessId))
		{
			break;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	const bool bDotNetChildRunning =
		DotNetProcessId > 0
		&& FPlatformProcess::IsApplicationRunning(DotNetProcessId);
	const bool bLeafChildRunning =
		LeafProcessId > 0
		&& FPlatformProcess::IsApplicationRunning(LeafProcessId);
	if (!bDotNetChildRunning || !bLeafChildRunning)
	{
		const FAvidScriptEditorCSharpLiveReloadServiceResult& DiagnosticResult =
			Service.GetLastResult();
		AddError(FString::Printf(
			TEXT("Real cancel child launch diagnostic | status=%d | stage=%d | category=%s | cause=%s | message=%s | output=%s"),
			static_cast<int32>(DiagnosticResult.Status),
			static_cast<int32>(DiagnosticResult.AsyncProgress.Stage),
			*DiagnosticResult.ErrorCategory,
			*DiagnosticResult.CauseErrorCategory,
			*DiagnosticResult.ErrorMessage,
			*DiagnosticResult.AsyncProgress.LatestOutputLine));
	}
	TestTrue(
		TEXT("Real cancel build launches dotnet child"),
		bDotNetChildRunning);
	TestTrue(
		TEXT("Real cancel build launches nested PowerShell child"),
		bLeafChildRunning);

	const int32 TickCountBeforeCancel =
		Component->GetRuntimeStats().TickCallCount;
	Service.Stop();
	TestEqual(
		TEXT("Service forwards one real cancel"),
		ObservedState->CancelCount,
		1);
	TestEqual(
		TEXT("Real build job enters canceled stage"),
		ObservedState->LastStage,
		EAvidScriptEditorCSharpAsyncBuildStage::Canceled);
	TestTrue(
		TEXT("Canceled real build job is destroyed"),
		ObservedState->bDestroyed);

	const double StoppedDeadline = FPlatformTime::Seconds() + 10.0;
	while (FPlatformTime::Seconds() < StoppedDeadline
		&& (FPlatformProcess::IsApplicationRunning(DotNetProcessId)
			|| FPlatformProcess::IsApplicationRunning(LeafProcessId)))
	{
		FPlatformProcess::Sleep(0.01f);
	}
	TestFalse(
		TEXT("Cancel terminates dotnet child"),
		FPlatformProcess::IsApplicationRunning(DotNetProcessId));
	TestFalse(
		TEXT("Cancel terminates nested PowerShell child"),
		FPlatformProcess::IsApplicationRunning(LeafProcessId));

	FString ReportText;
	FString ManifestText;
	FString WasmText;
	TestTrue(
		TEXT("Cancel restores committed report"),
		FFileHelper::LoadFileToString(ReportText, *ReportPath));
	TestEqual(
		TEXT("Restored report bytes are unchanged"),
		ReportText,
		FString(TEXT("committed-report")));
	TestTrue(
		TEXT("Cancel restores committed manifest"),
		FFileHelper::LoadFileToString(ManifestText, *ManifestPath));
	TestEqual(
		TEXT("Restored manifest bytes are unchanged"),
		ManifestText,
		FString(TEXT("committed-manifest")));
	TestTrue(
		TEXT("Cancel restores committed WASM"),
		FFileHelper::LoadFileToString(WasmText, *WasmPath));
	TestEqual(
		TEXT("Restored WASM bytes are unchanged"),
		WasmText,
		FString(TEXT("committed-wasm")));

	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(
		TEXT("Old runtime continues ticking after real cancel"),
		Component->GetRuntimeStats().TickCallCount,
		TickCountBeforeCancel + 1);
	TestEqual(TEXT("Canceled build never binds"), ApplyCount, 0);

	DestroyAvidScriptRealCancelWorld(World);
	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
