#include "AvidScriptEditorIdeLaunchService.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"

namespace
{
FString NormalizeAvidScriptIdePath(FString Path)
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

bool IsAvidScriptIdePathContained(
	const FString& Root,
	const FString& Candidate)
{
	const FString NormalizedRoot = NormalizeAvidScriptIdePath(Root);
	const FString NormalizedCandidate = NormalizeAvidScriptIdePath(Candidate);
	return !NormalizedRoot.IsEmpty()
		&& !NormalizedCandidate.IsEmpty()
		&& (NormalizedCandidate.Equals(NormalizedRoot, ESearchCase::IgnoreCase)
			|| NormalizedCandidate.StartsWith(
				NormalizedRoot + TEXT("/"),
				ESearchCase::IgnoreCase));
}

void SetAvidScriptIdeLaunchFailure(
	const FString& Category,
	const FString& Message,
	const FString& NextAction,
	FAvidScriptEditorIdeLaunchResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorMessage = Message;
	OutResult.NextAction = NextAction;
}

void AddAvidScriptIdeCandidate(
	TArray<FString>& Candidates,
	const FString& Root,
	const FString& RelativePath)
{
	if (!Root.IsEmpty())
	{
		Candidates.Add(NormalizeAvidScriptIdePath(FPaths::Combine(Root, RelativePath)));
	}
}

bool FindFirstAvidScriptIdeCandidate(
	const TArray<FString>& Candidates,
	FString& OutPath)
{
	for (const FString& Candidate : Candidates)
	{
		if (FPaths::FileExists(Candidate))
		{
			OutPath = Candidate;
			return true;
		}
	}
	return false;
}

class FAvidScriptEditorPlatformIdeLaunchHost final
	: public IAvidScriptEditorIdeLaunchHost
{
public:
	virtual bool ResolveExecutable(
		const EAvidScriptEditorIdeKind Ide,
		FString& OutExecutablePath,
		FString& OutErrorMessage) override
	{
		OutExecutablePath.Reset();
		OutErrorMessage.Reset();
		TArray<FString> Candidates;
		const FString ProgramFiles = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles"));
		const FString ProgramFilesX86 = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramFiles(x86)"));
		const FString LocalAppData = FPlatformMisc::GetEnvironmentVariable(TEXT("LOCALAPPDATA"));

		switch (Ide)
		{
		case EAvidScriptEditorIdeKind::VisualStudio:
			AddAvidScriptIdeCandidate(
				Candidates,
				FPlatformMisc::GetEnvironmentVariable(TEXT("VSINSTALLDIR")),
				TEXT("Common7/IDE/devenv.exe"));
			for (const FString& Version : { FString(TEXT("2026")), FString(TEXT("2022")) })
			{
				for (const FString& Edition : {
					FString(TEXT("Enterprise")),
					FString(TEXT("Professional")),
					FString(TEXT("Community")) })
				{
					AddAvidScriptIdeCandidate(
						Candidates,
						ProgramFiles,
						FPaths::Combine(
							TEXT("Microsoft Visual Studio"),
							Version,
							Edition,
							TEXT("Common7/IDE/devenv.exe")));
				}
			}
			break;
		case EAvidScriptEditorIdeKind::Rider:
		{
			TArray<FString> RiderMatches;
			for (const FString& Root : {
				FPaths::Combine(LocalAppData, TEXT("JetBrains/Toolbox/apps/Rider")),
				FPaths::Combine(ProgramFiles, TEXT("JetBrains")) })
			{
				if (IFileManager::Get().DirectoryExists(*Root))
				{
					IFileManager::Get().FindFilesRecursive(
						RiderMatches,
						*Root,
						TEXT("rider64.exe"),
						true,
						false,
						false);
				}
			}
			RiderMatches.Sort();
			for (int32 Index = RiderMatches.Num() - 1; Index >= 0; --Index)
			{
				Candidates.Add(NormalizeAvidScriptIdePath(RiderMatches[Index]));
			}
			break;
		}
		case EAvidScriptEditorIdeKind::VisualStudioCode:
			AddAvidScriptIdeCandidate(Candidates, LocalAppData, TEXT("Programs/Microsoft VS Code/Code.exe"));
			AddAvidScriptIdeCandidate(Candidates, ProgramFiles, TEXT("Microsoft VS Code/Code.exe"));
			AddAvidScriptIdeCandidate(Candidates, ProgramFilesX86, TEXT("Microsoft VS Code/Code.exe"));
			break;
		default:
			OutErrorMessage = TEXT("System default launch does not require an executable.");
			return false;
		}

		if (FindFirstAvidScriptIdeCandidate(Candidates, OutExecutablePath))
		{
			return true;
		}
		OutErrorMessage = FString::Printf(
			TEXT("%s executable could not be discovered."),
			*FAvidScriptEditorIdeLaunchService::GetIdeName(Ide));
		return false;
	}

	virtual bool LaunchFile(
		const FString& Path,
		FString& OutErrorMessage) override
	{
		if (FPlatformProcess::LaunchFileInDefaultExternalApplication(
				*Path,
				nullptr,
				ELaunchVerb::Open,
				false))
		{
			return true;
		}
		OutErrorMessage = FString::Printf(
			TEXT("No application accepted the workspace file: %s"),
			*Path);
		return false;
	}

	virtual bool LaunchProcess(
		const FString& ExecutablePath,
		const FString& Arguments,
		const FString& WorkingDirectory,
		FString& OutErrorMessage) override
	{
		FProcHandle Process = FPlatformProcess::CreateProc(
			*ExecutablePath,
			*Arguments,
			true,
			false,
			false,
			nullptr,
			0,
			*WorkingDirectory,
			nullptr,
			nullptr);
		if (Process.IsValid())
		{
			FPlatformProcess::CloseProc(Process);
			return true;
		}
		OutErrorMessage = FString::Printf(
			TEXT("IDE process could not be started: %s"),
			*ExecutablePath);
		return false;
	}
};
} // namespace

