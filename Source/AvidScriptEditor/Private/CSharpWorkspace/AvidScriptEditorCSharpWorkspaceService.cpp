#include "AvidScriptEditorCSharpWorkspaceService.h"

#include "AvidScriptEditorCSharpBindingEmitter.h"
#include "AvidScriptEditorCSharpBuildService.h"

#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"

namespace
{
constexpr const TCHAR* AvidScriptWorkspaceSourceFile = TEXT("GameplayScript.cs");
constexpr const TCHAR* AvidScriptWorkspaceProjectFile = TEXT("AvidScript.Gameplay.csproj");
constexpr const TCHAR* AvidScriptWorkspaceSolutionFile = TEXT("AvidScript.Gameplay.slnx");
constexpr const TCHAR* AvidScriptWorkspaceEditorConfigFile = TEXT(".editorconfig");
constexpr const TCHAR* AvidScriptWorkspaceProfileFile = TEXT("default.csharp-profile.json");
constexpr const TCHAR* AvidScriptWorkspaceGlobalJsonFile = TEXT("global.json");
constexpr const TCHAR* AvidScriptWorkspaceFacadeFile = TEXT("AvidScript.Bindings.generated.cs");
constexpr const TCHAR* AvidScriptWorkspaceModuleId = TEXT("csharp_project_gameplay");
constexpr const TCHAR* AvidScriptWorkspaceArtifactStem = TEXT("project_gameplay");

FString NormalizeAvidScriptWorkspacePath(FString Path)
{
    if (!Path.IsEmpty())
    {
        Path = FPaths::ConvertRelativePathToFull(Path);
        FPaths::NormalizeFilename(Path);
        FPaths::CollapseRelativeDirectories(Path);
        FPaths::RemoveDuplicateSlashes(Path);
        while (Path.Len() > 3 && Path.EndsWith(TEXT("/")))
        {
            Path.LeftChopInline(1);
        }
    }
    return Path;
}

FString GetAvidScriptWorkspacePluginRoot()
{
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
    if (Plugin.IsValid())
    {
        return NormalizeAvidScriptWorkspacePath(Plugin->GetBaseDir());
    }

    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        FPaths::ProjectDir(),
        TEXT("Plugins"),
        TEXT("AvidScript")));
}

bool IsAvidScriptWorkspacePathContained(
    const FString& Root,
    const FString& Candidate)
{
    const FString NormalizedRoot = NormalizeAvidScriptWorkspacePath(Root);
    const FString NormalizedCandidate = NormalizeAvidScriptWorkspacePath(Candidate);
    if (NormalizedRoot.IsEmpty() || NormalizedCandidate.IsEmpty())
    {
        return false;
    }
    if (NormalizedCandidate.Equals(NormalizedRoot, ESearchCase::IgnoreCase))
    {
        return true;
    }

    return NormalizedCandidate.StartsWith(
        NormalizedRoot + TEXT("/"),
        ESearchCase::IgnoreCase);
}

void SetAvidScriptWorkspaceFailure(
    const FString& ErrorCategory,
    const FString& ErrorMessage,
    const FString& NextAction,
    FAvidScriptEditorCSharpWorkspaceResult& OutResult)
{
    OutResult.bSucceeded = false;
    OutResult.ErrorCategory = ErrorCategory;
    OutResult.ErrorMessage = ErrorMessage;
    OutResult.NextAction = NextAction;
}

bool LoadAvidScriptWorkspaceTemplate(
    const FString& TemplateRoot,
    const FString& TemplateFile,
    FString& OutText,
    FAvidScriptEditorCSharpWorkspaceResult& OutResult)
{
    const FString TemplatePath = NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        TemplateRoot,
        TemplateFile));
    if (!FPaths::FileExists(TemplatePath)
        || !FFileHelper::LoadFileToString(OutText, *TemplatePath))
    {
        SetAvidScriptWorkspaceFailure(
            TEXT("workspace_template_missing"),
            FString::Printf(TEXT("C# workspace template could not be read: %s"), *TemplatePath),
            TEXT("restore the plugin ProjectWorkspace templates and retry"),
            OutResult);
        return false;
    }
    return true;
}

