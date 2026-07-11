#include "AvidScriptWasmReload.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Ssl.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWasmReload, Log, All);

namespace
{
void ResetReloadResult(
	FAvidScriptWasmReloadResult& OutResult,
	const FString& PreviousModuleId,
	const FString& CandidateModuleId,
	const FString& ActiveModuleId)
{
	OutResult = FAvidScriptWasmReloadResult();
	OutResult.PreviousModuleId = PreviousModuleId;
	OutResult.CandidateModuleId = CandidateModuleId;
	OutResult.ActiveModuleId = ActiveModuleId;
}

void CopyRuntimeFailure(
	const FAvidScriptWasmSmokeResult& RuntimeResult,
	FAvidScriptWasmReloadResult& OutResult)
{
	OutResult.RuntimeResult = RuntimeResult;
	OutResult.ExportName = RuntimeResult.ExportName;
	OutResult.ErrorCategory = RuntimeResult.ErrorCategory;
	OutResult.NextAction = RuntimeResult.NextAction;
	OutResult.ErrorMessage = RuntimeResult.ErrorMessage;
}

void SetReloadFailure(
	FAvidScriptWasmReloadResult& OutResult,
	const FString& ExportName,
	const FString& Category,
	const FString& Details,
	const FString& NextAction)
{
	OutResult.ExportName = ExportName;
	OutResult.ErrorCategory = Category;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript reload rejected | previous=%s | candidate=%s | active=%s | export=%s | category=%s | details=%s | next=%s"),
		OutResult.PreviousModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.PreviousModuleId,
		OutResult.CandidateModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.CandidateModuleId,
		OutResult.ActiveModuleId.IsEmpty() ? TEXT("<none>") : *OutResult.ActiveModuleId,
		ExportName.IsEmpty() ? TEXT("<none>") : *ExportName,
		*Category,
		*Details,
		*NextAction);

	UE_LOG(LogAvidScriptWasmReload, Warning, TEXT("%s"), *OutResult.ErrorMessage);
}

void MarkRejectedReloadWithRollback(
	const FString& ActiveModuleId,
	FAvidScriptWasmReloadResult& OutResult)
{
	OutResult.ActiveModuleId = ActiveModuleId;
	OutResult.bRollbackPreservedLiveRuntime = !ActiveModuleId.IsEmpty();
}

void PrepareManifestLoadResult(
	FAvidScriptWasmReloadManifestLoadResult& OutResult,
	const FString& ManifestPath)
{
	OutResult = FAvidScriptWasmReloadManifestLoadResult();
	OutResult.ManifestPath = ManifestPath;
}

void SetManifestLoadFailure(
	FAvidScriptWasmReloadManifestLoadResult& OutResult,
	const FString& Category,
	const FString& Details,
	const FString& NextAction)
{
	OutResult.bSucceeded = false;
	OutResult.ByteSize = 0;
	OutResult.ErrorCategory = Category;
	OutResult.NextAction = NextAction;
	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript reload manifest load error | manifest=%s | module=%s | category=%s | details=%s | next=%s"),
		OutResult.ManifestPath.IsEmpty() ? TEXT("<none>") : *OutResult.ManifestPath,
		OutResult.ModulePath.IsEmpty() ? TEXT("<none>") : *OutResult.ModulePath,
		*Category,
		*Details,
		*NextAction);

	UE_LOG(LogAvidScriptWasmReload, Warning, TEXT("%s"), *OutResult.ErrorMessage);
}

