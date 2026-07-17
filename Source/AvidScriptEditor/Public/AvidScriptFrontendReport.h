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

struct FAvidScriptFrontendBindingImport
{
	FString StableId;
	int32 Ordinal = INDEX_NONE;
	FString Module;
	FString Name;
	FString Signature;
};

struct FAvidScriptFrontendBindingPackage
{
	bool bPresent = false;
	bool bRequired = false;
	FString PackageManifest;
	FString PackageName;
	FString PackageHash;
	FString DescriptorFile;
	FString DescriptorSha256;
	FString ReferenceSourceFile;
	FString ReferenceSourceSha256;
	int32 ProfileImportCount = 0;
	int32 UsedImportCount = 0;
	TArray<FAvidScriptFrontendBindingImport> UsedImports;
};

struct FAvidScriptFrontendReport
{
	int32 SchemaVersion = 0;
	FString Result;
	FString Source;
	FString SourceSha256;
	FString ScriptType;
	FString FrontendArtifact;
	int32 FrontendSchemaVersion = 0;
	FString FrontendVersion;
	FString SemanticArtifact;
	int32 SemanticSchemaVersion = 0;
	FString SemanticVersion;
	bool bSemanticSucceeded = false;
	FString SemanticSourceSha256;
	FString SemanticFrontendSha256;
	int32 SemanticDiagnosticCount = 0;
	FString GuestIrArtifact;
	int32 GuestIrSchemaVersion = 0;
	FString GuestIrVersion;
	bool bGuestIrSucceeded = false;
	FString GuestIrSemanticSha256;
	FString GuestIrSha256;
	FString Bindings;
	FString OutputRoot;
	FAvidScriptFrontendBindingPackage BindingPackage;
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