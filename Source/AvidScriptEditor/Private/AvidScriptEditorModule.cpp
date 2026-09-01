#include "AvidScriptEditorModule.h"

#include "AvidScriptComponent.h"
#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptEditorDiagnosticLog.h"
#include "AvidScriptEditorGeneratedBindingService.h"
#include "AvidScriptEditorResultPresentation.h"
#include "AvidScriptEditorSourceConfig.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Containers/Ticker.h"
#include "Debugging/AvidScriptEditorDebugTargetController.h"
#include "Debugging/SAvidScriptEditorDebugPanel.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

DEFINE_LOG_CATEGORY(LogAvidScriptEditor);

#define LOCTEXT_NAMESPACE "AvidScriptEditorModule"

namespace
{
FString NormalizeAvidScriptEditorModulePath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}

	return Path;
}

FString GetAvidScriptEditorModulePluginBaseDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (Plugin.IsValid())
	{
		return NormalizeAvidScriptEditorModulePath(Plugin->GetBaseDir());
	}

	return NormalizeAvidScriptEditorModulePath(FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins"),
		TEXT("AvidScript")));
}

void SetAvidScriptSampleCommandFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorCommandLaunchResult& OutResult)
{
	OutResult = FAvidScriptEditorCommandLaunchResult();
	OutResult.bSucceeded = false;
	OutResult.bReloadApplied = false;
	OutResult.Summary = FString::Printf(TEXT("%s: %s next=%s"), *ErrorCategory, *ErrorMessage, *NextAction);
	OutResult.CommandResult.Status = EAvidScriptEditorCommandStatus::CompileFailed;
	OutResult.CommandResult.ErrorCategory = ErrorCategory;
	OutResult.CommandResult.ErrorMessage = ErrorMessage;
	OutResult.CommandResult.NextAction = NextAction;
}

void SetAvidScriptCSharpProfileCommandFailure(
	const FAvidScriptEditorCSharpProfileLoadResult& ProfileResult,
	FAvidScriptEditorCSharpBuildResult& OutBuildResult)
{
	OutBuildResult = FAvidScriptEditorCSharpBuildResult();
	OutBuildResult.bSucceeded = false;
	OutBuildResult.ErrorCategory = ProfileResult.ErrorCategory;
	OutBuildResult.ErrorMessage = ProfileResult.ErrorMessage;
	OutBuildResult.NextAction = ProfileResult.NextAction;
	OutBuildResult.SourcePath = ProfileResult.BuildConfig.SourcePath;
	OutBuildResult.ProjectPath = ProfileResult.BuildConfig.ProjectPath;
	OutBuildResult.BuildScriptPath = ProfileResult.BuildConfig.BuildScriptPath;
	OutBuildResult.OutputRoot = ProfileResult.BuildConfig.OutputRoot;
	OutBuildResult.ReportPath = ProfileResult.BuildConfig.ReportPath;
	OutBuildResult.ManifestPath = ProfileResult.BuildConfig.ManifestPath;
	OutBuildResult.ModuleId = ProfileResult.BuildConfig.ModuleId;
	OutBuildResult.ArtifactStem = ProfileResult.BuildConfig.ArtifactStem;
}

void SetAvidScriptCSharpWorkspaceBuildFailure(
	const FAvidScriptEditorCSharpWorkspaceResult& WorkspaceResult,
	FAvidScriptEditorCSharpBuildResult& OutBuildResult)
{
	OutBuildResult = FAvidScriptEditorCSharpBuildResult();
	OutBuildResult.bSucceeded = false;
	OutBuildResult.ErrorCategory = WorkspaceResult.ErrorCategory;
	OutBuildResult.ErrorMessage = WorkspaceResult.ErrorMessage;
	OutBuildResult.NextAction = WorkspaceResult.NextAction;
	OutBuildResult.SourcePath = WorkspaceResult.SourcePath;
	OutBuildResult.ProjectPath = WorkspaceResult.ProjectPath;
	OutBuildResult.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	OutBuildResult.OutputRoot = WorkspaceResult.OutputRoot;
	OutBuildResult.ReportPath = WorkspaceResult.ReportPath;
	OutBuildResult.ManifestPath = WorkspaceResult.ManifestPath;
	OutBuildResult.ModuleId = TEXT("csharp_project_gameplay");
	OutBuildResult.ArtifactStem = TEXT("project_gameplay");
}

void SetAvidScriptCSharpLiveReloadStartFailure(
	const FString& ErrorCategory,
	const FString& CauseErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	const FAvidScriptEditorCSharpWorkspaceResult& WorkspaceResult,
	const FAvidScriptEditorComponentBindingResult& BindingResult,
	FAvidScriptEditorCSharpLiveReloadServiceResult& OutResult)
{
	OutResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	OutResult.bSucceeded = false;
	OutResult.bRunning = false;
	OutResult.Status = EAvidScriptEditorCSharpLiveReloadServiceStatus::StartFailed;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.CauseErrorCategory = CauseErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
	OutResult.WorkspaceRoot = WorkspaceResult.WorkspaceRoot;
	OutResult.ProfilePath = WorkspaceResult.ProfilePath;
	OutResult.TargetActorPath = BindingResult.ActorPath;
}

void LogAvidScriptMenuRegistrationFailure(const FAvidScriptEditorMenuRegistrationResult& Result)
{
	UE_LOG(
		LogAvidScriptEditor,
		Warning,
		TEXT("AvidScript menu registration failed: menu=%s entry=%s category=%s message=%s"),
		*Result.MenuName.ToString(),
		*Result.EntryName.ToString(),
		*Result.ErrorCategory,
		*Result.ErrorMessage);
}

void LogAvidScriptEditorPresentation(const FAvidScriptEditorCommandPresentation& Presentation)
{
	FAvidScriptEditorDiagnosticLog::Publish(Presentation);
	if (Presentation.Severity == EAvidScriptEditorPresentationSeverity::Info)
	{
		UE_LOG(
			LogAvidScriptEditor,
			Display,
			TEXT("%s: %s\n%s"),
			*Presentation.Title,
			*Presentation.Body,
			*Presentation.Details);
		return;
	}

	if (Presentation.Severity == EAvidScriptEditorPresentationSeverity::Warning)
	{
		UE_LOG(
			LogAvidScriptEditor,
			Warning,
			TEXT("%s: %s\n%s"),
			*Presentation.Title,
			*Presentation.Body,
			*Presentation.Details);
		return;
	}

	UE_LOG(
		LogAvidScriptEditor,
		Error,
		TEXT("%s: %s\n%s"),
		*Presentation.Title,
		*Presentation.Body,
		*Presentation.Details);
}
} // namespace

