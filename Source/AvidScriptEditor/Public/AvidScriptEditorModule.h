#pragma once

#include "AvidScriptEditorCommandLauncher.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorCSharpWorkspaceService.h"
#include "AvidScriptEditorMenuRegistrar.h"
#include "AvidScriptEditorSettingsService.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadService.h"

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"

DECLARE_LOG_CATEGORY_EXTERN(LogAvidScriptEditor, Log, All);

class IConsoleObject;

class FAvidScriptEditorModule final : public IModuleInterface
{
public:
	FAvidScriptEditorModule() = default;
	explicit FAvidScriptEditorModule(
		TUniquePtr<FAvidScriptEditorCSharpLiveReloadService> InCSharpLiveReloadService);

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
	static FName GetCSharpWorkspaceCreateEntryName();
	static FName GetCSharpWorkspaceBuildAndBindEntryName();
	static FName GetCSharpWorkspaceLiveReloadStartEntryName();
	static FName GetCSharpWorkspaceLiveReloadStopEntryName();
	static FString GetCSharpActorLifecycleReportPath();
	static FString GetCSharpActorLifecycleBuildScriptPath();
	static FString GetDefaultCSharpProfilePath();

	static FString GetProjectCSharpWorkspaceProfilePath();
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

	static FAvidScriptEditorMenuEntryConfig MakeCSharpWorkspaceCreateMenuEntryConfig(FSimpleDelegate ExecuteAction);
	static FAvidScriptEditorMenuEntryConfig MakeCSharpWorkspaceBuildAndBindMenuEntryConfig(FSimpleDelegate ExecuteAction);
	static FAvidScriptEditorMenuEntryConfig MakeCSharpWorkspaceLiveReloadStartMenuEntryConfig(FSimpleDelegate ExecuteAction);
	static FAvidScriptEditorMenuEntryConfig MakeCSharpWorkspaceLiveReloadStopMenuEntryConfig(FSimpleDelegate ExecuteAction);
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

	bool ExecuteCreateCSharpWorkspace(
		FAvidScriptEditorCSharpWorkspaceResult& OutResult,
		bool bOverwriteUserFiles = false);
	bool ExecuteCSharpWorkspaceBuildAndBinding(
		FAvidScriptEditorCSharpBuildResult& OutBuildResult,
		FAvidScriptEditorComponentBindingResult& OutBindingResult);
	bool ExecuteCSharpWorkspaceBuildAndBinding(
		const FAvidScriptEditorCSharpWorkspaceConfig& WorkspaceConfig,
		FAvidScriptEditorCSharpWorkspaceResult& OutWorkspaceResult,
		FAvidScriptEditorCSharpBuildResult& OutBuildResult,
		FAvidScriptEditorComponentBindingResult& OutBindingResult);
	bool ExecuteStartCSharpWorkspaceLiveReload(
		FAvidScriptEditorCSharpWorkspaceResult& OutWorkspaceResult,
		FAvidScriptEditorCSharpBuildResult& OutBuildResult,
		FAvidScriptEditorComponentBindingResult& OutBindingResult,
		FAvidScriptEditorCSharpLiveReloadServiceResult& OutLiveReloadResult);
	bool ExecuteStartCSharpWorkspaceLiveReload(
		const FAvidScriptEditorCSharpWorkspaceConfig& WorkspaceConfig,
		FAvidScriptEditorCSharpWorkspaceResult& OutWorkspaceResult,
		FAvidScriptEditorCSharpBuildResult& OutBuildResult,
		FAvidScriptEditorComponentBindingResult& OutBindingResult,
		FAvidScriptEditorCSharpLiveReloadServiceResult& OutLiveReloadResult);
	bool ExecuteStopCSharpWorkspaceLiveReload(
		FAvidScriptEditorCSharpLiveReloadServiceResult& OutLiveReloadResult);
	bool IsCSharpWorkspaceLiveReloadRunning() const;

private:
	void RegisterConsoleCommands();
	void UnregisterConsoleCommands();
	void HandleGenerateBindingsConsoleCommand(const TArray<FString>& Arguments);
	void HandlePublishCSharpBindingsConsoleCommand(const TArray<FString>& Arguments);
	void HandleAssetRegistryReadyForCSharpBindings();
	bool ExecutePublishCSharpBindings(const FString& OutputRoot);
	void RegisterMenus();
	void HandleRunSampleCommand();
	void HandleBindCSharpActorLifecycleReport();
	void HandleBuildAndBindCSharpActorLifecycleReport();
	void HandleBuildAndBindCSharpProfile();
	void HandleCreateDefaultCSharpProfileTemplate();
	void HandleCreateCSharpWorkspace();
	void HandleBuildAndBindCSharpWorkspace();
	void HandleStartCSharpWorkspaceLiveReload();
	void HandleStopCSharpWorkspaceLiveReload();

	TUniquePtr<FAvidScriptEditorCommandLauncher> CommandLauncher;
	TUniquePtr<FAvidScriptEditorCSharpLiveReloadService> CSharpLiveReloadService;
	IConsoleObject* GenerateBindingsConsoleCommand = nullptr;
	IConsoleObject* PublishCSharpBindingsConsoleCommand = nullptr;
	FDelegateHandle PublishCSharpBindingsAssetRegistryHandle;
	FString PendingCSharpBindingsOutputRoot;
	bool bExitAfterCSharpBindingsPublish = false;
	FDelegateHandle ToolMenusStartupCallbackHandle;
};
