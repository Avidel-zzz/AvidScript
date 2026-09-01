#include "AvidScriptEditorCSharpSourceIndexService.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#include <openssl/sha.h>

namespace
{
constexpr int32 AvidScriptSourceIndexSchemaVersion = 1;
constexpr const TCHAR* AvidScriptSourceIndexFileName = TEXT("AvidScript.SourceIndex.json");

FString NormalizeAvidScriptSourceIndexPath(FString Path)
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

void SetAvidScriptSourceIndexFailure(
	const FString& Category,
	const FString& Message,
	const FString& NextAction,
	FAvidScriptEditorCSharpSourceIndexResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.ErrorCategory = Category;
	OutResult.ErrorMessage = Message;
	OutResult.NextAction = NextAction;
}

bool TryGetAvidScriptSourceIndexSha256(
	const FString& Text,
	FString& OutSha256)
{
	const FTCHARToUTF8 Utf8(*Text);
	uint8 Digest[SHA256_DIGEST_LENGTH];
	if (::SHA256(
			reinterpret_cast<const unsigned char*>(Utf8.Get()),
			static_cast<size_t>(Utf8.Length()),
			Digest) == nullptr)
	{
		return false;
	}
	OutSha256 = BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	return true;
}

bool IsAvidScriptSourceIndexPortableId(const FString& Value)
{
	return !Value.IsEmpty()
		&& FPaths::IsRelative(Value)
		&& !Value.Equals(TEXT(".."))
		&& !Value.StartsWith(TEXT("../"))
		&& !Value.Contains(TEXT("/../"));
}

bool IsAvidScriptSourceIndexSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
	}
	return true;
}

bool MakeAvidScriptSourceIndexProjectRelative(
	const FString& ProjectRoot,
	const FString& Candidate,
	FString& OutRelative)
{
	const FString NormalizedRoot = NormalizeAvidScriptSourceIndexPath(ProjectRoot);
	const FString NormalizedCandidate = NormalizeAvidScriptSourceIndexPath(Candidate);
	if (NormalizedRoot.IsEmpty()
		|| NormalizedCandidate.IsEmpty()
		|| (!NormalizedCandidate.Equals(NormalizedRoot, ESearchCase::IgnoreCase)
			&& !NormalizedCandidate.StartsWith(
				NormalizedRoot + TEXT("/"),
				ESearchCase::IgnoreCase)))
	{
		return false;
	}

	OutRelative = NormalizedCandidate;
	FString RelativeBase = NormalizedRoot;
	if (!RelativeBase.EndsWith(TEXT("/")))
	{
		RelativeBase += TEXT("/");
	}
	if (!FPaths::MakePathRelativeTo(OutRelative, *RelativeBase))
	{
		return false;
	}
	FPaths::NormalizeFilename(OutRelative);
	return !OutRelative.IsEmpty()
		&& !OutRelative.StartsWith(TEXT("../"));
}

bool AddAvidScriptSourceIndexEntry(
	const FString& ProjectRoot,
	const FString& SourcePath,
	const FString& Kind,
	TArray<FAvidScriptEditorCSharpSourceIndexEntry>& OutSources,
	FAvidScriptEditorCSharpSourceIndexResult& OutResult)
{
	FString SourceId;
	if (!MakeAvidScriptSourceIndexProjectRelative(ProjectRoot, SourcePath, SourceId))
	{
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_path_outside_project"),
			FString::Printf(TEXT("Indexed C# source must remain inside the project: %s"), *SourcePath),
			TEXT("move the C# workspace and generated facade inside the current UE project"),
			OutResult);
		return false;
	}

	FString SourceText;
	FString SourceSha256;
	if (!FFileHelper::LoadFileToString(SourceText, *SourcePath)
		|| !TryGetAvidScriptSourceIndexSha256(SourceText, SourceSha256))
	{
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_source_unreadable"),
			FString::Printf(TEXT("Indexed C# source could not be read: %s"), *SourcePath),
			TEXT("restore the source file and refresh the C# workspace"),
			OutResult);
		return false;
	}

	FAvidScriptEditorCSharpSourceIndexEntry Entry;
	Entry.SourceId = SourceId;
	Entry.Sha256 = SourceSha256;
	Entry.Kind = Kind;
	OutSources.Add(MoveTemp(Entry));
	return true;
}

