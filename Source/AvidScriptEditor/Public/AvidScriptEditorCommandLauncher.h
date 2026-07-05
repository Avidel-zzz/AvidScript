#pragma once

#include "AvidScriptEditorCommandService.h"
#include "AvidScriptWasmRuntime.h"

#include "CoreMinimal.h"

struct FAvidScriptEditorCommandLaunchConfig
{
	FString SourcePath;
	FString BindingsPath;
	FString OutputRoot;
	FString ReportPath;
	FString Ldc2Path;
	FString ToolchainRoot;
	bool bSkipCompile = false;
};

struct FAvidScriptEditorCommandLaunchResult
{
	bool bSucceeded = false;
	bool bReloadApplied = false;
	FString SourcePath;
	FString BindingsPath;
	FString OutputRoot;
	FString ReportPath;
	FString ManifestPath;
	FString Summary;
	FAvidScriptEditorCommandResult CommandResult;
};

class FAvidScriptEditorCommandLauncher
{
public:
	static bool MakeDefaultConfigForSource(
		const FString& SourcePath,
		FAvidScriptEditorCommandLaunchConfig& OutConfig,
		FString& OutErrorMessage);

	void SetHostContext(const FAvidScriptWasmHostContext& HostContext);

	const FAvidScriptWasmReloadSession& GetReloadSession() const;
	FAvidScriptWasmReloadSession& GetMutableReloadSession();

	bool CompileSourceAndApply(
		const FAvidScriptEditorCommandLaunchConfig& Config,
		FAvidScriptEditorCommandLaunchResult& OutResult);

private:
	FAvidScriptWasmReloadSession ReloadSession;
};
