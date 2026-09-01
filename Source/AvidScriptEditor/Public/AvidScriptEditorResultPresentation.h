#pragma once

#include "AvidScriptEditorCallStack.h"
#include "AvidScriptEditorCommandLauncher.h"

#include "CoreMinimal.h"

struct FAvidScriptEditorComponentBindingResult;
struct FAvidScriptEditorCSharpBuildResult;
struct FAvidScriptEditorCSharpProfileTemplateResult;
struct FAvidScriptEditorCSharpWorkspaceResult;
struct FAvidScriptEditorIdeLaunchResult;

enum class EAvidScriptEditorPresentationSeverity : uint8
{
	Info,
	Warning,
	Error
};

struct FAvidScriptEditorCommandPresentation
{
	EAvidScriptEditorPresentationSeverity Severity = EAvidScriptEditorPresentationSeverity::Info;
	FString Title;
	FString Body;
	FString Details;
	FString SourcePath;
	FString ManifestPath;
	TArray<FAvidScriptEditorCallStackFrame> CallStack;
};

class FAvidScriptEditorResultPresenter
{
public:
	static FAvidScriptEditorCommandPresentation MakePresentation(
		const FAvidScriptEditorCommandLaunchResult& Result);

	static FAvidScriptEditorCommandPresentation MakeCSharpProfileTemplatePresentation(
		const FAvidScriptEditorCSharpProfileTemplateResult& Result);

	static FAvidScriptEditorCommandPresentation MakeCSharpWorkspacePresentation(
		const FAvidScriptEditorCSharpWorkspaceResult& Result);
	static FAvidScriptEditorCommandPresentation MakeIdeLaunchPresentation(
		const FAvidScriptEditorIdeLaunchResult& Result);

	static FAvidScriptEditorCommandPresentation MakeCSharpProfileBuildAndBindPresentation(
		const FString& ProfilePath,
		const FAvidScriptEditorCSharpBuildResult& BuildResult,
		const FAvidScriptEditorComponentBindingResult& BindingResult);
};