FString NormalizeFullPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString ResolveWasmPathFromManifest(
	const FString& ManifestPath,
	const FString& WasmPath)
{
	FString NormalizedWasmPath = WasmPath;
	FPaths::NormalizeFilename(NormalizedWasmPath);

	if (!FPaths::IsRelative(NormalizedWasmPath))
	{
		return NormalizeFullPath(NormalizedWasmPath);
	}

	const FString ManifestRelativeCandidate = NormalizeFullPath(FPaths::Combine(FPaths::GetPath(ManifestPath), NormalizedWasmPath));
	const FString ProjectRelativeCandidate = NormalizeFullPath(FPaths::Combine(FPaths::ProjectDir(), NormalizedWasmPath));
	const bool bLooksProjectRelative =
		NormalizedWasmPath.StartsWith(TEXT("Saved/"), ESearchCase::IgnoreCase) ||
		NormalizedWasmPath.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase) ||
		NormalizedWasmPath.StartsWith(TEXT("Plugins/"), ESearchCase::IgnoreCase);

	TArray<FString> Candidates;
	if (bLooksProjectRelative)
	{
		Candidates.Add(ProjectRelativeCandidate);
		Candidates.Add(ManifestRelativeCandidate);
	}
	else
	{
		Candidates.Add(ManifestRelativeCandidate);
		Candidates.Add(ProjectRelativeCandidate);
	}

	for (const FString& Candidate : Candidates)
	{
		if (FPaths::FileExists(Candidate))
		{
			return Candidate;
		}
	}

	return Candidates[0];
}

FString BytesToLowerHex(const uint8* Bytes, int32 ByteCount)
{
	FString Hex;
	Hex.Reserve(ByteCount * 2);
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Hex += FString::Printf(TEXT("%02x"), Bytes[Index]);
	}
	return Hex;
}

FString ComputeSha256Hex(const TArray<uint8>& Bytes)
{
	if (Bytes.IsEmpty())
	{
		return FString();
	}

	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Digest);
	return BytesToLowerHex(Digest, UE_ARRAY_COUNT(Digest));
}

bool RequireStringField(
	const FJsonObject& Object,
	const TCHAR* FieldName,
	FString& OutValue,
	FAvidScriptWasmReloadManifestLoadResult& OutResult)
{
	if (!Object.TryGetStringField(FieldName, OutValue) || OutValue.IsEmpty())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			FString::Printf(TEXT("required string field '%s' is missing or empty"), FieldName),
			TEXT("rebuild the script manifest with all required fields"));
		return false;
	}

	return true;
}

bool RequireJsonObjectField(
	const FJsonObject& Object,
	const TCHAR* FieldName,
	TSharedPtr<FJsonObject>& OutObject,
	FAvidScriptWasmReloadManifestLoadResult& OutResult)
{
	const TSharedPtr<FJsonObject>* ObjectPtr = nullptr;
	if (!Object.TryGetObjectField(FieldName, ObjectPtr) || ObjectPtr == nullptr || !ObjectPtr->IsValid())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			FString::Printf(TEXT("required object field '%s' is missing"), FieldName),
			TEXT("rebuild the script manifest with the required object fields"));
		return false;
	}

	OutObject = *ObjectPtr;
	return true;
}
} // namespace

FAvidScriptWasmReloadManifest FAvidScriptWasmReloadManifest::MakeSmoke(const FString& InModuleId)
{
	FAvidScriptWasmReloadManifest Manifest;
	Manifest.ModuleId = InModuleId;
	Manifest.AbiVersion = SupportedAbiVersion;
	Manifest.Language = TEXT("wasm");
	Manifest.RequiredExports = {
		TEXT("avid_on_begin_play"),
		TEXT("avid_on_tick")
	};
	return Manifest;
}