bool SerializeAvidScriptSourceIndex(
	const FAvidScriptEditorCSharpSourceIndex& Index,
	FString& OutJson)
{
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutJson);
	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("schema_version"), Index.SchemaVersion);
	Writer->WriteValue(TEXT("workspace"), Index.WorkspaceId);
	Writer->WriteValue(TEXT("solution"), Index.SolutionId);
	Writer->WriteValue(TEXT("project"), Index.ProjectId);
	Writer->WriteArrayStart(TEXT("sources"));
	for (const FAvidScriptEditorCSharpSourceIndexEntry& Source : Index.Sources)
	{
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("path"), Source.SourceId);
		Writer->WriteValue(TEXT("sha256"), Source.Sha256);
		Writer->WriteValue(TEXT("kind"), Source.Kind);
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	return Writer->Close();
}

bool WriteAvidScriptSourceIndexAtomic(
	const FString& Path,
	const FString& Text,
	FAvidScriptEditorCSharpSourceIndexResult& OutResult)
{
	if (IFileManager::Get().DirectoryExists(*Path))
	{
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_file_is_directory"),
			FString::Printf(TEXT("C# source index path is a directory: %s"), *Path),
			TEXT("move the conflicting directory and refresh the C# workspace"),
			OutResult);
		return false;
	}
	if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true))
	{
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_directory_failed"),
			FString::Printf(TEXT("C# source index directory could not be created: %s"), *FPaths::GetPath(Path)),
			TEXT("verify project write permissions and refresh the C# workspace"),
			OutResult);
		return false;
	}

	const FString TemporaryPath = Path + TEXT(".tmp.")
		+ FGuid::NewGuid().ToString(EGuidFormats::Digits);
	if (!FFileHelper::SaveStringToFile(
			Text,
			*TemporaryPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)
		|| !IFileManager::Get().Move(*Path, *TemporaryPath, true, true, false, true))
	{
		IFileManager::Get().Delete(*TemporaryPath, false, true, true);
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_publish_failed"),
			FString::Printf(TEXT("C# source index could not be published atomically: %s"), *Path),
			TEXT("close readers holding the index and refresh the C# workspace"),
			OutResult);
		return false;
	}
	return true;
}
} // namespace

FString FAvidScriptEditorCSharpSourceIndexService::MakeDefaultPath(
	const FString& GeneratedRoot)
{
	return NormalizeAvidScriptSourceIndexPath(FPaths::Combine(
		GeneratedRoot,
		AvidScriptSourceIndexFileName));
}

bool FAvidScriptEditorCSharpSourceIndexService::Publish(
	const FAvidScriptEditorCSharpSourceIndexConfig& Config,
	FAvidScriptEditorCSharpSourceIndexResult& OutResult)
{
	OutResult = FAvidScriptEditorCSharpSourceIndexResult();
	OutResult.IndexPath = NormalizeAvidScriptSourceIndexPath(Config.OutputPath);
	FString OutputId;
	if (!MakeAvidScriptSourceIndexProjectRelative(
			Config.ProjectRoot,
			OutResult.IndexPath,
			OutputId))
	{
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_output_outside_project"),
			FString::Printf(TEXT("C# source index must remain inside the project: %s"), *OutResult.IndexPath),
			TEXT("choose a generated root inside the current UE project"),
			OutResult);
		return false;
	}

	FAvidScriptEditorCSharpSourceIndex Index;
	Index.SchemaVersion = AvidScriptSourceIndexSchemaVersion;
	if (!MakeAvidScriptSourceIndexProjectRelative(Config.ProjectRoot, Config.WorkspaceRoot, Index.WorkspaceId)
		|| !MakeAvidScriptSourceIndexProjectRelative(Config.ProjectRoot, Config.SolutionPath, Index.SolutionId)
		|| !MakeAvidScriptSourceIndexProjectRelative(Config.ProjectRoot, Config.ProjectPath, Index.ProjectId))
	{
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_workspace_outside_project"),
			TEXT("C# workspace metadata must remain inside the current UE project."),
			TEXT("move the C# workspace inside the current UE project"),
			OutResult);
		return false;
	}
	if (!AddAvidScriptSourceIndexEntry(
			Config.ProjectRoot,
			Config.UserSourcePath,
			TEXT("user"),
			Index.Sources,
			OutResult)
		|| !AddAvidScriptSourceIndexEntry(
			Config.ProjectRoot,
			Config.GeneratedSourcePath,
			TEXT("generated"),
			Index.Sources,
			OutResult))
	{
		return false;
	}
	Index.Sources.Sort([](
		const FAvidScriptEditorCSharpSourceIndexEntry& Left,
		const FAvidScriptEditorCSharpSourceIndexEntry& Right)
	{
		return Left.SourceId < Right.SourceId;
	});

	FString Json;
	if (!SerializeAvidScriptSourceIndex(Index, Json)
		|| !TryGetAvidScriptSourceIndexSha256(Json, OutResult.IndexSha256)
		|| !WriteAvidScriptSourceIndexAtomic(OutResult.IndexPath, Json, OutResult))
	{
		if (OutResult.ErrorCategory.IsEmpty())
		{
			SetAvidScriptSourceIndexFailure(
				TEXT("source_index_serialize_failed"),
				TEXT("C# source index could not be serialized."),
				TEXT("repair the source index schema and refresh the C# workspace"),
				OutResult);
		}
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.SourceCount = Index.Sources.Num();
	return true;
}