FString FAvidScriptEditorIdeLaunchService::GetIdeName(
	const EAvidScriptEditorIdeKind Ide)
{
	switch (Ide)
	{
	case EAvidScriptEditorIdeKind::SystemDefault:
		return TEXT("system default IDE");
	case EAvidScriptEditorIdeKind::VisualStudio:
		return TEXT("Visual Studio");
	case EAvidScriptEditorIdeKind::Rider:
		return TEXT("Rider");
	case EAvidScriptEditorIdeKind::VisualStudioCode:
		return TEXT("Visual Studio Code");
	default:
		return TEXT("unknown IDE");
	}
}

bool FAvidScriptEditorIdeLaunchService::Launch(
	const FAvidScriptEditorIdeLaunchConfig& Config,
	FAvidScriptEditorIdeLaunchResult& OutResult,
	IAvidScriptEditorIdeLaunchHost* HostOverride)
{
	OutResult = FAvidScriptEditorIdeLaunchResult();
	OutResult.Ide = Config.Ide;
	const FString ProjectRoot = NormalizeAvidScriptIdePath(Config.ProjectRoot);
	OutResult.WorkingDirectory = NormalizeAvidScriptIdePath(Config.WorkspaceRoot);
	const FString SolutionPath = NormalizeAvidScriptIdePath(Config.SolutionPath);
	const FString ProjectPath = NormalizeAvidScriptIdePath(Config.ProjectPath);
	if (!IFileManager::Get().DirectoryExists(*OutResult.WorkingDirectory)
		|| !IsAvidScriptIdePathContained(ProjectRoot, OutResult.WorkingDirectory)
		|| !IsAvidScriptIdePathContained(OutResult.WorkingDirectory, SolutionPath)
		|| !IsAvidScriptIdePathContained(OutResult.WorkingDirectory, ProjectPath))
	{
		SetAvidScriptIdeLaunchFailure(
			TEXT("ide_workspace_invalid"),
			TEXT("The C# workspace paths are missing or outside the current UE project."),
			TEXT("regenerate the project C# workspace and retry"),
			OutResult);
		return false;
	}

	if (Config.Ide == EAvidScriptEditorIdeKind::VisualStudioCode)
	{
		OutResult.TargetPath = OutResult.WorkingDirectory;
		OutResult.Arguments = FString::Printf(
			TEXT("--reuse-window \"%s\""),
			*OutResult.TargetPath);
	}
	else if (Config.Ide == EAvidScriptEditorIdeKind::Rider
		&& !FPaths::FileExists(SolutionPath)
		&& FPaths::FileExists(ProjectPath))
	{
		OutResult.TargetPath = ProjectPath;
		OutResult.Arguments = FString::Printf(TEXT("\"%s\""), *ProjectPath);
	}
	else
	{
		OutResult.TargetPath = SolutionPath;
		OutResult.Arguments = FString::Printf(TEXT("\"%s\""), *SolutionPath);
	}
	if (Config.Ide != EAvidScriptEditorIdeKind::VisualStudioCode
		&& !FPaths::FileExists(OutResult.TargetPath))
	{
		SetAvidScriptIdeLaunchFailure(
			TEXT("ide_target_missing"),
			FString::Printf(TEXT("C# workspace target does not exist: %s"), *OutResult.TargetPath),
			TEXT("regenerate the project C# workspace and retry"),
			OutResult);
		return false;
	}

	FAvidScriptEditorPlatformIdeLaunchHost PlatformHost;
	IAvidScriptEditorIdeLaunchHost& Host = HostOverride != nullptr
		? *HostOverride
		: static_cast<IAvidScriptEditorIdeLaunchHost&>(PlatformHost);
	FString HostError;
	if (Config.Ide == EAvidScriptEditorIdeKind::SystemDefault)
	{
		if (!Host.LaunchFile(OutResult.TargetPath, HostError))
		{
			SetAvidScriptIdeLaunchFailure(
				TEXT("ide_file_launch_failed"),
				HostError,
				TEXT("associate .slnx with Visual Studio or Rider, or use an explicit IDE command"),
				OutResult);
			return false;
		}
	}
	else
	{
		OutResult.ExecutablePath = NormalizeAvidScriptIdePath(Config.ExecutableOverride);
		if (OutResult.ExecutablePath.IsEmpty()
			&& !Host.ResolveExecutable(Config.Ide, OutResult.ExecutablePath, HostError))
		{
			SetAvidScriptIdeLaunchFailure(
				TEXT("ide_executable_missing"),
				HostError,
				TEXT("install the selected IDE or configure an executable override"),
				OutResult);
			return false;
		}
		OutResult.ExecutablePath = NormalizeAvidScriptIdePath(OutResult.ExecutablePath);
		if (!FPaths::FileExists(OutResult.ExecutablePath))
		{
			SetAvidScriptIdeLaunchFailure(
				TEXT("ide_executable_missing"),
				FString::Printf(TEXT("IDE executable does not exist: %s"), *OutResult.ExecutablePath),
				TEXT("install the selected IDE or configure an executable override"),
				OutResult);
			return false;
		}
		if (!Host.LaunchProcess(
				OutResult.ExecutablePath,
				OutResult.Arguments,
				OutResult.WorkingDirectory,
				HostError))
		{
			SetAvidScriptIdeLaunchFailure(
				TEXT("ide_process_launch_failed"),
				HostError,
				TEXT("verify the selected IDE installation and retry"),
				OutResult);
			return false;
		}
	}

	OutResult.bSucceeded = true;
	OutResult.NextAction = TEXT("edit GameplayScript.cs, then build and bind the project C# gameplay script");
	return true;
}
