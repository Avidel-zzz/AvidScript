#pragma once

#include "AvidScriptEditorCommandLauncher.h"
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

	static bool MakeSampleCommandConfig(
		FAvidScriptEditorCommandLaunchConfig& OutConfig,
		FString& OutErrorMessage);

	static bool MakeCommandConfigForSource(
		const FString& SourcePath,
		const FAvidScriptEditorToolchainSettings& Settings,
		FAvidScriptEditorCommandLaunchConfig& OutConfig,
		FString& OutErrorMessage);

	static FAvidScriptEditorMenuEntryConfig MakeSampleMenuEntryConfig(FSimpleDelegate ExecuteAction);

	bool ExecuteSampleCommand(FAvidScriptEditorCommandLaunchResult& OutResult);

private:
	void RegisterMenus();
	void HandleRunSampleCommand();

	TUniquePtr<FAvidScriptEditorCommandLauncher> CommandLauncher;
	FDelegateHandle ToolMenusStartupCallbackHandle;
};
