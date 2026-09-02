#include "Commandlets/AvidScriptReleaseCommandlet.h"

#include "AvidScriptEditorCSharpBuildService.h"
#include "Dom/JsonObject.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptReleaseCommandlet, Log, All);

namespace
{
constexpr int32 AvidScriptReleaseArgumentFailure = 2;
constexpr int32 AvidScriptReleaseInputFailure = 3;
constexpr int32 AvidScriptReleaseBuildFailure = 4;
constexpr int32 AvidScriptReleaseOutputFailure = 5;

struct FAvidScriptReleaseArguments
{
	FString SourcePath;
	FString CSharpProjectPath;
	FString ModuleId;
	FString ArtifactStem;
	FString OutputRoot;
	FString DotNetPath;
	FString BindingPackagePath;
	FString RuntimeBindingPackagePath;
};

int32 FailAvidScriptRelease(
	const FString& Category,
	const FString& Message,
	int32 ExitCode)
{
	FString StableMessage = Message;
	StableMessage.ReplaceInline(TEXT("\r"), TEXT(" "));
	StableMessage.ReplaceInline(TEXT("\n"), TEXT(" "));
	UE_LOG(
		LogAvidScriptReleaseCommandlet,
		Error,
		TEXT("AVIDSCRIPT_RELEASE_RESULT category=%s message=\"%s\""),
		*Category,
		*StableMessage);
	return ExitCode;
}

bool IsAvidScriptReleaseGlobalSwitch(const FString& SwitchName)
{
	static const TSet<FString> AllowedSwitches = {
		TEXT("fullstdoutlogoutput"),
		TEXT("nop4"),
		TEXT("nosplash"),
		TEXT("nullrhi"),
		TEXT("stdout"),
		TEXT("unattended")
	};
	return AllowedSwitches.Contains(SwitchName.ToLower());
}

const FString* FindAvidScriptReleaseCanonicalParameter(
	const FString& ParameterName)
{
	static const TMap<FString, FString> AllowedParameters = {
		{ TEXT("artifactstem"), TEXT("ArtifactStem") },
		{ TEXT("bindingpackagepath"), TEXT("BindingPackagePath") },
		{ TEXT("csharpprojectpath"), TEXT("CSharpProjectPath") },
		{ TEXT("dotnetpath"), TEXT("DotNetPath") },
		{ TEXT("moduleid"), TEXT("ModuleId") },
		{ TEXT("outputroot"), TEXT("OutputRoot") },
		{ TEXT("runtimebindingpackagepath"), TEXT("RuntimeBindingPackagePath") },
		{ TEXT("sourcepath"), TEXT("SourcePath") }
	};
	return AllowedParameters.Find(ParameterName.ToLower());
}

bool ParseAvidScriptReleaseArguments(
	const FString& Params,
	FAvidScriptReleaseArguments& OutArguments,
	FString& OutCategory,
	FString& OutMessage)
{
	OutArguments = FAvidScriptReleaseArguments();
	TArray<FString> Tokens;
	TArray<FString> Switches;
	UCommandlet::ParseCommandLine(*Params, Tokens, Switches);
	if (!Tokens.IsEmpty())
	{
		OutCategory = TEXT("argument_unknown");
		OutMessage = FString::Printf(
			TEXT("Unexpected positional argument: %s"),
			*Tokens[0]);
		return false;
	}

	TMap<FString, FString> Values;
	for (const FString& Switch : Switches)
	{
		int32 AssignmentIndex = INDEX_NONE;
		if (!Switch.FindChar(TEXT('='), AssignmentIndex))
		{
			if (!IsAvidScriptReleaseGlobalSwitch(Switch))
			{
				OutCategory = TEXT("argument_unknown");
				OutMessage = FString::Printf(
					TEXT("Unknown switch: -%s"),
					*Switch);
				return false;
			}
			continue;
		}

		FString ParameterName = Switch.Left(AssignmentIndex);
		FString Value = Switch.Mid(AssignmentIndex + 1).TrimQuotes();
		ParameterName.TrimStartAndEndInline();
		Value.TrimStartAndEndInline();
		if (ParameterName.Equals(TEXT("run"), ESearchCase::IgnoreCase))
		{
			if (!Value.Equals(
					TEXT("AvidScriptRelease"),
					ESearchCase::IgnoreCase))
			{
				OutCategory = TEXT("argument_invalid");
				OutMessage = TEXT("The run parameter must select AvidScriptRelease.");
				return false;
			}
			continue;
		}
		if (ParameterName.Equals(TEXT("abslog"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		const FString* CanonicalName =
			FindAvidScriptReleaseCanonicalParameter(ParameterName);
		if (CanonicalName == nullptr)
		{
			OutCategory = TEXT("argument_unknown");
			OutMessage = FString::Printf(
				TEXT("Unknown parameter: -%s"),
				*ParameterName);
			return false;
		}
		if (Values.Contains(*CanonicalName))
		{
			OutCategory = TEXT("argument_duplicate");
			OutMessage = FString::Printf(
				TEXT("Duplicate parameter: -%s"),
				**CanonicalName);
			return false;
		}
		if (Value.IsEmpty())
		{
			OutCategory = TEXT("argument_invalid");
			OutMessage = FString::Printf(
				TEXT("Parameter must not be empty: -%s"),
				**CanonicalName);
			return false;
		}
		Values.Add(*CanonicalName, MoveTemp(Value));
	}

	static const TArray<FString> RequiredParameters = {
		TEXT("SourcePath"),
		TEXT("CSharpProjectPath"),
		TEXT("ModuleId"),
		TEXT("ArtifactStem"),
		TEXT("OutputRoot"),
		TEXT("DotNetPath")
	};
	for (const FString& RequiredParameter : RequiredParameters)
	{
		if (!Values.Contains(RequiredParameter))
		{
			OutCategory = TEXT("argument_missing");
			OutMessage = FString::Printf(
				TEXT("Required parameter is missing: -%s"),
				*RequiredParameter);
			return false;
		}
	}

	OutArguments.SourcePath = Values.FindChecked(TEXT("SourcePath"));
	OutArguments.CSharpProjectPath =
		Values.FindChecked(TEXT("CSharpProjectPath"));
	OutArguments.ModuleId = Values.FindChecked(TEXT("ModuleId"));
	OutArguments.ArtifactStem = Values.FindChecked(TEXT("ArtifactStem"));
	OutArguments.OutputRoot = Values.FindChecked(TEXT("OutputRoot"));
	OutArguments.DotNetPath = Values.FindChecked(TEXT("DotNetPath"));
	Values.RemoveAndCopyValue(
		TEXT("BindingPackagePath"),
		OutArguments.BindingPackagePath);
	Values.RemoveAndCopyValue(
		TEXT("RuntimeBindingPackagePath"),
		OutArguments.RuntimeBindingPackagePath);
	return true;
}

FString NormalizeAvidScriptReleasePath(const FString& Path)
{
	FString Normalized = FPaths::ConvertRelativePathToFull(Path);
	FPaths::CollapseRelativeDirectories(Normalized);
	FPaths::NormalizeFilename(Normalized);
	return Normalized;
}

bool IsAvidScriptReleasePathUnderRoot(
	const FString& Path,
	const FString& Root)
{
	const FString NormalizedRoot = Root.EndsWith(TEXT("/"))
		? Root
		: Root + TEXT("/");
	return Path.Equals(Root, ESearchCase::IgnoreCase)
		|| Path.StartsWith(NormalizedRoot, ESearchCase::IgnoreCase);
}

bool NormalizeAvidScriptReleaseProjectPath(
	const FString& Input,
	const FString& ProjectRoot,
	FString& OutPath)
{
	if (Input.IsEmpty() || FPaths::IsRelative(Input))
	{
		return false;
	}
	OutPath = NormalizeAvidScriptReleasePath(Input);
	return IsAvidScriptReleasePathUnderRoot(OutPath, ProjectRoot);
}

bool ValidateAvidScriptReleaseManifest(
	const FString& ManifestPath,
	const FString& ModuleId,
	const FString& ArtifactFile)
{
	FString ManifestJson;
	if (!FFileHelper::LoadFileToString(ManifestJson, *ManifestPath))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Manifest;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(ManifestJson);
	if (!FJsonSerializer::Deserialize(Reader, Manifest)
		|| !Manifest.IsValid())
	{
		return false;
	}
	FString ManifestModuleId;
	const TSharedPtr<FJsonObject>* Execution = nullptr;
	FString Format;
	FString Policy;
	FString File;
	return Manifest->TryGetStringField(TEXT("module_id"), ManifestModuleId)
		&& ManifestModuleId == ModuleId
		&& Manifest->TryGetObjectField(TEXT("execution"), Execution)
		&& Execution != nullptr
		&& Execution->IsValid()
		&& (*Execution)->TryGetStringField(TEXT("format"), Format)
		&& Format == TEXT("wasmtime_serialized_v1")
		&& (*Execution)->TryGetStringField(TEXT("policy"), Policy)
		&& Policy == TEXT("require_precompiled")
		&& (*Execution)->TryGetStringField(TEXT("file"), File)
		&& File == ArtifactFile;
}
} // namespace

UAvidScriptReleaseCommandlet::UAvidScriptReleaseCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UAvidScriptReleaseCommandlet::Main(const FString& Params)
{
	FAvidScriptReleaseArguments Arguments;
	FString ErrorCategory;
	FString ErrorMessage;
	if (!ParseAvidScriptReleaseArguments(
			Params,
			Arguments,
			ErrorCategory,
			ErrorMessage))
	{
		return FailAvidScriptRelease(
			ErrorCategory,
			ErrorMessage,
			AvidScriptReleaseArgumentFailure);
	}

	const FRegexPattern ModuleIdPattern(
		TEXT("^[a-z][a-z0-9_.-]{0,63}$"));
	FRegexMatcher ModuleIdMatcher(ModuleIdPattern, Arguments.ModuleId);
	const FRegexPattern ArtifactStemPattern(
		TEXT("^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$"));
	FRegexMatcher ArtifactStemMatcher(
		ArtifactStemPattern,
		Arguments.ArtifactStem);
	if (!ModuleIdMatcher.FindNext()
		|| !ArtifactStemMatcher.FindNext())
	{
		return FailAvidScriptRelease(
			TEXT("argument_invalid"),
			TEXT("ModuleId or ArtifactStem does not match the release command schema."),
			AvidScriptReleaseArgumentFailure);
	}

	const FString ProjectRoot =
		NormalizeAvidScriptReleasePath(FPaths::ProjectDir());
	FString SourcePath;
	FString ProjectPath;
	FString OutputRoot;
	FString BindingPackagePath;
	FString RuntimeBindingPackagePath;
	if (!NormalizeAvidScriptReleaseProjectPath(
			Arguments.SourcePath,
			ProjectRoot,
			SourcePath)
		|| !NormalizeAvidScriptReleaseProjectPath(
			Arguments.CSharpProjectPath,
			ProjectRoot,
			ProjectPath)
		|| !NormalizeAvidScriptReleaseProjectPath(
			Arguments.OutputRoot,
			ProjectRoot,
			OutputRoot)
		|| (!Arguments.BindingPackagePath.IsEmpty()
			&& !NormalizeAvidScriptReleaseProjectPath(
				Arguments.BindingPackagePath,
				ProjectRoot,
				BindingPackagePath))
		|| (!Arguments.RuntimeBindingPackagePath.IsEmpty()
			&& !NormalizeAvidScriptReleaseProjectPath(
				Arguments.RuntimeBindingPackagePath,
				ProjectRoot,
				RuntimeBindingPackagePath)))
	{
		return FailAvidScriptRelease(
			TEXT("path_outside_project"),
			TEXT("Release input paths must be absolute and remain inside ProjectRoot."),
			AvidScriptReleaseInputFailure);
	}
	if (FPaths::IsRelative(Arguments.DotNetPath))
	{
		return FailAvidScriptRelease(
			TEXT("dotnet_path_invalid"),
			TEXT("DotNetPath must be absolute."),
			AvidScriptReleaseInputFailure);
	}
	const FString DotNetPath =
		NormalizeAvidScriptReleasePath(Arguments.DotNetPath);

	struct FRequiredFile
	{
		const TCHAR* Category;
		const TCHAR* Label;
		const FString* Path;
	};
	TArray<FRequiredFile> RequiredFiles = {
		{ TEXT("source_missing"), TEXT("C# source"), &SourcePath },
		{ TEXT("csharp_project_missing"), TEXT("C# project"), &ProjectPath },
		{ TEXT("dotnet_missing"), TEXT("dotnet host"), &DotNetPath }
	};
	if (!BindingPackagePath.IsEmpty())
	{
		RequiredFiles.Add({
			TEXT("binding_package_missing"),
			TEXT("binding package"),
			&BindingPackagePath
		});
	}
	if (!RuntimeBindingPackagePath.IsEmpty())
	{
		RequiredFiles.Add({
			TEXT("runtime_binding_package_missing"),
			TEXT("runtime binding package"),
			&RuntimeBindingPackagePath
		});
	}
	for (const FRequiredFile& RequiredFile : RequiredFiles)
	{
		if (!FPaths::FileExists(*RequiredFile.Path))
		{
			return FailAvidScriptRelease(
				RequiredFile.Category,
				FString::Printf(
					TEXT("Required %s file does not exist: %s"),
					RequiredFile.Label,
					**RequiredFile.Path),
				AvidScriptReleaseInputFailure);
		}
	}
	if (FPaths::FileExists(OutputRoot))
	{
		return FailAvidScriptRelease(
			TEXT("output_root_invalid"),
			TEXT("OutputRoot resolves to a file."),
			AvidScriptReleaseInputFailure);
	}

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.SourcePath = SourcePath;
	Config.ProjectPath = ProjectPath;
	Config.OutputRoot = OutputRoot;
	Config.ReportPath =
		FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(
			OutputRoot,
			Arguments.ArtifactStem);
	Config.ManifestPath =
		FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(
			OutputRoot,
			Arguments.ArtifactStem);
	Config.BindingPackagePath = BindingPackagePath;
	Config.RuntimeBindingPackagePath = RuntimeBindingPackagePath;
	Config.DotNetPath = DotNetPath;
	Config.ModuleId = Arguments.ModuleId;
	Config.ArtifactStem = Arguments.ArtifactStem;
	Config.Configuration = TEXT("Release");
	Config.VmArtifactPolicy =
		EAvidScriptEditorVmArtifactPolicy::RequirePrecompiled;

	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!FAvidScriptEditorCSharpBuildService::BuildProfile(
			Config,
			BuildResult)
		|| !BuildResult.bSucceeded)
	{
		return FailAvidScriptRelease(
			BuildResult.ErrorCategory.IsEmpty()
				? FString(TEXT("build_failed"))
				: BuildResult.ErrorCategory,
			BuildResult.ErrorMessage.IsEmpty()
				? FString(TEXT("C# release build failed."))
				: BuildResult.ErrorMessage,
			AvidScriptReleaseBuildFailure);
	}

	const FString ExpectedManifestPath =
		NormalizeAvidScriptReleasePath(Config.ManifestPath);
	const FString ExpectedArtifactPath = NormalizeAvidScriptReleasePath(
		FPaths::Combine(
			OutputRoot,
			Arguments.ArtifactStem + TEXT(".wasmtime.cwasm")));
	const FString ArtifactFile = FPaths::GetCleanFilename(ExpectedArtifactPath);
	if (!BuildResult.ManifestPath.Equals(
			ExpectedManifestPath,
			ESearchCase::IgnoreCase)
		|| !FPaths::FileExists(ExpectedManifestPath))
	{
		return FailAvidScriptRelease(
			TEXT("manifest_missing"),
			TEXT("C# release build did not publish the expected Runtime manifest."),
			AvidScriptReleaseOutputFailure);
	}
	if (!BuildResult.bVmArtifactPublished
		|| BuildResult.VmArtifactFormat != TEXT("wasmtime_serialized_v1")
		|| BuildResult.VmArtifactPolicy != TEXT("require_precompiled")
		|| !BuildResult.VmArtifactPath.Equals(
			ExpectedArtifactPath,
			ESearchCase::IgnoreCase)
		|| !FPaths::FileExists(ExpectedArtifactPath))
	{
		return FailAvidScriptRelease(
			TEXT("precompiled_missing"),
			TEXT("C# release build did not publish the required cwasm artifact."),
			AvidScriptReleaseOutputFailure);
	}
	if (!ValidateAvidScriptReleaseManifest(
			ExpectedManifestPath,
			Arguments.ModuleId,
			ArtifactFile))
	{
		return FailAvidScriptRelease(
			TEXT("manifest_contract_invalid"),
			TEXT("Runtime manifest does not require the published precompiled artifact."),
			AvidScriptReleaseOutputFailure);
	}

	UE_LOG(
		LogAvidScriptReleaseCommandlet,
		Display,
		TEXT("AVIDSCRIPT_RELEASE_RESULT category=success module_id=%s manifest=\"%s\" cwasm=\"%s\""),
		*Arguments.ModuleId,
		*ExpectedManifestPath,
		*ExpectedArtifactPath);
	return 0;
}