FAvidScriptEditorModule::FAvidScriptEditorModule(
	TUniquePtr<FAvidScriptEditorCSharpLiveReloadService> InCSharpLiveReloadService)
	: CSharpLiveReloadService(MoveTemp(InCSharpLiveReloadService))
{
}

void FAvidScriptEditorModule::StartupModule()
{
	FAvidScriptEditorDiagnosticLog::Register();
	CommandLauncher = MakeUnique<FAvidScriptEditorCommandLauncher>();
	if (!CSharpLiveReloadService)
	{
		CSharpLiveReloadService = MakeUnique<FAvidScriptEditorCSharpLiveReloadService>();
	}
	GeneratedTypeReloadService =
		MakeUnique<FAvidScriptEditorGeneratedTypeReloadService>();
	DebugTargetController = MakeShared<FAvidScriptEditorDebugTargetController>();
	RegisterDebuggerTab();
	BeginPIEHandle = FEditorDelegates::BeginPIE.AddRaw(
		this,
		&FAvidScriptEditorModule::HandleBeginPIE);
	EndPIEHandle = FEditorDelegates::EndPIE.AddRaw(
		this,
		&FAvidScriptEditorModule::HandleEndPIE);
	DebugTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FAvidScriptEditorModule::TickDebugger),
		0.2f);
	if (GEditor != nullptr && GEditor->PlayWorld != nullptr)
	{
		DebugTargetController->HandleBeginPIE();
	}
	FAvidScriptEditorGeneratedTypeReloadServiceResult GeneratedTypeReloadResult;
	if (!GeneratedTypeReloadService->Start(
			FAvidScriptEditorGeneratedTypeReloadPolicy::GetDefaultDescriptorPath(),
			GeneratedTypeReloadResult))
	{
		UE_LOG(
			LogAvidScriptEditor,
			Warning,
			TEXT("Generated type reload watcher did not start: category=%s details=%s"),
			*GeneratedTypeReloadResult.ErrorCategory,
			*GeneratedTypeReloadResult.ErrorMessage);
	}
	RegisterConsoleCommands();
	ToolMenusStartupCallbackHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAvidScriptEditorModule::RegisterMenus));

	UE_LOG(LogAvidScriptEditor, Display, TEXT("AvidScriptEditor module started."));
}

void FAvidScriptEditorModule::ShutdownModule()
{
	UnregisterConsoleCommands();
	FAvidScriptEditorDiagnosticLog::Unregister();
	if (DebugTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DebugTickerHandle);
		DebugTickerHandle.Reset();
	}
	if (BeginPIEHandle.IsValid())
	{
		FEditorDelegates::BeginPIE.Remove(BeginPIEHandle);
		BeginPIEHandle.Reset();
	}
	if (EndPIEHandle.IsValid())
	{
		FEditorDelegates::EndPIE.Remove(EndPIEHandle);
		EndPIEHandle.Reset();
	}
	if (DebugTargetController.IsValid())
	{
		DebugTargetController->HandleEndPIE();
	}
	UnregisterDebuggerTab();
	if (PublishCSharpBindingsAssetRegistryHandle.IsValid()
		&& FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
	{
		FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"))
			.Get()
			.OnFilesLoaded()
			.Remove(PublishCSharpBindingsAssetRegistryHandle);
		PublishCSharpBindingsAssetRegistryHandle.Reset();
	}

	if (CSharpLiveReloadService)
	{
		CSharpLiveReloadService->Stop();
		CSharpLiveReloadService.Reset();
	}
	if (GeneratedTypeReloadService)
	{
		GeneratedTypeReloadService->Stop();
		GeneratedTypeReloadService.Reset();
	}

	if (ToolMenusStartupCallbackHandle.IsValid())
	{
		UToolMenus::UnRegisterStartupCallback(ToolMenusStartupCallbackHandle);
		ToolMenusStartupCallbackHandle.Reset();
	}

	if (UToolMenus* ToolMenus = UToolMenus::TryGet())
	{
		ToolMenus->UnregisterOwnerByName(GetToolMenuOwnerName());
	}

	CommandLauncher.Reset();
	DebugTargetController.Reset();

	UE_LOG(LogAvidScriptEditor, Display, TEXT("AvidScriptEditor module stopped."));
}

void FAvidScriptEditorModule::RegisterConsoleCommands()
{
	GenerateBindingsConsoleCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AvidScript.GenerateBindings"),
		TEXT("Generate the project binding module from a descriptor file. Usage: AvidScript.GenerateBindings <descriptor>"),
		FConsoleCommandWithArgsDelegate::CreateRaw(
			this,
			&FAvidScriptEditorModule::HandleGenerateBindingsConsoleCommand),
		ECVF_Default);
	PublishCSharpBindingsConsoleCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("AvidScript.PublishCSharpBindings"),
		TEXT("Publish the generated C# gameplay binding package. Usage: AvidScript.PublishCSharpBindings [output-root] [exit]"),
		FConsoleCommandWithArgsDelegate::CreateRaw(
			this,
			&FAvidScriptEditorModule::HandlePublishCSharpBindingsConsoleCommand),
		ECVF_Default);
}

void FAvidScriptEditorModule::UnregisterConsoleCommands()
{
	if (GenerateBindingsConsoleCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(GenerateBindingsConsoleCommand);
		GenerateBindingsConsoleCommand = nullptr;
	}
	if (PublishCSharpBindingsConsoleCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(PublishCSharpBindingsConsoleCommand);
		PublishCSharpBindingsConsoleCommand = nullptr;
	}
}

