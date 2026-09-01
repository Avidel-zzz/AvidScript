#pragma once

#include "CoreMinimal.h"

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpSourceIndexEntry
{
	FString SourceId;
	FString Sha256;
	FString Kind;
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpSourceIndex
{
	int32 SchemaVersion = 0;
	FString WorkspaceId;
	FString SolutionId;
	FString ProjectId;
	TArray<FAvidScriptEditorCSharpSourceIndexEntry> Sources;
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpSourceIndexConfig
{
	FString ProjectRoot;
	FString WorkspaceRoot;
	FString SolutionPath;
	FString ProjectPath;
	FString UserSourcePath;
	FString GeneratedSourcePath;
	FString OutputPath;
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpSourceIndexResult
{
	bool bSucceeded = false;
	FString IndexPath;
	FString IndexSha256;
	int32 SourceCount = 0;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpSourceIndexService
{
public:
	static FString MakeDefaultPath(const FString& GeneratedRoot);

	static bool Publish(
		const FAvidScriptEditorCSharpSourceIndexConfig& Config,
		FAvidScriptEditorCSharpSourceIndexResult& OutResult);

	static bool Load(
		const FString& IndexPath,
		FAvidScriptEditorCSharpSourceIndex& OutIndex,
		FAvidScriptEditorCSharpSourceIndexResult& OutResult);
};