bool FAvidScriptWasmReloadManifestLoader::LoadFromFile(
	const FString& ManifestPath,
	FAvidScriptWasmReloadManifest& OutManifest,
	TArray<uint8>& OutBytecode,
	FAvidScriptWasmReloadManifestLoadResult& OutResult)
{
	OutManifest = FAvidScriptWasmReloadManifest();
	OutBytecode.Reset();

	const FString ManifestFullPath = ManifestPath.IsEmpty() ? FString() : NormalizeFullPath(ManifestPath);
	PrepareManifestLoadResult(OutResult, ManifestFullPath);

	if (ManifestPath.IsEmpty())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_path_invalid"),
			TEXT("manifest path is empty"),
			TEXT("provide a .avidscript.json manifest path before loading"));
		return false;
	}

	if (!FPaths::FileExists(ManifestFullPath))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_file_missing"),
			FString::Printf(TEXT("file does not exist: %s"), *ManifestFullPath),
			TEXT("build the script artifact and manifest before loading"));
		return false;
	}

	FString ManifestJson;
	if (!FFileHelper::LoadFileToString(ManifestJson, *ManifestFullPath))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_file_read_failed"),
			FString::Printf(TEXT("failed to read manifest file: %s"), *ManifestFullPath),
			TEXT("verify manifest file permissions and retry after the writer closes it"));
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ManifestJson);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			TEXT("manifest JSON could not be parsed as an object"),
			TEXT("rewrite the manifest as valid JSON and retry"));
		return false;
	}

	int32 SchemaVersion = 0;
	if (!RootObject->TryGetNumberField(TEXT("schema_version"), SchemaVersion) ||
		SchemaVersion != FAvidScriptWasmReloadManifest::SupportedSchemaVersion)
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			FString::Printf(
				TEXT("schema_version must be %d"),
				FAvidScriptWasmReloadManifest::SupportedSchemaVersion),
			TEXT("rebuild the script manifest with the runtime supported schema"));
		return false;
	}

	FAvidScriptWasmReloadManifest Manifest;
	if (!RequireStringField(*RootObject, TEXT("module_id"), Manifest.ModuleId, OutResult))
	{
		return false;
	}

	if (!RootObject->TryGetNumberField(TEXT("abi_version"), Manifest.AbiVersion))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			TEXT("required numeric field 'abi_version' is missing"),
			TEXT("rebuild the script manifest with a supported ABI version"));
		return false;
	}

	if (Manifest.AbiVersion != FAvidScriptWasmReloadManifest::SupportedAbiVersion)
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("abi_mismatch"),
			FString::Printf(
				TEXT("manifest ABI version %d does not match supported ABI version %d"),
				Manifest.AbiVersion,
				FAvidScriptWasmReloadManifest::SupportedAbiVersion),
			TEXT("rebuild the script artifact with the runtime supported ABI"));
		return false;
	}

	if (!RequireStringField(*RootObject, TEXT("language"), Manifest.Language, OutResult))
	{
		return false;
	}

	TSharedPtr<FJsonObject> WasmObject;
	if (!RequireJsonObjectField(*RootObject, TEXT("wasm"), WasmObject, OutResult))
	{
		return false;
	}

	FString WasmPathFromManifest;
	if (!RequireStringField(*WasmObject, TEXT("file"), WasmPathFromManifest, OutResult) ||
		!RequireStringField(*WasmObject, TEXT("sha256"), Manifest.WasmSha256, OutResult))
	{
		return false;
	}

	Manifest.WasmSha256 = Manifest.WasmSha256.ToLower();
	Manifest.WasmFile = ResolveWasmPathFromManifest(ManifestFullPath, WasmPathFromManifest);
	OutResult.ModulePath = Manifest.WasmFile;

	if (!FPaths::FileExists(Manifest.WasmFile))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("module_file_missing"),
			FString::Printf(TEXT("wasm file does not exist: %s"), *Manifest.WasmFile),
			TEXT("build the WASM module before loading this manifest"));
		return false;
	}

	TArray<uint8> Bytecode;
	if (!FFileHelper::LoadFileToArray(Bytecode, *Manifest.WasmFile))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("module_file_read_failed"),
			FString::Printf(TEXT("failed to read wasm file: %s"), *Manifest.WasmFile),
			TEXT("verify wasm file permissions and retry after the writer closes it"));
		return false;
	}

	if (Bytecode.IsEmpty())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("module_file_empty"),
			FString::Printf(TEXT("wasm file is empty: %s"), *Manifest.WasmFile),
			TEXT("rebuild the WASM module and retry after the file is fully written"));
		return false;
	}

	const FString ActualSha256 = ComputeSha256Hex(Bytecode);
	if (ActualSha256.IsEmpty() || ActualSha256 != Manifest.WasmSha256)
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("module_hash_mismatch"),
			FString::Printf(
				TEXT("manifest sha256 %s does not match wasm sha256 %s"),
				*Manifest.WasmSha256,
				ActualSha256.IsEmpty() ? TEXT("<failed>") : *ActualSha256),
			TEXT("rebuild the manifest after the WASM file is fully written"));
		return false;
	}

	if (!RootObject->TryGetStringArrayField(TEXT("required_exports"), Manifest.RequiredExports) ||
		Manifest.RequiredExports.IsEmpty())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			TEXT("required_exports must contain at least one export name"),
			TEXT("declare required guest exports before activating this module"));
		return false;
	}

	for (const FString& RequiredExport : Manifest.RequiredExports)
	{
		if (RequiredExport.IsEmpty())
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("manifest_invalid"),
				TEXT("required_exports contains an empty export name"),
				TEXT("remove empty export names from the manifest"));
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* RequiredImportValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("required_imports"), RequiredImportValues) ||
		RequiredImportValues == nullptr || RequiredImportValues->IsEmpty())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			TEXT("required_imports must contain at least one import"),
			TEXT("declare required host imports before activating this module"));
		return false;
	}

	for (const TSharedPtr<FJsonValue>& ImportValue : *RequiredImportValues)
	{
		const TSharedPtr<FJsonObject> ImportObject = ImportValue.IsValid() ? ImportValue->AsObject() : nullptr;
		if (!ImportObject.IsValid())
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("manifest_invalid"),
				TEXT("required_imports contains a non-object value"),
				TEXT("write each required import as an object with module and name fields"));
			return false;
		}

		FAvidScriptWasmRequiredImport RequiredImport;
		if (!RequireStringField(*ImportObject, TEXT("module"), RequiredImport.ModuleName, OutResult) ||
			!RequireStringField(*ImportObject, TEXT("name"), RequiredImport.ImportName, OutResult))
		{
			return false;
		}

		Manifest.RequiredImports.Add(MoveTemp(RequiredImport));
	}

	OutManifest = MoveTemp(Manifest);
	OutBytecode = MoveTemp(Bytecode);
	OutResult.bSucceeded = true;
	OutResult.ByteSize = OutBytecode.Num();
	return true;
}

