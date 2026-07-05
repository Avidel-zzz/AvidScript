#pragma once

#include "CoreMinimal.h"

struct FAvidScriptEditorMenuEntryConfig
{
	FName OwnerName;
	FName MenuName;
	FName SectionName;
	FName EntryName;
	FText SectionLabel;
	FText Label;
	FText ToolTip;
	FSimpleDelegate ExecuteAction;
};

struct FAvidScriptEditorMenuRegistrationResult
{
	bool bSucceeded = false;
	FName MenuName;
	FName SectionName;
	FName EntryName;
	FString ErrorCategory;
	FString ErrorMessage;
};

class FAvidScriptEditorMenuRegistrar
{
public:
	static bool RegisterMenuEntry(
		const FAvidScriptEditorMenuEntryConfig& Config,
		FAvidScriptEditorMenuRegistrationResult& OutResult);
};
