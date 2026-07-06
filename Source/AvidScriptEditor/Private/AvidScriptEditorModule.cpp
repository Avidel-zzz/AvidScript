#include "AvidScriptEditorModule.h"

#include "AvidScriptEditorResultPresentation.h"
#include "AvidScriptEditorSourceConfig.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"

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
} // namespace

void FAvidScriptEditorModule::StartupModule()
{
	CommandLauncher = MakeUnique<FAvidScriptEditorCommandLauncher>();
	ToolMenusStartupCallbackHandle = UToolMenus::RegisterStartupCallback(
		FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FAvidScriptEditorModule::RegisterMenus));

	UE_LOG(LogAvidScriptEditor, Display, TEXT("AvidScriptEditor module started."));
}

void FAvidScriptEditorModule::ShutdownModule()
{
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

	UE_LOG(LogAvidScriptEditor, Display, TEXT("AvidScriptEditor module stopped."));
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

FString FAvidScriptEditorModule::GetCSharpActorLifecycleReportPath()
{
	return FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleReportPath();
}

FString FAvidScriptEditorModule::GetCSharpActorLifecycleBuildScriptPath()
{
	return FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
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

void FAvidScriptEditorModule::RegisterMenus()
{
	FAvidScriptEditorMenuRegistrationResult Result;
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
#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAvidScriptEditorModule, AvidScriptEditor)