FAvidScriptWasmReloadSession::~FAvidScriptWasmReloadSession()
{
	UnloadLive();
}

bool FAvidScriptWasmReloadSession::LoadInitialModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult)
{
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, Manifest.ModuleId, PreviousModuleId);

	if (!ValidateManifest(Manifest, PreviousModuleId, OutResult))
	{
		return false;
	}

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime;
	if (!BuildValidatedRuntime(Bytecode, BytecodeSize, Manifest, CandidateRuntime, OutResult))
	{
		OutResult.ActiveModuleId = PreviousModuleId;
		return false;
	}

	if (!ActivateValidatedRuntime(CandidateRuntime, Manifest, OutResult))
	{
		OutResult.ActiveModuleId = GetLiveModuleId();
		return false;
	}

	OutResult.bSucceeded = true;
	OutResult.ActiveModuleId = Manifest.ModuleId;
	return true;
}

bool FAvidScriptWasmReloadSession::ReloadModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult)
{
	const FString PreviousModuleId = GetLiveModuleId();
	ResetReloadResult(OutResult, PreviousModuleId, Manifest.ModuleId, PreviousModuleId);

	if (!ValidateManifest(Manifest, PreviousModuleId, OutResult))
	{
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}

	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime;
	if (!BuildValidatedRuntime(Bytecode, BytecodeSize, Manifest, CandidateRuntime, OutResult))
	{
		++RejectedReloadCount;
		MarkRejectedReloadWithRollback(PreviousModuleId, OutResult);
		return false;
	}

	if (!ActivateValidatedRuntime(CandidateRuntime, Manifest, OutResult))
	{
		++RejectedReloadCount;
		const FString ActiveModuleId = GetLiveModuleId();
		MarkRejectedReloadWithRollback(ActiveModuleId, OutResult);
		return false;
	}

	++SuccessfulReloadCount;
	OutResult.bSucceeded = true;
	OutResult.bReloadApplied = true;
	OutResult.ActiveModuleId = Manifest.ModuleId;
	return true;
}
void FAvidScriptWasmReloadSession::SetHostContext(const FAvidScriptWasmHostContext& InHostContext)
{
	HostContext = InHostContext;
	if (LiveRuntime)
	{
		LiveRuntime->SetHostContext(HostContext);
	}
}

