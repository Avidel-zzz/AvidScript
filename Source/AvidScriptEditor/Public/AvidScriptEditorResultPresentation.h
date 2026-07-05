#pragma once

#include "AvidScriptEditorCommandLauncher.h"

#include "CoreMinimal.h"

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
};

class FAvidScriptEditorResultPresenter
{
public:
	static FAvidScriptEditorCommandPresentation MakePresentation(
		const FAvidScriptEditorCommandLaunchResult& Result);
};