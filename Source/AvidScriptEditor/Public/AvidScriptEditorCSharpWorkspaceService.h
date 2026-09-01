#pragma once

#include "CoreMinimal.h"

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpWorkspaceConfig
{
    FString WorkspaceRoot;
    FString TemplateRoot;
    FString GeneratedRoot;
    FString BindingPackageRoot;
    FString OutputRoot;
    bool bOverwriteUserFiles = false;
};

struct AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpWorkspaceResult
{
    bool bSucceeded = false;
    bool bSourceCreated = false;
    bool bProjectCreated = false;
    bool bSolutionCreated = false;
    bool bEditorConfigCreated = false;
    bool bProfileCreated = false;
    bool bGlobalJsonCreated = false;
    bool bFacadeRefreshed = false;
    bool bSourceIndexRefreshed = false;
    int32 CreatedUserFileCount = 0;
    int32 UpdatedUserFileCount = 0;
    int32 PreservedUserFileCount = 0;
    FString WorkspaceRoot;
    FString SourcePath;
    FString ProjectPath;
    FString SolutionPath;
    FString EditorConfigPath;
    FString ProfilePath;
    FString GlobalJsonPath;
    FString GeneratedRoot;
    FString FacadePath;
    FString SourceIndexPath;
    FString SourceIndexSha256;
    FString BindingPackageRoot;
    FString BindingPackageManifestPath;
    FString BindingPackageHash;
    FString OutputRoot;
    FString ReportPath;
    FString ManifestPath;
    FString ErrorCategory;
    FString ErrorMessage;
    FString NextAction;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpWorkspaceService
{
public:
    static FString GetDefaultWorkspaceRoot();
    static FString GetDefaultTemplateRoot();
    static FString GetDefaultGeneratedRoot();
    static FString GetDefaultBindingPackageRoot();
    static FString GetDefaultOutputRoot();
    static FString GetDefaultSourcePath();
    static FString GetDefaultProjectPath();
    static FString GetDefaultSolutionPath();
    static FString GetDefaultEditorConfigPath();
    static FString GetDefaultProfilePath();
    static FString GetDefaultGlobalJsonPath();
    static FString GetDefaultFacadePath();

    static bool CreateOrRefresh(
        const FAvidScriptEditorCSharpWorkspaceConfig& Config,
        FAvidScriptEditorCSharpWorkspaceResult& OutResult);
};
