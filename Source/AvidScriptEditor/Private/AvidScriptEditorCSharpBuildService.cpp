#include "AvidScriptEditorCSharpBuildService.h"

#include "CSharpBuild/AvidScriptEditorCSharpBuildInvoker.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildPipeline.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
constexpr const TCHAR* AvidScriptDefaultCSharpActorLifecycleModuleId =
	TEXT("csharp_actor_lifecycle");
constexpr const TCHAR* AvidScriptDefaultCSharpActorLifecycleArtifactStem =
	TEXT("actor_lifecycle");

void NormalizeAvidScriptCSharpBuildPath(FString& Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}
}

FString NormalizeAvidScriptCSharpBuildPathCopy(FString Path)
{
	NormalizeAvidScriptCSharpBuildPath(Path);
	return Path;
}

FString GetAvidScriptCSharpBuildPluginBaseDir()
{
	const TSharedPtr<IPlugin> Plugin =
		IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (Plugin.IsValid())
	{
		return NormalizeAvidScriptCSharpBuildPathCopy(Plugin->GetBaseDir());
	}

	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins"),
		TEXT("AvidScript")));
}

FString GetAvidScriptCSharpArtifactStemOrDefault(
	const FString& ArtifactStem)
{
	return ArtifactStem.IsEmpty()
		? FString(AvidScriptDefaultCSharpActorLifecycleArtifactStem)
		: ArtifactStem;
}

FString MakeAvidScriptCSharpReportPathForOutputRoot(
	const FString& OutputRoot,
	const FString& ArtifactStem)
{
	FString ReportPath = FPaths::Combine(
		OutputRoot,
		GetAvidScriptCSharpArtifactStemOrDefault(ArtifactStem)
			+ TEXT(".csharp.report.json"));
	NormalizeAvidScriptCSharpBuildPath(ReportPath);
	return ReportPath;
}

FString MakeAvidScriptCSharpManifestPathForOutputRoot(
	const FString& OutputRoot,
	const FString& ArtifactStem)
{
	FString ManifestPath = FPaths::Combine(
		OutputRoot,
		GetAvidScriptCSharpArtifactStemOrDefault(ArtifactStem)
			+ TEXT(".avidscript.json"));
	NormalizeAvidScriptCSharpBuildPath(ManifestPath);
	return ManifestPath;
}
} // namespace

FString FAvidScriptEditorCSharpBuildService::
	GetDefaultActorLifecycleBuildScriptPath()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		GetAvidScriptCSharpBuildPluginBaseDir(),
		TEXT("Build"),
		TEXT("BuildCSharpActorLifecycle.ps1")));
}

FString FAvidScriptEditorCSharpBuildService::
	GetDefaultActorLifecycleSourcePath()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		GetAvidScriptCSharpBuildPluginBaseDir(),
		TEXT("Samples"),
		TEXT("CSharp"),
		TEXT("ActorLifecycle"),
		TEXT("ActorLifecycleScript.cs")));
}

FString FAvidScriptEditorCSharpBuildService::
	GetDefaultActorLifecycleProjectPath()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		GetAvidScriptCSharpBuildPluginBaseDir(),
		TEXT("Samples"),
		TEXT("CSharp"),
		TEXT("ActorLifecycle"),
		TEXT("AvidScript.ActorLifecycle.csproj")));
}

FString FAvidScriptEditorCSharpBuildService::
	GetDefaultActorLifecycleOutputRoot()
{
	return NormalizeAvidScriptCSharpBuildPathCopy(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ActorLifecycle")));
}

FString FAvidScriptEditorCSharpBuildService::
	GetDefaultActorLifecycleReportPath()
{
	return MakeReportPathForOutputRoot(
		GetDefaultActorLifecycleOutputRoot(),
		GetDefaultActorLifecycleArtifactStem());
}

FString FAvidScriptEditorCSharpBuildService::
	GetDefaultActorLifecycleManifestPath()
{
	return MakeManifestPathForOutputRoot(
		GetDefaultActorLifecycleOutputRoot(),
		GetDefaultActorLifecycleArtifactStem());
}

FString FAvidScriptEditorCSharpBuildService::
	GetDefaultActorLifecycleModuleId()
{
	return AvidScriptDefaultCSharpActorLifecycleModuleId;
}

FString FAvidScriptEditorCSharpBuildService::
	GetDefaultActorLifecycleArtifactStem()
{
	return AvidScriptDefaultCSharpActorLifecycleArtifactStem;
}

FString FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
	const FString& OutputRoot,
	const FString& ArtifactStem)
{
	return MakeAvidScriptCSharpReportPathForOutputRoot(
		OutputRoot,
		ArtifactStem);
}

FString FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
	const FString& OutputRoot,
	const FString& ArtifactStem)
{
	return MakeAvidScriptCSharpManifestPathForOutputRoot(
		OutputRoot,
		ArtifactStem);
}

bool FAvidScriptEditorCSharpBuildService::BuildProfile(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	FAvidScriptEditorCSharpBuildPlan Plan;
	if (!FAvidScriptEditorCSharpBuildPipeline::Prepare(
			Config,
			Plan,
			OutResult))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		FAvidScriptEditorCSharpBuildPipeline::Cleanup(Plan);
	};

	if (Plan.bAutomaticBindingSlice)
	{
		FAvidScriptEditorCSharpBuildResult BootstrapResult;
		FAvidScriptEditorCSharpBuildInvoker::BuildOnce(
			Plan.BootstrapConfig,
			BootstrapResult);
		if (!FAvidScriptEditorCSharpBuildPipeline::CompleteBootstrap(
				Plan,
				BootstrapResult,
				OutResult))
		{
			return false;
		}
	}

	FAvidScriptEditorCSharpBuildResult FinalResult;
	FAvidScriptEditorCSharpBuildInvoker::BuildOnce(
		Plan.FinalConfig,
		FinalResult);
	return FAvidScriptEditorCSharpBuildPipeline::CompleteFinal(
		Plan,
		FinalResult,
		OutResult);
}

bool FAvidScriptEditorCSharpBuildService::BuildActorLifecycle(
	const FAvidScriptEditorCSharpBuildConfig& Config,
	FAvidScriptEditorCSharpBuildResult& OutResult)
{
	return BuildProfile(Config, OutResult);
}
