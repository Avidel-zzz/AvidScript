#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorIdeLaunchService.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
class FAvidScriptEditorFakeIdeLaunchHost final
	: public IAvidScriptEditorIdeLaunchHost
{
public:
	virtual bool ResolveExecutable(
		const EAvidScriptEditorIdeKind Ide,
		FString& OutExecutablePath,
		FString& OutErrorMessage) override
	{
		++ResolveCount;
		OutExecutablePath = ResolvedExecutable;
		OutErrorMessage = ResolveError;
		return bResolveSucceeds;
	}

	virtual bool LaunchFile(
		const FString& Path,
		FString& OutErrorMessage) override
	{
		++FileLaunchCount;
		LastTarget = Path;
		OutErrorMessage = LaunchError;
		return bLaunchSucceeds;
	}

	virtual bool LaunchProcess(
		const FString& ExecutablePath,
		const FString& Arguments,
		const FString& WorkingDirectory,
		FString& OutErrorMessage) override
	{
		++ProcessLaunchCount;
		LastExecutable = ExecutablePath;
		LastArguments = Arguments;
		LastWorkingDirectory = WorkingDirectory;
		OutErrorMessage = LaunchError;
		return bLaunchSucceeds;
	}

	bool bResolveSucceeds = true;
	bool bLaunchSucceeds = true;
	int32 ResolveCount = 0;
	int32 FileLaunchCount = 0;
	int32 ProcessLaunchCount = 0;
	FString ResolvedExecutable;
	FString ResolveError;
	FString LaunchError;
	FString LastTarget;
	FString LastExecutable;
	FString LastArguments;
	FString LastWorkingDirectory;
};

FString MakeAvidScriptIdeTestPath(
	const FString& Root,
	const FString& RelativePath)
{
	FString Path = FPaths::ConvertRelativePathToFull(FPaths::Combine(Root, RelativePath));
	FPaths::NormalizeFilename(Path);
	return Path;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorIdeLaunchServiceTest,
	"AvidScript.Editor.CSharpWorkspace.IdeLaunchService",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorIdeLaunchServiceTest::RunTest(const FString& Parameters)
{
	const FString ProjectRoot = MakeAvidScriptIdeTestPath(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P61/IdeLaunch"));
	const FString WorkspaceRoot = MakeAvidScriptIdeTestPath(ProjectRoot, TEXT("Scripts/AvidScript"));
	const FString SolutionPath = MakeAvidScriptIdeTestPath(WorkspaceRoot, TEXT("AvidScript.Gameplay.slnx"));
	const FString ProjectPath = MakeAvidScriptIdeTestPath(WorkspaceRoot, TEXT("AvidScript.Gameplay.csproj"));
	const FString ExecutablePath = MakeAvidScriptIdeTestPath(ProjectRoot, TEXT("Tools/FakeIde.exe"));
	IFileManager::Get().DeleteDirectory(*ProjectRoot, false, true);
	IFileManager::Get().MakeDirectory(*WorkspaceRoot, true);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ExecutablePath), true);
	TestTrue(TEXT("Solution fixture writes"), FFileHelper::SaveStringToFile(TEXT("<Solution />"), *SolutionPath));
	TestTrue(TEXT("Project fixture writes"), FFileHelper::SaveStringToFile(TEXT("<Project />"), *ProjectPath));
	TestTrue(TEXT("Executable fixture writes"), FFileHelper::SaveStringToFile(TEXT("fixture"), *ExecutablePath));

	FAvidScriptEditorIdeLaunchConfig Config;
	Config.ProjectRoot = ProjectRoot;
	Config.WorkspaceRoot = WorkspaceRoot;
	Config.SolutionPath = SolutionPath;
	Config.ProjectPath = ProjectPath;
	Config.ExecutableOverride = ExecutablePath;
	FAvidScriptEditorFakeIdeLaunchHost Host;
	Host.ResolvedExecutable = ExecutablePath;

	FAvidScriptEditorIdeLaunchResult Result;
	Config.Ide = EAvidScriptEditorIdeKind::SystemDefault;
	TestTrue(TEXT("System default launches"), FAvidScriptEditorIdeLaunchService::Launch(Config, Result, &Host));
	TestEqual(TEXT("System default uses file association"), Host.FileLaunchCount, 1);
	TestEqual(TEXT("System default target is solution"), Host.LastTarget, SolutionPath);

	Config.Ide = EAvidScriptEditorIdeKind::VisualStudio;
	TestTrue(TEXT("Visual Studio launches"), FAvidScriptEditorIdeLaunchService::Launch(Config, Result, &Host));
	TestTrue(TEXT("Visual Studio receives solution"), Host.LastArguments.Contains(TEXT("AvidScript.Gameplay.slnx")));

	Config.Ide = EAvidScriptEditorIdeKind::Rider;
	TestTrue(TEXT("Rider launches"), FAvidScriptEditorIdeLaunchService::Launch(Config, Result, &Host));
	TestTrue(TEXT("Rider receives solution"), Host.LastArguments.Contains(TEXT("AvidScript.Gameplay.slnx")));

	Config.Ide = EAvidScriptEditorIdeKind::VisualStudioCode;
	TestTrue(TEXT("Visual Studio Code launches"), FAvidScriptEditorIdeLaunchService::Launch(Config, Result, &Host));
	TestTrue(TEXT("Visual Studio Code receives workspace root"), Host.LastArguments.Contains(TEXT("Scripts/AvidScript")));
	TestEqual(TEXT("IDE launches use workspace working directory"), Host.LastWorkingDirectory, WorkspaceRoot);

	FAvidScriptEditorIdeLaunchConfig OutsideConfig = Config;
	OutsideConfig.WorkspaceRoot = MakeAvidScriptIdeTestPath(ProjectRoot, TEXT("../Outside"));
	TestFalse(TEXT("Workspace outside project is rejected"), FAvidScriptEditorIdeLaunchService::Launch(OutsideConfig, Result, &Host));
	TestEqual(TEXT("Outside workspace category"), Result.ErrorCategory, FString(TEXT("ide_workspace_invalid")));

	FAvidScriptEditorIdeLaunchConfig MissingTargetConfig = Config;
	MissingTargetConfig.Ide = EAvidScriptEditorIdeKind::VisualStudio;
	MissingTargetConfig.SolutionPath = MakeAvidScriptIdeTestPath(WorkspaceRoot, TEXT("Missing.slnx"));
	TestFalse(TEXT("Missing solution is rejected"), FAvidScriptEditorIdeLaunchService::Launch(MissingTargetConfig, Result, &Host));
	TestEqual(TEXT("Missing target category"), Result.ErrorCategory, FString(TEXT("ide_target_missing")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