void FAvidScriptWasmReloadSession::ClearHostContext()
{
	HostContext = FAvidScriptWasmHostContext();
	if (LiveRuntime)
	{
		LiveRuntime->ClearHostContext();
	}
}

bool FAvidScriptWasmReloadSession::TickLive(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	if (!IsLiveLoaded())
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ModuleId = LiveManifest.ModuleId;
		OutResult.ExportName = TEXT("avid_on_tick");
		OutResult.ErrorCategory = TEXT("invalid_state");
		OutResult.NextAction = TEXT("load a validated WASM module before ticking the live script runtime");
		OutResult.ErrorMessage = FString::Printf(
			TEXT("AvidScript live runtime tick rejected | module=%s | category=invalid_state | details=no live runtime is loaded"),
			LiveManifest.ModuleId.IsEmpty() ? TEXT("<none>") : *LiveManifest.ModuleId);
		return false;
	}

	return LiveRuntime->Tick(DeltaSeconds, OutResult);
}

bool FAvidScriptWasmReloadSession::EndPlayLive(FAvidScriptWasmSmokeResult& OutResult)
{
	if (!IsLiveLoaded())
	{
		OutResult = FAvidScriptWasmSmokeResult();
		OutResult.ModuleId = LiveManifest.ModuleId;
		OutResult.ExportName = TEXT("avid_on_end_play");
		OutResult.ErrorCategory = TEXT("invalid_state");
		OutResult.NextAction = TEXT("load a validated WASM module before ending the live script runtime");
		OutResult.ErrorMessage = FString::Printf(
			TEXT("AvidScript live runtime EndPlay rejected | module=%s | category=invalid_state | details=no live runtime is loaded"),
			LiveManifest.ModuleId.IsEmpty() ? TEXT("<none>") : *LiveManifest.ModuleId);
		return false;
	}

	return LiveRuntime->EndPlay(OutResult);
}

void FAvidScriptWasmReloadSession::UnloadLive()
{
	if (LiveRuntime)
	{
		if (LiveRuntime->HasBegunPlay())
		{
			FAvidScriptWasmSmokeResult EndPlayResult;
			EndPlayLive(EndPlayResult);
		}
		LiveRuntime->Unload();
		LiveRuntime.Reset();
	}
	LiveManifest = FAvidScriptWasmReloadManifest();
}

bool FAvidScriptWasmReloadSession::IsLiveLoaded() const
{
	return LiveRuntime.IsValid() && LiveRuntime->IsLoaded();
}

FString FAvidScriptWasmReloadSession::GetLiveModuleId() const
{
	return IsLiveLoaded() ? LiveRuntime->GetModuleId() : FString();
}

int32 FAvidScriptWasmReloadSession::GetLiveTickCallCount() const
{
	return IsLiveLoaded() ? LiveRuntime->GetTickCallCount() : 0;
}

int32 FAvidScriptWasmReloadSession::GetLivePendingTimerCount() const
{
	return IsLiveLoaded() ? LiveRuntime->GetPendingTimerCount() : 0;
}

int32 FAvidScriptWasmReloadSession::GetLiveTimerCallbackCount() const
{
	return IsLiveLoaded() ? LiveRuntime->GetTimerCallbackCount() : 0;
}