void FAvidScriptEditorModule::HandleGenerateBindingsConsoleCommand(
	const TArray<FString>& Arguments)
{
	if (Arguments.Num() != 1)
	{
		UE_LOG(
			LogAvidScriptEditor,
			Error,
			TEXT("AvidScript.GenerateBindings requires exactly one descriptor path."));
		return;
	}

	const FString DescriptorPath = NormalizeAvidScriptEditorModulePath(
		FPaths::IsRelative(Arguments[0])
			? FPaths::Combine(FPaths::ProjectDir(), Arguments[0])
			: Arguments[0]);
	FAvidScriptEditorGeneratedBindingResult Result;
	if (!FAvidScriptEditorGeneratedBindingService::GenerateProjectModuleFromDescriptorFile(
			FPaths::GetProjectFilePath(),
			DescriptorPath,
			Result))
	{
		UE_LOG(
			LogAvidScriptEditor,
			Error,
			TEXT("AvidScript.GenerateBindings failed. category=%s source=%s error=%s"),
			*Result.ErrorCategory,
			*Result.ErrorSource,
			*Result.ErrorMessage);
		return;
	}

	UE_LOG(
		LogAvidScriptEditor,
		Display,
		TEXT("AvidScript.GenerateBindings succeeded. package=%s bindings=%d reused=%s output=%s"),
		*Result.PackageHash,
		Result.BindingCount,
		Result.bReusedExistingModule ? TEXT("true") : TEXT("false"),
		*Result.OutputDirectory);
}

void FAvidScriptEditorModule::HandlePublishCSharpBindingsConsoleCommand(
	const TArray<FString>& Arguments)
{
	if (Arguments.Num() > 2
		|| Arguments.Num() == 2 && !Arguments[1].Equals(TEXT("exit"), ESearchCase::IgnoreCase))
	{
		UE_LOG(
			LogAvidScriptEditor,
			Error,
			TEXT("AvidScript.PublishCSharpBindings expects [output-root] [exit]."));
		return;
	}

	const bool bExit = Arguments.Num() == 1
		&& Arguments[0].Equals(TEXT("exit"), ESearchCase::IgnoreCase)
		|| Arguments.Num() == 2;
	const bool bHasOutputRoot = !Arguments.IsEmpty()
		&& !Arguments[0].Equals(TEXT("exit"), ESearchCase::IgnoreCase);
	const FString OutputRoot = !bHasOutputRoot
		? FAvidScriptEditorCSharpBindingEmitter::GetDefaultOutputRoot()
		: NormalizeAvidScriptEditorModulePath(
			FPaths::IsRelative(Arguments[0])
				? FPaths::Combine(FPaths::ProjectDir(), Arguments[0])
				: Arguments[0]);
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	if (AssetRegistryModule.Get().IsLoadingAssets())
	{
		if (PublishCSharpBindingsAssetRegistryHandle.IsValid())
		{
			UE_LOG(LogAvidScriptEditor, Error, TEXT("A C# binding publication is already queued."));
			return;
		}
		PendingCSharpBindingsOutputRoot = OutputRoot;
		bExitAfterCSharpBindingsPublish = bExit;
		PublishCSharpBindingsAssetRegistryHandle = AssetRegistryModule.Get().OnFilesLoaded().AddRaw(
			this,
			&FAvidScriptEditorModule::HandleAssetRegistryReadyForCSharpBindings);
		UE_LOG(
			LogAvidScriptEditor,
			Display,
			TEXT("AvidScript.PublishCSharpBindings queued until AssetRegistry is ready."));
		return;
	}

	const bool bSucceeded = ExecutePublishCSharpBindings(OutputRoot);
	if (bExit)
	{
		FPlatformMisc::RequestExitWithStatus(true, bSucceeded ? 0 : 1);
	}
}

void FAvidScriptEditorModule::HandleAssetRegistryReadyForCSharpBindings()
{
	FAssetRegistryModule& AssetRegistryModule =
		FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	AssetRegistryModule.Get().OnFilesLoaded().Remove(PublishCSharpBindingsAssetRegistryHandle);
	PublishCSharpBindingsAssetRegistryHandle.Reset();
	const FString OutputRoot = MoveTemp(PendingCSharpBindingsOutputRoot);
	const bool bExit = bExitAfterCSharpBindingsPublish;
	bExitAfterCSharpBindingsPublish = false;
	const bool bSucceeded = ExecutePublishCSharpBindings(OutputRoot);
	if (bExit)
	{
		FPlatformMisc::RequestExitWithStatus(true, bSucceeded ? 0 : 1);
	}
}

bool FAvidScriptEditorModule::ExecutePublishCSharpBindings(const FString& OutputRoot)
{
	FAvidScriptCSharpBindingEmitResult Result;
	if (!FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(OutputRoot, Result))
	{
		UE_LOG(
			LogAvidScriptEditor,
			Error,
			TEXT("AvidScript.PublishCSharpBindings failed. category=%s source=%s error=%s"),
			*Result.ErrorCategory,
			*Result.ErrorSource,
			*Result.ErrorMessage);
		return false;
	}

	UE_LOG(
		LogAvidScriptEditor,
		Display,
		TEXT("AvidScript.PublishCSharpBindings succeeded. package=%s reused=%s reference=%s manifest=%s"),
		*Result.PackageHash,
		Result.bReusedExistingPackage ? TEXT("true") : TEXT("false"),
		*Result.ReferenceSourcePath,
		*Result.ManifestPath);
	return true;
}

FName FAvidScriptEditorModule::GetToolMenuOwnerName()
{
	return TEXT("AvidScriptEditor");
}

FName FAvidScriptEditorModule::GetSampleCommandMenuName()
{
	return TEXT("LevelEditor.MainMenu.Tools");
}

FName FAvidScriptEditorModule::GetSampleCommandSectionName()
{
	return TEXT("AvidScript");
}

FName FAvidScriptEditorModule::GetSampleCommandEntryName()
{
	return TEXT("AvidScript.RunSampleCommand");
}

FString FAvidScriptEditorModule::GetSampleCommandSourcePath()
{
	return NormalizeAvidScriptEditorModulePath(FPaths::Combine(
		GetAvidScriptEditorModulePluginBaseDir(),
		TEXT("Samples"),
		TEXT("AvidScript"),
		TEXT("ActorSetLocation"),
		TEXT("actor_set_location.avid")));
}

FName FAvidScriptEditorModule::GetCSharpActorLifecycleBindEntryName()
{
	return TEXT("AvidScript.BindCSharpActorLifecycleReport");
}

FName FAvidScriptEditorModule::GetCSharpActorLifecycleBuildAndBindEntryName()
{
	return TEXT("AvidScript.BuildAndBindCSharpActorLifecycle");
}

