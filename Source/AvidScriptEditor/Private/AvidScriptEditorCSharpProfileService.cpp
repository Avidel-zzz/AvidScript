#include "AvidScriptEditorCSharpProfileService.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
FString NormalizeAvidScriptCSharpProfilePath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}

	return Path;
}

FString NormalizeAvidScriptCSharpProfileFieldPath(const FString& Path)
{
	if (Path.IsEmpty())
	{
		return FString();
	}

	FString Candidate = Path;
	FPaths::NormalizeFilename(Candidate);
	if (FPaths::IsRelative(Candidate))
	{
		Candidate = FPaths::Combine(FPaths::ProjectDir(), Candidate);
	}

	return NormalizeAvidScriptCSharpProfilePath(Candidate);
}

void SetAvidScriptCSharpProfileFailure(
	const FString& ErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorCSharpProfileLoadResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}

bool TryGetAvidScriptCSharpProfileStringField(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	FString& OutValue)
{
	OutValue.Reset();
	if (!Object.IsValid())
	{
		return false;
	}

	return Object->TryGetStringField(FieldName, OutValue) && !OutValue.IsEmpty();
}

FString MakeAvidScriptCSharpProfileSafeToken(const FString& RawValue, const FString& Fallback)
{
	FString Token;
	for (const TCHAR Character : RawValue)
	{
		if (FChar::IsAlnum(Character))
		{
			Token.AppendChar(FChar::ToLower(Character));
			continue;
		}

		if (!Token.EndsWith(TEXT("_")))
		{
			Token.AppendChar(TEXT('_'));
		}
	}

	Token.TrimStartAndEndInline();
	while (Token.StartsWith(TEXT("_")))
	{
		Token.RightChopInline(1);
	}
	while (Token.EndsWith(TEXT("_")))
	{
		Token.LeftChopInline(1);
	}

	return Token.IsEmpty() ? Fallback : Token;
}

bool LoadAvidScriptCSharpProfileJson(
	const FString& ProfilePath,
	TSharedPtr<FJsonObject>& OutObject,
	FAvidScriptEditorCSharpProfileLoadResult& OutResult)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ProfilePath))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_read_failed"),
			FString::Printf(TEXT("C# profile could not be read: %s"), *ProfilePath),
			TEXT("verify the profile file can be read and is not locked by another process"),
			OutResult);
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, OutObject) || !OutObject.IsValid())
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_json_invalid"),
			FString::Printf(TEXT("C# profile JSON is invalid: %s"), *ProfilePath),
			TEXT("fix the profile JSON syntax and rerun the command"),
			OutResult);
		return false;
	}

	return true;
}
} // namespace

FString FAvidScriptEditorCSharpProfileService::GetDefaultProfilePath()
{
	return NormalizeAvidScriptCSharpProfilePath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpProfiles"),
		TEXT("default.csharp-profile.json")));
}