bool FAvidScriptWasmReloadSession::ValidateManifest(
	const FAvidScriptWasmReloadManifest& Manifest,
	const FString& PreviousModuleId,
	FAvidScriptWasmReloadResult& OutResult) const
{
	OutResult.PreviousModuleId = PreviousModuleId;
	OutResult.CandidateModuleId = Manifest.ModuleId;

	if (Manifest.ModuleId.IsEmpty())
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("manifest_invalid"),
			TEXT("manifest ModuleId is empty"),
			TEXT("assign a stable module id before loading or reloading a script"));
		return false;
	}

	if (Manifest.AbiVersion != FAvidScriptWasmReloadManifest::SupportedAbiVersion)
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("abi_mismatch"),
			FString::Printf(
				TEXT("manifest ABI version %d does not match supported ABI version %d"),
				Manifest.AbiVersion,
				FAvidScriptWasmReloadManifest::SupportedAbiVersion),
			TEXT("rebuild the script with the runtime supported ABI or keep the previous live runtime"));
		return false;
	}

	if (Manifest.RequiredExports.IsEmpty())
	{
		SetReloadFailure(
			OutResult,
			TEXT("<manifest>"),
			TEXT("manifest_invalid"),
			TEXT("manifest RequiredExports is empty"),
			TEXT("declare the guest exports that must be validated before activation"));
		return false;
	}

	return true;
}

bool FAvidScriptWasmReloadSession::BuildValidatedRuntime(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FAvidScriptWasmReloadManifest& Manifest,
	TUniquePtr<FAvidScriptWasmRuntimeInstance>& OutRuntime,
	FAvidScriptWasmReloadResult& OutResult) const
{
	TUniquePtr<FAvidScriptWasmRuntimeInstance> CandidateRuntime = MakeUnique<FAvidScriptWasmRuntimeInstance>();
	FAvidScriptWasmSmokeResult RuntimeResult;

	if (!CandidateRuntime->LoadModule(Bytecode, BytecodeSize, Manifest.ModuleId, RuntimeResult))
	{
		CopyRuntimeFailure(RuntimeResult, OutResult);
		return false;
	}

	if (!CandidateRuntime->ValidateRequiredExports(Manifest.RequiredExports, RuntimeResult))
	{
		CopyRuntimeFailure(RuntimeResult, OutResult);
		CandidateRuntime->Unload();
		return false;
	}

	CandidateRuntime->SetHostContext(HostContext);
	OutResult.RuntimeResult = RuntimeResult;
	OutRuntime = MoveTemp(CandidateRuntime);
	return true;
}

bool FAvidScriptWasmReloadSession::ActivateValidatedRuntime(
	TUniquePtr<FAvidScriptWasmRuntimeInstance>& CandidateRuntime,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadResult& OutResult)
{
	if (!CandidateRuntime)
	{
		SetReloadFailure(
			OutResult,
			TEXT("<runtime>"),
			TEXT("invalid_state"),
			TEXT("validated candidate runtime is missing"),
			TEXT("rebuild and validate the candidate before activation"));
		return false;
	}

	if (LiveRuntime)
	{
		if (LiveRuntime->HasBegunPlay())
		{
			FAvidScriptWasmSmokeResult PreviousEndPlayResult;
			if (!LiveRuntime->EndPlay(PreviousEndPlayResult))
			{
				CopyRuntimeFailure(PreviousEndPlayResult, OutResult);
				LiveRuntime->Unload();
				LiveRuntime.Reset();
				LiveManifest = FAvidScriptWasmReloadManifest();
				CandidateRuntime->Unload();
				return false;
			}
		}

		LiveRuntime->Unload();
		LiveRuntime.Reset();
		LiveManifest = FAvidScriptWasmReloadManifest();
	}

	FAvidScriptWasmSmokeResult BeginPlayResult;
	if (!CandidateRuntime->BeginPlay(BeginPlayResult))
	{
		CopyRuntimeFailure(BeginPlayResult, OutResult);
		CandidateRuntime->Unload();
		return false;
	}

	OutResult.RuntimeResult = BeginPlayResult;
	LiveRuntime = MoveTemp(CandidateRuntime);
	LiveManifest = Manifest;
	return true;
}
