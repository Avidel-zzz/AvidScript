#pragma once

#include "CoreMinimal.h"

struct FAvidScriptFrontendDiagnostic
{
	FString Code;
	FString Severity;
	FString File;
	int32 Start = INDEX_NONE;
	int32 Length = 0;
	int32 Line = 0;
	int32 Column = 0;
	int32 EndLine = 0;
	int32 EndColumn = 0;
	FString Message;

	bool IsError() const;
};

struct FAvidScriptFrontendBuildEvent
{
	FString Result;
	TMap<FString, FString> Fields;
};

struct FAvidScriptFrontendReport
{
	int32 SchemaVersion = 0;
	FString Source;
	FString SourceSha256;
	FString ScriptType;
	FString FrontendArtifact;
	int32 FrontendSchemaVersion = 0;
	FString FrontendVersion;
	FString Bindings;
	FString OutputRoot;
	int32 ExitCode = 0;
	bool bSucceeded = false;
	TArray<FAvidScriptFrontendDiagnostic> Diagnostics;
	TArray<FAvidScriptFrontendBuildEvent> BuildEvents;
	TArray<FString> RawOutput;

	const FAvidScriptFrontendBuildEvent* GetLastBuildEvent() const;
	bool HasErrorDiagnostics() const;
};

struct FAvidScriptFrontendReportLoadResult
{
	bool bSucceeded = false;
	FString ErrorCategory;
	FString ErrorMessage;
	FString ReportPath;
};

class FAvidScriptFrontendReportReader
{
public:
	static bool LoadFromFile(
		const FString& ReportPath,
		FAvidScriptFrontendReport& OutReport,
		FAvidScriptFrontendReportLoadResult& OutResult);
};