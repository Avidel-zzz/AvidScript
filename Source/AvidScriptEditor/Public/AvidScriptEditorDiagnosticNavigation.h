#pragma once

#include "AvidScriptFrontendReport.h"

#include "CoreMinimal.h"

struct FAvidScriptEditorDiagnosticNavigationResult
{
	bool bSucceeded = false;
	FString ErrorCategory;
	FString ErrorMessage;
	FString AbsoluteSourcePath;
	int32 Line = 0;
	int32 Column = 0;
};

struct FAvidScriptEditorSourceLocation
{
	FString File;
	FString SourceSha256;
	int32 Line = 0;
	int32 Column = 0;

	bool IsValid() const
	{
		return !File.IsEmpty() && Line > 0 && Column > 0;
	}
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorDiagnosticNavigation
{
public:
	static bool Resolve(
		const FAvidScriptEditorSourceLocation& Location,
		const FString& ProjectDirectory,
		FAvidScriptEditorDiagnosticNavigationResult& OutResult);

	static bool Open(
		const FAvidScriptEditorSourceLocation& Location,
		const FString& ProjectDirectory,
		FAvidScriptEditorDiagnosticNavigationResult& OutResult);

	static bool Resolve(
		const FAvidScriptFrontendDiagnostic& Diagnostic,
		const FString& ProjectDirectory,
		FAvidScriptEditorDiagnosticNavigationResult& OutResult);

	static bool Open(
		const FAvidScriptFrontendDiagnostic& Diagnostic,
		const FString& ProjectDirectory,
		FAvidScriptEditorDiagnosticNavigationResult& OutResult);
};