FString EscapeAvidScriptWorkspaceXmlAttribute(FString Value)
{
    Value.ReplaceInline(TEXT("&"), TEXT("&amp;"));
    Value.ReplaceInline(TEXT("\""), TEXT("&quot;"));
    Value.ReplaceInline(TEXT("<"), TEXT("&lt;"));
    Value.ReplaceInline(TEXT(">"), TEXT("&gt;"));
    return Value;
}

bool ValidateAvidScriptWorkspaceTemplateResolved(
    const FString& TemplateName,
    const FString& Text,
    FAvidScriptEditorCSharpWorkspaceResult& OutResult)
{
    if (!Text.Contains(TEXT("{{")) && !Text.Contains(TEXT("}}")))
    {
        return true;
    }

    SetAvidScriptWorkspaceFailure(
        TEXT("workspace_template_token_unresolved"),
        FString::Printf(TEXT("C# workspace template still contains a token: %s"), *TemplateName),
        TEXT("repair the template token set before creating the workspace"),
        OutResult);
    return false;
}

bool WriteAvidScriptWorkspaceTextAtomic(
    const FString& Path,
    const FString& Text,
    FAvidScriptEditorCSharpWorkspaceResult& OutResult)
{
    if (IFileManager::Get().DirectoryExists(*Path))
    {
        SetAvidScriptWorkspaceFailure(
            TEXT("workspace_file_is_directory"),
            FString::Printf(TEXT("C# workspace file path is a directory: %s"), *Path),
            TEXT("move or remove the conflicting directory and retry"),
            OutResult);
        return false;
    }

    const FString ParentDirectory = FPaths::GetPath(Path);
    if (!IFileManager::Get().MakeDirectory(*ParentDirectory, true))
    {
        SetAvidScriptWorkspaceFailure(
            TEXT("workspace_directory_failed"),
            FString::Printf(TEXT("C# workspace directory could not be created: %s"), *ParentDirectory),
            TEXT("verify the project directory is writable and retry"),
            OutResult);
        return false;
    }

    const FString TemporaryPath = Path + TEXT(".tmp.")
        + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    if (!FFileHelper::SaveStringToFile(
            Text,
            *TemporaryPath,
            FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        SetAvidScriptWorkspaceFailure(
            TEXT("workspace_write_failed"),
            FString::Printf(TEXT("C# workspace temporary file could not be written: %s"), *TemporaryPath),
            TEXT("verify free space and project write permissions, then retry"),
            OutResult);
        return false;
    }

    const bool bMoved = IFileManager::Get().Move(
        *Path,
        *TemporaryPath,
        true,
        true,
        false,
        true);
    if (!bMoved)
    {
        IFileManager::Get().Delete(*TemporaryPath, false, true, true);
        SetAvidScriptWorkspaceFailure(
            TEXT("workspace_publish_failed"),
            FString::Printf(TEXT("C# workspace file could not be published atomically: %s"), *Path),
            TEXT("close readers holding the destination file and retry"),
            OutResult);
        return false;
    }
    return true;
}

bool WriteAvidScriptWorkspaceUserFile(
    const FString& Path,
    const FString& Text,
    const bool bOverwrite,
    bool& bOutCreated,
    FAvidScriptEditorCSharpWorkspaceResult& OutResult)
{
    bOutCreated = false;
    const bool bExists = FPaths::FileExists(Path);
    if (bExists && !bOverwrite)
    {
        ++OutResult.PreservedUserFileCount;
        return true;
    }
    if (!WriteAvidScriptWorkspaceTextAtomic(Path, Text, OutResult))
    {
        return false;
    }

    if (bExists)
    {
        ++OutResult.UpdatedUserFileCount;
    }
    else
    {
        bOutCreated = true;
        ++OutResult.CreatedUserFileCount;
    }
    return true;
}

bool ValidateAvidScriptWorkspaceOwnedPath(
    const FString& ProjectRoot,
    const FString& Description,
    const FString& Candidate,
    FAvidScriptEditorCSharpWorkspaceResult& OutResult)
{
    if (IsAvidScriptWorkspacePathContained(ProjectRoot, Candidate))
    {
        return true;
    }

    SetAvidScriptWorkspaceFailure(
        TEXT("workspace_path_outside_project"),
        FString::Printf(TEXT("%s must remain inside the project: %s"), *Description, *Candidate),
        TEXT("choose workspace, generated, binding, and output roots inside the current UE project"),
        OutResult);
    return false;
}
} // namespace

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultWorkspaceRoot()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        FPaths::ProjectDir(),
        TEXT("Scripts"),
        TEXT("AvidScript")));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultTemplateRoot()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        GetAvidScriptWorkspacePluginRoot(),
        TEXT("Templates"),
        TEXT("CSharp"),
        TEXT("ProjectWorkspace")));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultGeneratedRoot()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        FPaths::ProjectIntermediateDir(),
        TEXT("AvidScript"),
        TEXT("CSharpWorkspace")));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultBindingPackageRoot()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        GetDefaultGeneratedRoot(),
        TEXT("BindingPackages")));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultOutputRoot()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        FPaths::ProjectSavedDir(),
        TEXT("AvidScriptCSharpGuest"),
        TEXT("ProjectGameplay")));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultSourcePath()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        GetDefaultWorkspaceRoot(),
        AvidScriptWorkspaceSourceFile));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultProjectPath()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        GetDefaultWorkspaceRoot(),
        AvidScriptWorkspaceProjectFile));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultProfilePath()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        GetDefaultWorkspaceRoot(),
        AvidScriptWorkspaceProfileFile));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultSolutionPath()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        GetDefaultWorkspaceRoot(),
        AvidScriptWorkspaceSolutionFile));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultEditorConfigPath()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        GetDefaultWorkspaceRoot(),
        AvidScriptWorkspaceEditorConfigFile));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultGlobalJsonPath()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        GetDefaultWorkspaceRoot(),
        AvidScriptWorkspaceGlobalJsonFile));
}