bool FAvidScriptEditorCSharpProfileService::LoadProfile(
	const FString& ProfilePath,
	FAvidScriptEditorCSharpProfileLoadResult& OutResult)
{
	OutResult = FAvidScriptEditorCSharpProfileLoadResult();

	if (ProfilePath.IsEmpty())
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_empty"),
			TEXT("C# profile path is empty."),
			TEXT("choose or create a C# profile JSON file"),
			OutResult);
		return false;
	}

	OutResult.NormalizedProfilePath = NormalizeAvidScriptCSharpProfilePath(ProfilePath);
	if (!FPaths::FileExists(OutResult.NormalizedProfilePath))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_missing"),
			FString::Printf(TEXT("C# profile does not exist: %s"), *OutResult.NormalizedProfilePath),
			TEXT("create the C# profile JSON file or choose an existing profile"),
			OutResult);
		return false;
	}

	TSharedPtr<FJsonObject> ProfileObject;
	if (!LoadAvidScriptCSharpProfileJson(OutResult.NormalizedProfilePath, ProfileObject, OutResult))
	{
		return false;
	}

	double SchemaVersion = 0.0;
	if (!ProfileObject->TryGetNumberField(TEXT("schema_version"), SchemaVersion) || static_cast<int32>(SchemaVersion) != 1)
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_schema_unsupported"),
			TEXT("C# profile schema_version must be 1."),
			TEXT("update the profile JSON to schema_version 1"),
			OutResult);
		return false;
	}

	FString Language;
	if (!TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("language"), Language) || !Language.Equals(TEXT("csharp"), ESearchCase::IgnoreCase))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("profile_language_unsupported"),
			TEXT("C# profile language must be csharp."),
			TEXT("set language to csharp in the profile JSON"),
			OutResult);
		return false;
	}

	FString SourcePath;
	if (!TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("source_path"), SourcePath))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("source_empty"),
			TEXT("C# profile requires source_path."),
			TEXT("set source_path to an existing .cs file"),
			OutResult);
		return false;
	}

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.SourcePath = NormalizeAvidScriptCSharpProfileFieldPath(SourcePath);
	if (!FPaths::GetExtension(Config.SourcePath, true).Equals(TEXT(".cs"), ESearchCase::IgnoreCase))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("source_not_csharp"),
			FString::Printf(TEXT("C# profile source_path must use .cs: %s"), *Config.SourcePath),
			TEXT("choose a C# .cs source file"),
			OutResult);
		return false;
	}

	if (!FPaths::FileExists(Config.SourcePath))
	{
		SetAvidScriptCSharpProfileFailure(
			TEXT("source_missing"),
			FString::Printf(TEXT("C# profile source does not exist: %s"), *Config.SourcePath),
			TEXT("create the C# source file or update source_path"),
			OutResult);
		return false;
	}

	FString ProjectPath;
	Config.ProjectPath = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("project_path"), ProjectPath)
		? NormalizeAvidScriptCSharpProfileFieldPath(ProjectPath)
		: FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();

	FString ModuleId;
	FString ArtifactStem;
	const FString SourceToken = MakeAvidScriptCSharpProfileSafeToken(FPaths::GetBaseFilename(Config.SourcePath), TEXT("profile"));
	Config.ModuleId = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("module_id"), ModuleId)
		? ModuleId
		: FString::Printf(TEXT("csharp_%s"), *SourceToken);
	Config.ArtifactStem = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("artifact_stem"), ArtifactStem)
		? ArtifactStem
		: SourceToken;

	FString OutputRoot;
	Config.OutputRoot = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("output_root"), OutputRoot)
		? NormalizeAvidScriptCSharpProfileFieldPath(OutputRoot)
		: NormalizeAvidScriptCSharpProfilePath(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScriptCSharpGuest"),
			TEXT("Profiles"),
			Config.ArtifactStem));

	FString ReportPath;
	Config.ReportPath = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("report_path"), ReportPath)
		? NormalizeAvidScriptCSharpProfileFieldPath(ReportPath)
		: FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);

	FString ManifestPath;
	Config.ManifestPath = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("manifest_path"), ManifestPath)
		? NormalizeAvidScriptCSharpProfileFieldPath(ManifestPath)
		: FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);

	FString BuildScriptPath;
	Config.BuildScriptPath = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("build_script_path"), BuildScriptPath)
		? NormalizeAvidScriptCSharpProfileFieldPath(BuildScriptPath)
		: FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();

	FString DotNetPath;
	if (TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("dotnet_path"), DotNetPath))
	{
		Config.DotNetPath = NormalizeAvidScriptCSharpProfileFieldPath(DotNetPath);
	}

	FString Configuration;
	Config.Configuration = TryGetAvidScriptCSharpProfileStringField(ProfileObject, TEXT("configuration"), Configuration)
		? Configuration
		: FString(TEXT("Release"));

	OutResult.BuildConfig = MoveTemp(Config);
	OutResult.bSucceeded = true;
	return true;
}