bool FAvidScriptEditorCSharpSourceIndexService::Load(
	const FString& IndexPath,
	FAvidScriptEditorCSharpSourceIndex& OutIndex,
	FAvidScriptEditorCSharpSourceIndexResult& OutResult)
{
	OutIndex = FAvidScriptEditorCSharpSourceIndex();
	OutResult = FAvidScriptEditorCSharpSourceIndexResult();
	OutResult.IndexPath = NormalizeAvidScriptSourceIndexPath(IndexPath);
	FString Json;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Json, *OutResult.IndexPath)
		|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root)
		|| !Root.IsValid())
	{
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_invalid_json"),
			FString::Printf(TEXT("C# source index could not be parsed: %s"), *OutResult.IndexPath),
			TEXT("refresh the C# workspace to rebuild the source index"),
			OutResult);
		return false;
	}

	double SchemaVersion = 0.0;
	const TArray<TSharedPtr<FJsonValue>>* Sources = nullptr;
	if (!Root->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
		|| static_cast<int32>(SchemaVersion) != AvidScriptSourceIndexSchemaVersion
		|| !Root->TryGetStringField(TEXT("workspace"), OutIndex.WorkspaceId)
		|| !Root->TryGetStringField(TEXT("solution"), OutIndex.SolutionId)
		|| !Root->TryGetStringField(TEXT("project"), OutIndex.ProjectId)
		|| !Root->TryGetArrayField(TEXT("sources"), Sources)
		|| Sources == nullptr
		|| !IsAvidScriptSourceIndexPortableId(OutIndex.WorkspaceId)
		|| !IsAvidScriptSourceIndexPortableId(OutIndex.SolutionId)
		|| !IsAvidScriptSourceIndexPortableId(OutIndex.ProjectId))
	{
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_schema_invalid"),
			TEXT("C# source index schema is incomplete or unsupported."),
			TEXT("refresh the C# workspace with the current AvidScript plugin"),
			OutResult);
		return false;
	}
	OutIndex.SchemaVersion = static_cast<int32>(SchemaVersion);
	for (const TSharedPtr<FJsonValue>& Value : *Sources)
	{
		const TSharedPtr<FJsonObject> Source = Value.IsValid() ? Value->AsObject() : nullptr;
		FAvidScriptEditorCSharpSourceIndexEntry Entry;
		if (!Source.IsValid()
			|| !Source->TryGetStringField(TEXT("path"), Entry.SourceId)
			|| !Source->TryGetStringField(TEXT("sha256"), Entry.Sha256)
			|| !Source->TryGetStringField(TEXT("kind"), Entry.Kind)
			|| !IsAvidScriptSourceIndexPortableId(Entry.SourceId)
			|| !IsAvidScriptSourceIndexSha256(Entry.Sha256)
			|| (Entry.Kind != TEXT("user") && Entry.Kind != TEXT("generated")))
		{
			SetAvidScriptSourceIndexFailure(
				TEXT("source_index_entry_invalid"),
				TEXT("C# source index contains an invalid source entry."),
				TEXT("refresh the C# workspace to rebuild the source index"),
				OutResult);
			return false;
		}
		OutIndex.Sources.Add(MoveTemp(Entry));
	}

	if (!TryGetAvidScriptSourceIndexSha256(Json, OutResult.IndexSha256))
	{
		SetAvidScriptSourceIndexFailure(
			TEXT("source_index_hash_failed"),
			TEXT("C# source index hash could not be calculated."),
			TEXT("refresh the C# workspace and retry"),
			OutResult);
		return false;
	}
	OutResult.bSucceeded = true;
	OutResult.SourceCount = OutIndex.Sources.Num();
	return true;
}
