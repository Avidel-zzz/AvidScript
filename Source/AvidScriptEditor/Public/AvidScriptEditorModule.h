#pragma once

#include "AvidScriptEditorCommandLauncher.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorMenuRegistrar.h"
#include "AvidScriptEditorSettingsService.h"

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAvidScriptEditor, Log, All);

class FAvidScriptEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	static FName GetToolMenuOwnerName();
	static FName GetSampleCommandMenuName();
	static FName GetSampleCommandSectionName();
	static FName GetSampleCommandEntryName();
	static FString GetSampleCommandSourcePath();
	static FName GetCSharpActorLifecycleBindEntryName();
	static FName GetCSharpActorLifecycleBuildAndBindEntryName();
	static FName GetCSharpProfileBuildAndBindEntryName();
	static FName GetCSharpProfileTemplateEntryName();
	static FString GetCSharpActorLifecycleReportPath();
	static FString GetCSharpActorLifecycleBuildScriptPath();
	static FString GetDefaultCSharpProfilePath();

	static bool MakeSampleCommandConfig(
		FAvidScriptEditorCommandLaunchConfig& OutConfig,
		FString& OutErrorMessage);

	static bool MakeCommandConfigForSource(
		const FString& SourcePath,
		const FAvidScriptEditorToolchainSettings& Settings,
		FAvidScriptEditorCommandLaunchConfig& OutConfig,
		FString& OutErrorMessage);

	static FAvidScriptEditorMenuEntryConfig MakeSampleMenuEntryConfig(FSimpleDelegate ExecuteAction);
	static FAvidScriptEditorMenuEntryConfig MakeCSharpActorLifecycleBindMenuEntryConfig(FSimpleDelegate ExecuteAction);
	static FAvidScriptEditorMenuEntryConfig MakeCSharpActorLifecycleBuildAndBindMenuEntryConfig(FSimpleDelegate ExecuteAction);
	static FAvidScriptEditorMenuEntryConfig MakeCSharpProfileBuildAndBindMenuEntryConfig(FSimpleDelegate ExecuteAction);
	static FAvidScriptEditorMenuEntryConfig MakeCSharpProfileTemplateMenuEntryConfig(FSimpleDelegate ExecuteAction);

	bool ExecuteSampleCommand(FAvidScriptEditorCommandLaunchResult& OutResult);
	bool ExecuteCSharpActorLifecycleBinding(FAvidScriptEditorComponentBindingResult& OutResult);
	bool ExecuteCSharpActorLifecycleBuildAndBinding(
		FAvidScriptEditorCSharpBuildResult& OutBuildResult,
		FAvidScriptEditorComponentBindingResult& OutBindingResult);
	bool ExecuteCSharpProfileBuildAndBinding(
		const FString& ProfilePath,
		FAvidScriptEditorCSharpBuildResult& OutBuildResult,
		FAvidScriptEditorComponentBindingResult& OutBindingResult);
	bool ExecuteCreateCSharpProfileTemplate(
		const FString& ProfilePath,
		FAvidScriptEditorCSharpProfileTemplateResult& OutResult,
		bool bOverwrite = false);

private:
	void RegisterMenus();
	void HandleRunSampleCommand();
	void HandleBindCSharpActorLifecycleReport();
	void HandleBuildAndBindCSharpActorLifecycleReport();
	void HandleBuildAndBindCSharpProfile();
	void HandleCreateDefaultCSharpProfileTemplate();

	TUniquePtr<FAvidScriptEditorCommandLauncher> CommandLauncher;
	FDelegateHandle ToolMenusStartupCallbackHandle;
};