FName FAvidScriptEditorModule::GetCSharpProfileBuildAndBindEntryName()
{
	return TEXT("AvidScript.BuildAndBindCSharpProfile");
}

FName FAvidScriptEditorModule::GetCSharpProfileTemplateEntryName()
{
	return TEXT("AvidScript.CreateDefaultCSharpProfile");
}

FName FAvidScriptEditorModule::GetCSharpWorkspaceCreateEntryName()
{
	return TEXT("AvidScript.CreateProjectCSharpGameplayWorkspace");
}

FName FAvidScriptEditorModule::GetCSharpWorkspaceBuildAndBindEntryName()
{
	return TEXT("AvidScript.BuildAndBindProjectCSharpGameplay");
}

FName FAvidScriptEditorModule::GetCSharpWorkspaceLiveReloadStartEntryName()
{
	return TEXT("AvidScript.StartProjectCSharpLiveReload");
}

FName FAvidScriptEditorModule::GetCSharpWorkspaceLiveReloadStopEntryName()
{
	return TEXT("AvidScript.StopProjectCSharpLiveReload");
}

FName FAvidScriptEditorModule::GetDebuggerTabName()
{
	return TEXT("AvidScript.Debugger");
}

FName FAvidScriptEditorModule::GetDebuggerMenuEntryName()
{
	return TEXT("AvidScript.OpenDebugger");
}

FString FAvidScriptEditorModule::GetCSharpActorLifecycleReportPath()
{
	return FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleReportPath();
}

FString FAvidScriptEditorModule::GetCSharpActorLifecycleBuildScriptPath()
{
	return FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
}

FString FAvidScriptEditorModule::GetDefaultCSharpProfilePath()
{
	return FAvidScriptEditorCSharpProfileService::GetDefaultProfilePath();
}

FString FAvidScriptEditorModule::GetProjectCSharpWorkspaceProfilePath()
{
	return FAvidScriptEditorCSharpWorkspaceService::GetDefaultProfilePath();
}

bool FAvidScriptEditorModule::MakeSampleCommandConfig(
	FAvidScriptEditorCommandLaunchConfig& OutConfig,
	FString& OutErrorMessage)
{
	const FAvidScriptEditorToolchainSettings Settings;
	return MakeCommandConfigForSource(
		GetSampleCommandSourcePath(),
		Settings,
		OutConfig,
		OutErrorMessage);
}

