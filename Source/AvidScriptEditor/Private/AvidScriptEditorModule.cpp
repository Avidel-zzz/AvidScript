#include "AvidScriptEditorModule.h"

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

bool FAvidScriptEditorModule::MakeSampleCommandConfig(
	FAvidScriptEditorCommandLaunchConfig& OutConfig,
	FString& OutErrorMessage)
{
	return FAvidScriptEditorCommandLauncher::MakeDefaultConfigForSource(
		GetSampleCommandSourcePath(),
		OutConfig,
		OutErrorMessage);
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

void FAvidScriptEditorModule::RegisterMenus()
{
	FAvidScriptEditorMenuRegistrationResult Result;
	const FAvidScriptEditorMenuEntryConfig MenuConfig = MakeSampleMenuEntryConfig(
		FSimpleDelegate::CreateRaw(this, &FAvidScriptEditorModule::HandleRunSampleCommand));

	if (!FAvidScriptEditorMenuRegistrar::RegisterMenuEntry(MenuConfig, Result))
	{
		UE_LOG(
			LogAvidScriptEditor,
			Warning,
			TEXT("AvidScript sample menu registration failed: category=%s message=%s"),
			*Result.ErrorCategory,
			*Result.ErrorMessage);
	}
}

void FAvidScriptEditorModule::HandleRunSampleCommand()
{
	FAvidScriptEditorCommandLaunchResult Result;
	if (ExecuteSampleCommand(Result))
	{
		UE_LOG(LogAvidScriptEditor, Display, TEXT("AvidScript sample command succeeded: %s"), *Result.Summary);
		return;
	}

	UE_LOG(LogAvidScriptEditor, Warning, TEXT("AvidScript sample command failed: %s"), *Result.Summary);
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FAvidScriptEditorModule, AvidScriptEditor)