FString FAvidScriptEditorCSharpWorkspaceService::GetDefaultFacadePath()
{
    return NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        GetDefaultGeneratedRoot(),
        AvidScriptWorkspaceFacadeFile));
}

bool FAvidScriptEditorCSharpWorkspaceService::CreateOrRefresh(
    const FAvidScriptEditorCSharpWorkspaceConfig& Config,
    FAvidScriptEditorCSharpWorkspaceResult& OutResult)
{
    OutResult = FAvidScriptEditorCSharpWorkspaceResult();

    OutResult.WorkspaceRoot = NormalizeAvidScriptWorkspacePath(
        Config.WorkspaceRoot.IsEmpty() ? GetDefaultWorkspaceRoot() : Config.WorkspaceRoot);
    OutResult.GeneratedRoot = NormalizeAvidScriptWorkspacePath(
        Config.GeneratedRoot.IsEmpty() ? GetDefaultGeneratedRoot() : Config.GeneratedRoot);
    OutResult.BindingPackageRoot = NormalizeAvidScriptWorkspacePath(
        Config.BindingPackageRoot.IsEmpty() ? GetDefaultBindingPackageRoot() : Config.BindingPackageRoot);
    OutResult.OutputRoot = NormalizeAvidScriptWorkspacePath(
        Config.OutputRoot.IsEmpty() ? GetDefaultOutputRoot() : Config.OutputRoot);
    const FString TemplateRoot = NormalizeAvidScriptWorkspacePath(
        Config.TemplateRoot.IsEmpty() ? GetDefaultTemplateRoot() : Config.TemplateRoot);

    OutResult.SourcePath = NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        OutResult.WorkspaceRoot,
        AvidScriptWorkspaceSourceFile));
    OutResult.ProjectPath = NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        OutResult.WorkspaceRoot,
        AvidScriptWorkspaceProjectFile));
    OutResult.SolutionPath = NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        OutResult.WorkspaceRoot,
        AvidScriptWorkspaceSolutionFile));
    OutResult.EditorConfigPath = NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        OutResult.WorkspaceRoot,
        AvidScriptWorkspaceEditorConfigFile));
    OutResult.ProfilePath = NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        OutResult.WorkspaceRoot,
        AvidScriptWorkspaceProfileFile));
    OutResult.GlobalJsonPath = NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        OutResult.WorkspaceRoot,
        AvidScriptWorkspaceGlobalJsonFile));
    OutResult.FacadePath = NormalizeAvidScriptWorkspacePath(FPaths::Combine(
        OutResult.GeneratedRoot,
        AvidScriptWorkspaceFacadeFile));
    OutResult.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
        OutResult.OutputRoot,
        AvidScriptWorkspaceArtifactStem);
    OutResult.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
        OutResult.OutputRoot,
        AvidScriptWorkspaceArtifactStem);

    const FString ProjectRoot = NormalizeAvidScriptWorkspacePath(FPaths::ProjectDir());
    if (!ValidateAvidScriptWorkspaceOwnedPath(ProjectRoot, TEXT("C# workspace root"), OutResult.WorkspaceRoot, OutResult)
        || !ValidateAvidScriptWorkspaceOwnedPath(ProjectRoot, TEXT("C# generated root"), OutResult.GeneratedRoot, OutResult)
        || !ValidateAvidScriptWorkspaceOwnedPath(ProjectRoot, TEXT("C# binding package root"), OutResult.BindingPackageRoot, OutResult)
        || !ValidateAvidScriptWorkspaceOwnedPath(ProjectRoot, TEXT("C# output root"), OutResult.OutputRoot, OutResult))
    {
        return false;
    }
    if (IsAvidScriptWorkspacePathContained(OutResult.WorkspaceRoot, OutResult.GeneratedRoot))
    {
        SetAvidScriptWorkspaceFailure(
            TEXT("workspace_generated_root_user_owned"),
            FString::Printf(TEXT("Generated C# files must remain outside the user workspace: %s"), *OutResult.GeneratedRoot),
            TEXT("choose an Intermediate or Saved generated root outside Scripts/AvidScript"),
            OutResult);
        return false;
    }

    FString SourceTemplate;
    FString ProjectTemplate;
    FString SolutionTemplate;
    FString EditorConfigTemplate;
    FString ProfileTemplate;
    FString GlobalJsonTemplate;
    if (!LoadAvidScriptWorkspaceTemplate(TemplateRoot, AvidScriptWorkspaceSourceFile, SourceTemplate, OutResult)
        || !LoadAvidScriptWorkspaceTemplate(TemplateRoot, TEXT("AvidScript.Gameplay.csproj.template"), ProjectTemplate, OutResult)
        || !LoadAvidScriptWorkspaceTemplate(TemplateRoot, TEXT("AvidScript.Gameplay.slnx.template"), SolutionTemplate, OutResult)
        || !LoadAvidScriptWorkspaceTemplate(TemplateRoot, TEXT(".editorconfig.template"), EditorConfigTemplate, OutResult)
        || !LoadAvidScriptWorkspaceTemplate(TemplateRoot, TEXT("default.csharp-profile.json.template"), ProfileTemplate, OutResult)
        || !LoadAvidScriptWorkspaceTemplate(TemplateRoot, AvidScriptWorkspaceGlobalJsonFile, GlobalJsonTemplate, OutResult))
    {
        return false;
    }

    FString FacadeRelativePath = OutResult.FacadePath;
    FString WorkspaceBase = OutResult.WorkspaceRoot;
    if (!WorkspaceBase.EndsWith(TEXT("/")))
    {
        WorkspaceBase += TEXT("/");
    }
    if (!FPaths::MakePathRelativeTo(FacadeRelativePath, *WorkspaceBase))
    {
        SetAvidScriptWorkspaceFailure(
            TEXT("workspace_facade_relative_path_failed"),
            FString::Printf(TEXT("Generated C# facade path could not be made relative: %s"), *OutResult.FacadePath),
            TEXT("choose workspace and generated roots on the same project volume"),
            OutResult);
        return false;
    }
    FPaths::NormalizeFilename(FacadeRelativePath);
    ProjectTemplate.ReplaceInline(
        TEXT("{{GENERATED_FACADE_RELATIVE_PATH}}"),
        *EscapeAvidScriptWorkspaceXmlAttribute(FacadeRelativePath),
        ESearchCase::CaseSensitive);
    SolutionTemplate.ReplaceInline(
        TEXT("{{PROJECT_PATH}}"),
        *EscapeAvidScriptWorkspaceXmlAttribute(AvidScriptWorkspaceProjectFile),
        ESearchCase::CaseSensitive);

    ProfileTemplate.ReplaceInline(TEXT("{{SOURCE_PATH}}"), *OutResult.SourcePath, ESearchCase::CaseSensitive);
    ProfileTemplate.ReplaceInline(TEXT("{{PROJECT_PATH}}"), *OutResult.ProjectPath, ESearchCase::CaseSensitive);
    ProfileTemplate.ReplaceInline(TEXT("{{OUTPUT_ROOT}}"), *OutResult.OutputRoot, ESearchCase::CaseSensitive);
    ProfileTemplate.ReplaceInline(TEXT("{{REPORT_PATH}}"), *OutResult.ReportPath, ESearchCase::CaseSensitive);
    ProfileTemplate.ReplaceInline(TEXT("{{MANIFEST_PATH}}"), *OutResult.ManifestPath, ESearchCase::CaseSensitive);
    ProfileTemplate.ReplaceInline(
        TEXT("{{BUILD_SCRIPT_PATH}}"),
        *FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath(),
        ESearchCase::CaseSensitive);

    if (!ValidateAvidScriptWorkspaceTemplateResolved(AvidScriptWorkspaceProjectFile, ProjectTemplate, OutResult)
        || !ValidateAvidScriptWorkspaceTemplateResolved(AvidScriptWorkspaceSolutionFile, SolutionTemplate, OutResult)
        || !ValidateAvidScriptWorkspaceTemplateResolved(AvidScriptWorkspaceEditorConfigFile, EditorConfigTemplate, OutResult)
        || !ValidateAvidScriptWorkspaceTemplateResolved(AvidScriptWorkspaceProfileFile, ProfileTemplate, OutResult))
    {
        return false;
    }

    FAvidScriptCSharpBindingEmitResult BindingResult;
    if (!FAvidScriptEditorCSharpBindingEmitter::PublishEngineGameplay(
            OutResult.BindingPackageRoot,
            BindingResult))
    {
        SetAvidScriptWorkspaceFailure(
            BindingResult.ErrorCategory.IsEmpty()
                ? FString(TEXT("workspace_binding_publish_failed"))
                : BindingResult.ErrorCategory,
            BindingResult.ErrorMessage.IsEmpty()
                ? FString(TEXT("C# gameplay IDE binding package could not be published."))
                : BindingResult.ErrorMessage,
            BindingResult.NextAction.IsEmpty()
                ? FString(TEXT("repair the reflected gameplay binding profile and retry"))
                : BindingResult.NextAction,
            OutResult);
        return false;
    }
    OutResult.BindingPackageManifestPath = NormalizeAvidScriptWorkspacePath(BindingResult.ManifestPath);
    OutResult.BindingPackageHash = BindingResult.PackageHash;

    FString FacadeText;
    if (!FFileHelper::LoadFileToString(FacadeText, *BindingResult.ReferenceSourcePath))
    {
        SetAvidScriptWorkspaceFailure(
            TEXT("workspace_facade_read_failed"),
            FString::Printf(TEXT("Generated C# facade could not be read: %s"), *BindingResult.ReferenceSourcePath),
            TEXT("repair the generated binding package and retry workspace refresh"),
            OutResult);
        return false;
    }
    if (!WriteAvidScriptWorkspaceTextAtomic(OutResult.FacadePath, FacadeText, OutResult))
    {
        return false;
    }
    OutResult.bFacadeRefreshed = true;

    if (!WriteAvidScriptWorkspaceUserFile(
            OutResult.SourcePath,
            SourceTemplate,
            Config.bOverwriteUserFiles,
            OutResult.bSourceCreated,
            OutResult)
        || !WriteAvidScriptWorkspaceUserFile(
            OutResult.ProjectPath,
            ProjectTemplate,
            Config.bOverwriteUserFiles,
            OutResult.bProjectCreated,
            OutResult)
        || !WriteAvidScriptWorkspaceUserFile(
            OutResult.SolutionPath,
            SolutionTemplate,
            Config.bOverwriteUserFiles,
            OutResult.bSolutionCreated,
            OutResult)
        || !WriteAvidScriptWorkspaceUserFile(
            OutResult.EditorConfigPath,
            EditorConfigTemplate,
            Config.bOverwriteUserFiles,
            OutResult.bEditorConfigCreated,
            OutResult)
        || !WriteAvidScriptWorkspaceUserFile(
            OutResult.ProfilePath,
            ProfileTemplate,
            Config.bOverwriteUserFiles,
            OutResult.bProfileCreated,
            OutResult)
        || !WriteAvidScriptWorkspaceUserFile(
            OutResult.GlobalJsonPath,
            GlobalJsonTemplate,
            Config.bOverwriteUserFiles,
            OutResult.bGlobalJsonCreated,
            OutResult))
    {
        return false;
    }

    OutResult.bSucceeded = true;
    OutResult.NextAction = TEXT("open AvidScript.Gameplay.slnx, then run Build And Bind Project C# Gameplay Script");
    return true;
}