bool FAvidScriptEditorModule::MakeCommandConfigForSource(
	const FString& SourcePath,
	const FAvidScriptEditorToolchainSettings& Settings,
	FAvidScriptEditorCommandLaunchConfig& OutConfig,
	FString& OutErrorMessage)
{
	OutConfig = FAvidScriptEditorCommandLaunchConfig();
	OutErrorMessage.Reset();

	FAvidScriptEditorSourceConfigRequest Request;
	Request.SourcePath = SourcePath;

	FAvidScriptEditorSourceConfigResult Result;
	if (!FAvidScriptEditorSourceConfigService::BuildLaunchConfig(Request, Result))
	{
		OutErrorMessage = FString::Printf(
			TEXT("%s: %s next=%s"),
			*Result.ErrorCategory,
			*Result.ErrorMessage,
			*Result.NextAction);
		return false;
	}

	OutConfig = Result.LaunchConfig;
	FAvidScriptEditorSettingsService::ApplySettings(Settings, OutConfig);
	return true;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeSampleMenuEntryConfig(FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetSampleCommandEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT("AvidScriptRunSampleCommandLabel", "Run Sample Script");
	Config.ToolTip = LOCTEXT("AvidScriptRunSampleCommandToolTip", "Compile and apply the AvidScript actor_set_location sample.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeCSharpActorLifecycleBindMenuEntryConfig(FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetCSharpActorLifecycleBindEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT("AvidScriptBindCSharpActorLifecycleLabel", "Bind C# ActorLifecycle Script");
	Config.ToolTip = LOCTEXT("AvidScriptBindCSharpActorLifecycleToolTip", "Apply the latest C# ActorLifecycle build report to the selected Actor.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeCSharpActorLifecycleBuildAndBindMenuEntryConfig(FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetCSharpActorLifecycleBuildAndBindEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT("AvidScriptBuildAndBindCSharpActorLifecycleLabel", "Build And Bind C# ActorLifecycle Script");
	Config.ToolTip = LOCTEXT("AvidScriptBuildAndBindCSharpActorLifecycleToolTip", "Build the C# ActorLifecycle sample and apply its report to the selected Actor.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeCSharpProfileBuildAndBindMenuEntryConfig(FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetCSharpProfileBuildAndBindEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT("AvidScriptBuildAndBindCSharpProfileLabel", "Build And Bind C# Profile Script");
	Config.ToolTip = LOCTEXT("AvidScriptBuildAndBindCSharpProfileToolTip", "Build the configured C# profile and apply its report to the selected Actor.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeCSharpProfileTemplateMenuEntryConfig(FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetCSharpProfileTemplateEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT("AvidScriptCreateDefaultCSharpProfileLabel", "Create Default C# Profile");
	Config.ToolTip = LOCTEXT("AvidScriptCreateDefaultCSharpProfileToolTip", "Create the default C# profile JSON if it does not already exist.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeCSharpWorkspaceCreateMenuEntryConfig(FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetCSharpWorkspaceCreateEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT("AvidScriptCreateProjectCSharpGameplayWorkspaceLabel", "Create Project C# Gameplay Workspace");
	Config.ToolTip = LOCTEXT("AvidScriptCreateProjectCSharpGameplayWorkspaceToolTip", "Create project-owned C# gameplay files and refresh the generated UE API facade without overwriting existing source.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeCSharpWorkspaceBuildAndBindMenuEntryConfig(FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetCSharpWorkspaceBuildAndBindEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT("AvidScriptBuildAndBindProjectCSharpGameplayLabel", "Build And Bind Project C# Gameplay Script");
	Config.ToolTip = LOCTEXT("AvidScriptBuildAndBindProjectCSharpGameplayToolTip", "Refresh the generated UE API facade, build the project C# gameplay script, and bind it to the selected Actor.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeCSharpWorkspaceLiveReloadStartMenuEntryConfig(
	FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetCSharpWorkspaceLiveReloadStartEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT(
		"AvidScriptStartProjectCSharpLiveReloadLabel",
		"Start Project C# Auto Live Reload");
	Config.ToolTip = LOCTEXT(
		"AvidScriptStartProjectCSharpLiveReloadToolTip",
		"Build and bind the project C# gameplay script, then watch its workspace and reload the fixed Actor after source changes.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeCSharpWorkspaceLiveReloadStopMenuEntryConfig(
	FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetCSharpWorkspaceLiveReloadStopEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT(
		"AvidScriptStopProjectCSharpLiveReloadLabel",
		"Stop Project C# Auto Live Reload");
	Config.ToolTip = LOCTEXT(
		"AvidScriptStopProjectCSharpLiveReloadToolTip",
		"Stop watching and automatically rebuilding the project C# gameplay workspace.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

FAvidScriptEditorMenuEntryConfig FAvidScriptEditorModule::MakeDebuggerMenuEntryConfig(
	FSimpleDelegate ExecuteAction)
{
	FAvidScriptEditorMenuEntryConfig Config;
	Config.OwnerName = GetToolMenuOwnerName();
	Config.MenuName = GetSampleCommandMenuName();
	Config.SectionName = GetSampleCommandSectionName();
	Config.EntryName = GetDebuggerMenuEntryName();
	Config.SectionLabel = LOCTEXT("AvidScriptMenuSection", "AvidScript");
	Config.Label = LOCTEXT("AvidScriptDebuggerLabel", "AvidScript Debugger");
	Config.ToolTip = LOCTEXT(
		"AvidScriptDebuggerToolTip",
		"Open the AvidScript C# debugger for live PIE sessions.");
	Config.ExecuteAction = MoveTemp(ExecuteAction);
	return Config;
}

bool FAvidScriptEditorModule::ExecuteSampleCommand(FAvidScriptEditorCommandLaunchResult& OutResult)
{
	if (!CommandLauncher.IsValid())
	{
		SetAvidScriptSampleCommandFailure(
			TEXT("launcher_unavailable"),
			TEXT("AvidScript sample command cannot run before the editor command launcher is initialized."),
			TEXT("restart the editor or reload the AvidScriptEditor module"),
			OutResult);
		return false;
	}

	FAvidScriptEditorCommandLaunchConfig Config;
	FString ErrorMessage;
	if (!MakeSampleCommandConfig(Config, ErrorMessage))
	{
		SetAvidScriptSampleCommandFailure(
			TEXT("sample_config_failed"),
			ErrorMessage,
			TEXT("verify the AvidScript sample source exists in the plugin Samples directory"),
			OutResult);
		return false;
	}

	return CommandLauncher->CompileSourceAndApply(Config, OutResult);
}

bool FAvidScriptEditorModule::ExecuteCSharpActorLifecycleBinding(
	FAvidScriptEditorComponentBindingResult& OutResult)
{
	return FAvidScriptEditorComponentBindingService::ApplyCSharpReportToSelectedActor(
		GetCSharpActorLifecycleReportPath(),
		OutResult);
}

bool FAvidScriptEditorModule::ExecuteCSharpActorLifecycleBuildAndBinding(
	FAvidScriptEditorCSharpBuildResult& OutBuildResult,
	FAvidScriptEditorComponentBindingResult& OutBindingResult)
{
	OutBindingResult = FAvidScriptEditorComponentBindingResult();

	FAvidScriptEditorCSharpBuildConfig BuildConfig;
	BuildConfig.BuildScriptPath = GetCSharpActorLifecycleBuildScriptPath();
	BuildConfig.OutputRoot = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleOutputRoot();
	BuildConfig.ReportPath = GetCSharpActorLifecycleReportPath();

	if (!FAvidScriptEditorCSharpBuildService::BuildActorLifecycle(BuildConfig, OutBuildResult))
	{
		return false;
	}

	return ExecuteCSharpActorLifecycleBinding(OutBindingResult);
}

bool FAvidScriptEditorModule::ExecuteCSharpProfileBuildAndBinding(
	const FString& ProfilePath,
	FAvidScriptEditorCSharpBuildResult& OutBuildResult,
	FAvidScriptEditorComponentBindingResult& OutBindingResult)
{
	OutBindingResult = FAvidScriptEditorComponentBindingResult();

	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!FAvidScriptEditorCSharpProfileService::LoadProfile(ProfilePath, ProfileResult))
	{
		SetAvidScriptCSharpProfileCommandFailure(ProfileResult, OutBuildResult);
		return false;
	}

	if (!FAvidScriptEditorCSharpBuildService::BuildProfile(
			FAvidScriptEditorCSharpProfileService::MakeBuildRequest(ProfileResult),
			OutBuildResult))
	{
		return false;
	}

	return FAvidScriptEditorComponentBindingService::ApplyCSharpReportToSelectedActor(
		OutBuildResult.ReportPath,
		OutBindingResult);
}
bool FAvidScriptEditorModule::ExecuteCreateCSharpProfileTemplate(
	const FString& ProfilePath,
	FAvidScriptEditorCSharpProfileTemplateResult& OutResult,
	bool bOverwrite)
{
	return FAvidScriptEditorCSharpProfileService::WriteProfileTemplate(ProfilePath, OutResult, bOverwrite);
}

bool FAvidScriptEditorModule::ExecuteCreateCSharpWorkspace(
	FAvidScriptEditorCSharpWorkspaceResult& OutResult,
	bool bOverwriteUserFiles)
{
	FAvidScriptEditorCSharpWorkspaceConfig Config;
	Config.bOverwriteUserFiles = bOverwriteUserFiles;
	return FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(Config, OutResult);
}

bool FAvidScriptEditorModule::ExecuteCSharpWorkspaceBuildAndBinding(
	FAvidScriptEditorCSharpBuildResult& OutBuildResult,
	FAvidScriptEditorComponentBindingResult& OutBindingResult)
{
	FAvidScriptEditorCSharpWorkspaceConfig WorkspaceConfig;
	FAvidScriptEditorCSharpWorkspaceResult WorkspaceResult;
	return ExecuteCSharpWorkspaceBuildAndBinding(
		WorkspaceConfig,
		WorkspaceResult,
		OutBuildResult,
		OutBindingResult);
}

bool FAvidScriptEditorModule::ExecuteCSharpWorkspaceBuildAndBinding(
	const FAvidScriptEditorCSharpWorkspaceConfig& WorkspaceConfig,
	FAvidScriptEditorCSharpWorkspaceResult& OutWorkspaceResult,
	FAvidScriptEditorCSharpBuildResult& OutBuildResult,
	FAvidScriptEditorComponentBindingResult& OutBindingResult)
{
	OutBindingResult = FAvidScriptEditorComponentBindingResult();
	if (!FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(
			WorkspaceConfig,
			OutWorkspaceResult))
	{
		SetAvidScriptCSharpWorkspaceBuildFailure(OutWorkspaceResult, OutBuildResult);
		return false;
	}

	return ExecuteCSharpProfileBuildAndBinding(
		OutWorkspaceResult.ProfilePath,
		OutBuildResult,
		OutBindingResult);
}

bool FAvidScriptEditorModule::ExecuteStartCSharpWorkspaceLiveReload(
	FAvidScriptEditorCSharpWorkspaceResult& OutWorkspaceResult,
	FAvidScriptEditorCSharpBuildResult& OutBuildResult,
	FAvidScriptEditorComponentBindingResult& OutBindingResult,
	FAvidScriptEditorCSharpLiveReloadServiceResult& OutLiveReloadResult)
{
	const FAvidScriptEditorCSharpWorkspaceConfig WorkspaceConfig;
	return ExecuteStartCSharpWorkspaceLiveReload(
		WorkspaceConfig,
		OutWorkspaceResult,
		OutBuildResult,
		OutBindingResult,
		OutLiveReloadResult);
}

bool FAvidScriptEditorModule::ExecuteStartCSharpWorkspaceLiveReload(
	const FAvidScriptEditorCSharpWorkspaceConfig& WorkspaceConfig,
	FAvidScriptEditorCSharpWorkspaceResult& OutWorkspaceResult,
	FAvidScriptEditorCSharpBuildResult& OutBuildResult,
	FAvidScriptEditorComponentBindingResult& OutBindingResult,
	FAvidScriptEditorCSharpLiveReloadServiceResult& OutLiveReloadResult)
{
	OutWorkspaceResult = FAvidScriptEditorCSharpWorkspaceResult();
	OutBuildResult = FAvidScriptEditorCSharpBuildResult();
	OutBindingResult = FAvidScriptEditorComponentBindingResult();
	OutLiveReloadResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
	if (!CSharpLiveReloadService)
	{
		CSharpLiveReloadService = MakeUnique<FAvidScriptEditorCSharpLiveReloadService>();
	}
	if (CSharpLiveReloadService->IsRunning())
	{
		const FAvidScriptEditorCSharpLiveReloadServiceResult ActiveResult =
			CSharpLiveReloadService->GetLastResult();
		OutLiveReloadResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
		OutLiveReloadResult.bSucceeded = false;
		OutLiveReloadResult.bRunning = true;
		OutLiveReloadResult.Status = EAvidScriptEditorCSharpLiveReloadServiceStatus::StartFailed;
		OutLiveReloadResult.WorkspaceRoot = ActiveResult.WorkspaceRoot;
		OutLiveReloadResult.ProfilePath = ActiveResult.ProfilePath;
		OutLiveReloadResult.TargetActorPath = ActiveResult.TargetActorPath;
		OutLiveReloadResult.ErrorCategory = TEXT("live_reload_already_running");
		OutLiveReloadResult.ErrorMessage = TEXT("Project C# Auto Live Reload is already running.");
		OutLiveReloadResult.NextAction = TEXT("stop the active watcher before starting it again");
		return false;
	}

	if (!ExecuteCSharpWorkspaceBuildAndBinding(
			WorkspaceConfig,
			OutWorkspaceResult,
			OutBuildResult,
			OutBindingResult))
	{
		const bool bBuildFailed = !OutBuildResult.bSucceeded;
		SetAvidScriptCSharpLiveReloadStartFailure(
			bBuildFailed
				? FString(TEXT("live_reload_initial_build_failed"))
				: FString(TEXT("live_reload_initial_binding_failed")),
			bBuildFailed ? OutBuildResult.ErrorCategory : OutBindingResult.ErrorCategory,
			bBuildFailed ? OutBuildResult.ErrorMessage : OutBindingResult.ErrorMessage,
			bBuildFailed ? OutBuildResult.NextAction : OutBindingResult.NextAction,
			OutWorkspaceResult,
			OutBindingResult,
			OutLiveReloadResult);
		return false;
	}

	UAvidScriptComponent* BoundComponent = OutBindingResult.Component;
	AActor* FixedTarget = BoundComponent != nullptr ? BoundComponent->GetOwner() : nullptr;
	if (!IsValid(FixedTarget) || FixedTarget->IsActorBeingDestroyed())
	{
		SetAvidScriptCSharpLiveReloadStartFailure(
			TEXT("live_reload_target_unavailable"),
			TEXT("binding_component_owner_invalid"),
			TEXT("The initial Project C# binding did not return a valid Actor owner."),
			TEXT("select a valid Actor and run Project C# Auto Live Reload again"),
			OutWorkspaceResult,
			OutBindingResult,
			OutLiveReloadResult);
		return false;
	}

	FAvidScriptEditorCSharpLiveReloadServiceConfig LiveReloadConfig;
	LiveReloadConfig.WorkspaceRoot = OutWorkspaceResult.WorkspaceRoot;
	LiveReloadConfig.ProfilePath = OutWorkspaceResult.ProfilePath;
	return CSharpLiveReloadService->Start(
		LiveReloadConfig,
		FixedTarget,
		OutLiveReloadResult);
}

bool FAvidScriptEditorModule::ExecuteStopCSharpWorkspaceLiveReload(
	FAvidScriptEditorCSharpLiveReloadServiceResult& OutLiveReloadResult)
{
	if (!CSharpLiveReloadService)
	{
		OutLiveReloadResult = FAvidScriptEditorCSharpLiveReloadServiceResult();
		OutLiveReloadResult.bSucceeded = true;
		OutLiveReloadResult.Status = EAvidScriptEditorCSharpLiveReloadServiceStatus::Stopped;
		return true;
	}

	CSharpLiveReloadService->Stop();
	OutLiveReloadResult = CSharpLiveReloadService->GetLastResult();
	return OutLiveReloadResult.bSucceeded;
}

bool FAvidScriptEditorModule::IsCSharpWorkspaceLiveReloadRunning() const
{
	return CSharpLiveReloadService && CSharpLiveReloadService->IsRunning();
}

void FAvidScriptEditorModule::RegisterDebuggerTab()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	if (FGlobalTabmanager::Get()->HasTabSpawner(GetDebuggerTabName()))
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GetDebuggerTabName());
	}
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(
		GetDebuggerTabName(),
		FOnSpawnTab::CreateRaw(this, &FAvidScriptEditorModule::SpawnDebuggerTab))
		.SetDisplayName(LOCTEXT("AvidScriptDebuggerTabTitle", "AvidScript Debugger"))
		.SetTooltipText(LOCTEXT(
			"AvidScriptDebuggerTabToolTip",
			"Debug live C# WebAssembly gameplay sessions."))
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "BlueprintDebugger.TabIcon"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void FAvidScriptEditorModule::UnregisterDebuggerTab()
{
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}
	if (const TSharedPtr<SDockTab> ExistingTab =
		FGlobalTabmanager::Get()->FindExistingLiveTab(GetDebuggerTabName()))
	{
		ExistingTab->RequestCloseTab();
	}
	if (FGlobalTabmanager::Get()->HasTabSpawner(GetDebuggerTabName()))
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GetDebuggerTabName());
	}
}

TSharedRef<SDockTab> FAvidScriptEditorModule::SpawnDebuggerTab(const FSpawnTabArgs& Args)
{
	check(DebugTargetController.IsValid());
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			MakeAvidScriptEditorDebugPanel(DebugTargetController.ToSharedRef())
		];
}

bool FAvidScriptEditorModule::TickDebugger(float DeltaTime)
{
	if (!DebugTargetController.IsValid() || !DebugTargetController->IsPIEActive())
	{
		return true;
	}
	FString Error;
	if (!DebugTargetController->Tick(Error))
	{
		if (!Error.IsEmpty() && Error != LastDebuggerTickError)
		{
			UE_LOG(LogAvidScriptEditor, Warning, TEXT("AvidScript debugger refresh failed: %s"), *Error);
		}
		LastDebuggerTickError = MoveTemp(Error);
		return true;
	}
	LastDebuggerTickError.Reset();
	return true;
}

void FAvidScriptEditorModule::HandleBeginPIE(bool bIsSimulating)
{
	if (DebugTargetController.IsValid())
	{
		DebugTargetController->HandleBeginPIE();
	}
}

void FAvidScriptEditorModule::HandleEndPIE(bool bIsSimulating)
{
	LastDebuggerTickError.Reset();
	if (DebugTargetController.IsValid())
	{
		DebugTargetController->HandleEndPIE();
	}
}

void FAvidScriptEditorModule::HandleOpenDebugger()
{
	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->TryInvokeTab(GetDebuggerTabName());
	}
}

void FAvidScriptEditorModule::RegisterMenus()
{
	FAvidScriptEditorMenuRegistrationResult Result;
	const FAvidScriptEditorMenuEntryConfig DebuggerMenuConfig = MakeDebuggerMenuEntryConfig(
		FSimpleDelegate::CreateRaw(this, &FAvidScriptEditorModule::HandleOpenDebugger));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(DebuggerMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}

	const FAvidScriptEditorMenuEntryConfig SampleMenuConfig = MakeSampleMenuEntryConfig(
		FSimpleDelegate::CreateRaw(this, &FAvidScriptEditorModule::HandleRunSampleCommand));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(SampleMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}

	const FAvidScriptEditorMenuEntryConfig CSharpBuildAndBindMenuConfig = MakeCSharpActorLifecycleBuildAndBindMenuEntryConfig(
		FSimpleDelegate::CreateRaw(this, &FAvidScriptEditorModule::HandleBuildAndBindCSharpActorLifecycleReport));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(CSharpBuildAndBindMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}


	const FAvidScriptEditorMenuEntryConfig CSharpWorkspaceCreateMenuConfig = MakeCSharpWorkspaceCreateMenuEntryConfig(
		FSimpleDelegate::CreateRaw(this, &FAvidScriptEditorModule::HandleCreateCSharpWorkspace));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(CSharpWorkspaceCreateMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}

	const FAvidScriptEditorMenuEntryConfig CSharpWorkspaceBuildAndBindMenuConfig = MakeCSharpWorkspaceBuildAndBindMenuEntryConfig(
		FSimpleDelegate::CreateRaw(this, &FAvidScriptEditorModule::HandleBuildAndBindCSharpWorkspace));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(CSharpWorkspaceBuildAndBindMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}

	const FAvidScriptEditorMenuEntryConfig CSharpLiveReloadStartMenuConfig =
		MakeCSharpWorkspaceLiveReloadStartMenuEntryConfig(
			FSimpleDelegate::CreateRaw(
				this,
				&FAvidScriptEditorModule::HandleStartCSharpWorkspaceLiveReload));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(CSharpLiveReloadStartMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}

	const FAvidScriptEditorMenuEntryConfig CSharpLiveReloadStopMenuConfig =
		MakeCSharpWorkspaceLiveReloadStopMenuEntryConfig(
			FSimpleDelegate::CreateRaw(
				this,
				&FAvidScriptEditorModule::HandleStopCSharpWorkspaceLiveReload));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(CSharpLiveReloadStopMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}

	const FAvidScriptEditorMenuEntryConfig CSharpProfileTemplateMenuConfig = MakeCSharpProfileTemplateMenuEntryConfig(
		FSimpleDelegate::CreateRaw(this, &FAvidScriptEditorModule::HandleCreateDefaultCSharpProfileTemplate));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(CSharpProfileTemplateMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}
	const FAvidScriptEditorMenuEntryConfig CSharpProfileBuildAndBindMenuConfig = MakeCSharpProfileBuildAndBindMenuEntryConfig(
		FSimpleDelegate::CreateRaw(this, &FAvidScriptEditorModule::HandleBuildAndBindCSharpProfile));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(CSharpProfileBuildAndBindMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}
	const FAvidScriptEditorMenuEntryConfig CSharpBindMenuConfig = MakeCSharpActorLifecycleBindMenuEntryConfig(
		FSimpleDelegate::CreateRaw(this, &FAvidScriptEditorModule::HandleBindCSharpActorLifecycleReport));
	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(CSharpBindMenuConfig, Result))
	{
		LogAvidScriptMenuRegistrationFailure(Result);
	}
}

void FAvidScriptEditorModule::HandleRunSampleCommand()
{
	FAvidScriptEditorCommandLaunchResult Result;
	ExecuteSampleCommand(Result);

	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakePresentation(Result);
	LogAvidScriptEditorPresentation(Presentation);
}

void FAvidScriptEditorModule::HandleBindCSharpActorLifecycleReport()
{
	FAvidScriptEditorComponentBindingResult Result;
	if (ExecuteCSharpActorLifecycleBinding(Result))
	{
		UE_LOG(
			LogAvidScriptEditor,
			Display,
			TEXT("C# ActorLifecycle binding applied: actor=%s manifest=%s report=%s"),
			*Result.ActorPath,
			*Result.NormalizedManifestPath,
			*Result.ReportPath);
		return;
	}

	UE_LOG(
		LogAvidScriptEditor,
		Warning,
		TEXT("C# ActorLifecycle binding failed: category=%s message=%s next=%s report=%s"),
		*Result.ErrorCategory,
		*Result.ErrorMessage,
		*Result.NextAction,
		*Result.ReportPath);
}

void FAvidScriptEditorModule::HandleBuildAndBindCSharpActorLifecycleReport()
{
	FAvidScriptEditorCSharpBuildResult BuildResult;
	FAvidScriptEditorComponentBindingResult BindingResult;
	if (ExecuteCSharpActorLifecycleBuildAndBinding(BuildResult, BindingResult))
	{
		UE_LOG(
			LogAvidScriptEditor,
			Display,
			TEXT("C# ActorLifecycle build-and-bind applied: actor=%s manifest=%s report=%s"),
			*BindingResult.ActorPath,
			*BindingResult.NormalizedManifestPath,
			*BindingResult.ReportPath);
		return;
	}

	if (!BuildResult.bSucceeded)
	{
		UE_LOG(
			LogAvidScriptEditor,
			Warning,
			TEXT("C# ActorLifecycle build failed: category=%s message=%s exit=%d report=%s stderr=%s"),
			*BuildResult.ErrorCategory,
			*BuildResult.ErrorMessage,
			BuildResult.ProcessExitCode,
			*BuildResult.ReportPath,
			*BuildResult.Stderr);
		return;
	}

	UE_LOG(
		LogAvidScriptEditor,
		Warning,
		TEXT("C# ActorLifecycle build succeeded but binding failed: category=%s message=%s next=%s report=%s"),
		*BindingResult.ErrorCategory,
		*BindingResult.ErrorMessage,
		*BindingResult.NextAction,
		*BindingResult.ReportPath);
}

void FAvidScriptEditorModule::HandleCreateDefaultCSharpProfileTemplate()
{
	FAvidScriptEditorCSharpProfileTemplateResult Result;
	FAvidScriptEditorCSharpProfileService::WriteDefaultProfileTemplate(Result, false);

	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakeCSharpProfileTemplatePresentation(Result);
	LogAvidScriptEditorPresentation(Presentation);
}

void FAvidScriptEditorModule::HandleBuildAndBindCSharpProfile()
{
	const FString ProfilePath = GetDefaultCSharpProfilePath();
	FAvidScriptEditorCSharpBuildResult BuildResult;
	FAvidScriptEditorComponentBindingResult BindingResult;
	ExecuteCSharpProfileBuildAndBinding(ProfilePath, BuildResult, BindingResult);

	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakeCSharpProfileBuildAndBindPresentation(ProfilePath, BuildResult, BindingResult);
	LogAvidScriptEditorPresentation(Presentation);
}

void FAvidScriptEditorModule::HandleCreateCSharpWorkspace()
{
	FAvidScriptEditorCSharpWorkspaceResult WorkspaceResult;
	ExecuteCreateCSharpWorkspace(WorkspaceResult, false);
	LogAvidScriptEditorPresentation(
		FAvidScriptEditorResultPresenter::MakeCSharpWorkspacePresentation(WorkspaceResult));
}

void FAvidScriptEditorModule::HandleBuildAndBindCSharpWorkspace()
{
	FAvidScriptEditorCSharpBuildResult BuildResult;
	FAvidScriptEditorComponentBindingResult BindingResult;
	ExecuteCSharpWorkspaceBuildAndBinding(BuildResult, BindingResult);
	LogAvidScriptEditorPresentation(
		FAvidScriptEditorResultPresenter::MakeCSharpProfileBuildAndBindPresentation(
			GetProjectCSharpWorkspaceProfilePath(),
			BuildResult,
			BindingResult));
}

void FAvidScriptEditorModule::HandleStartCSharpWorkspaceLiveReload()
{
	FAvidScriptEditorCSharpWorkspaceResult WorkspaceResult;
	FAvidScriptEditorCSharpBuildResult BuildResult;
	FAvidScriptEditorComponentBindingResult BindingResult;
	FAvidScriptEditorCSharpLiveReloadServiceResult LiveReloadResult;
	if (ExecuteStartCSharpWorkspaceLiveReload(
			WorkspaceResult,
			BuildResult,
			BindingResult,
			LiveReloadResult))
	{
		UE_LOG(
			LogAvidScriptEditor,
			Display,
			TEXT("Project C# Auto Live Reload started: actor=%s workspace=%s profile=%s"),
			*LiveReloadResult.TargetActorPath,
			*LiveReloadResult.WorkspaceRoot,
			*LiveReloadResult.ProfilePath);
		return;
	}

	UE_LOG(
		LogAvidScriptEditor,
		Warning,
		TEXT("Project C# Auto Live Reload start failed: category=%s cause=%s message=%s next=%s"),
		*LiveReloadResult.ErrorCategory,
		*LiveReloadResult.CauseErrorCategory,
		*LiveReloadResult.ErrorMessage,
		*LiveReloadResult.NextAction);
}

void FAvidScriptEditorModule::HandleStopCSharpWorkspaceLiveReload()
{
	FAvidScriptEditorCSharpLiveReloadServiceResult LiveReloadResult;
	ExecuteStopCSharpWorkspaceLiveReload(LiveReloadResult);
	UE_LOG(
		LogAvidScriptEditor,
		Display,
		TEXT("Project C# Auto Live Reload stopped: actor=%s workspace=%s"),
		*LiveReloadResult.TargetActorPath,
		*LiveReloadResult.WorkspaceRoot);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAvidScriptEditorModule, AvidScriptEditor)
