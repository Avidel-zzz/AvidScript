#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptEditorIdeKind : uint8
{
	SystemDefault,
	VisualStudio,
	Rider,
	VisualStudioCode
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorIdeLaunchConfig
{
	EAvidScriptEditorIdeKind Ide = EAvidScriptEditorIdeKind::SystemDefault;
	FString ProjectRoot;
	FString WorkspaceRoot;
	FString SolutionPath;
	FString ProjectPath;
	FString ExecutableOverride;
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorIdeLaunchResult
{
	bool bSucceeded = false;
	EAvidScriptEditorIdeKind Ide = EAvidScriptEditorIdeKind::SystemDefault;
	FString TargetPath;
	FString ExecutablePath;
	FString Arguments;
	FString WorkingDirectory;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
};

class AVIDSCRIPTEDITOR_API IAvidScriptEditorIdeLaunchHost
{
public:
	virtual ~IAvidScriptEditorIdeLaunchHost() = default;

	virtual bool ResolveExecutable(
		EAvidScriptEditorIdeKind Ide,
		FString& OutExecutablePath,
		FString& OutErrorMessage) = 0;

	virtual bool LaunchFile(
		const FString& Path,
		FString& OutErrorMessage) = 0;

	virtual bool LaunchProcess(
		const FString& ExecutablePath,
		const FString& Arguments,
		const FString& WorkingDirectory,
		FString& OutErrorMessage) = 0;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorIdeLaunchService
{
public:
	static FString GetIdeName(EAvidScriptEditorIdeKind Ide);

	static bool Launch(
		const FAvidScriptEditorIdeLaunchConfig& Config,
		FAvidScriptEditorIdeLaunchResult& OutResult,
		IAvidScriptEditorIdeLaunchHost* HostOverride = nullptr);
};
