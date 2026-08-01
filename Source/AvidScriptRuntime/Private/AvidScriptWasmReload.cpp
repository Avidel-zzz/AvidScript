#include "AvidScriptWasmReload.h"

#include "Diagnostics/AvidScriptWasmDebugMap.h"
#include "Validation/AvidScriptWasmImportPolicy.h"

#include "AvidScriptVmBackend.h"
#include "AvidScriptWasmModuleLayout.h"
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

FString ResolveArtifactPathFromManifest(
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

bool TryResolveDebugMapPathFromManifest(
	const FString& ManifestPath,
	const FString& RelativePath,
	FString& OutPath)
{
	OutPath.Reset();
	if (RelativePath.IsEmpty()
		|| !FPaths::IsRelative(RelativePath)
		|| RelativePath.StartsWith(TEXT("/"))
		|| RelativePath.Contains(TEXT(":"))
		|| RelativePath.Contains(TEXT("\\")))
	{
		return false;
	}

	FString Collapsed = RelativePath;
	FPaths::CollapseRelativeDirectories(Collapsed, true);
	FPaths::NormalizeFilename(Collapsed);
	if (Collapsed != RelativePath
		|| RelativePath.Contains(TEXT("//"))
		|| RelativePath.StartsWith(TEXT("./"))
		|| RelativePath.EndsWith(TEXT("/")))
	{
		return false;
	}

	OutPath = ResolveArtifactPathFromManifest(ManifestPath, RelativePath);
	FString ProjectRoot = NormalizeFullPath(FPaths::ProjectDir());
	FString ManifestRoot = NormalizeFullPath(FPaths::GetPath(ManifestPath));
	if (!ProjectRoot.EndsWith(TEXT("/")))
	{
		ProjectRoot += TEXT("/");
	}
	if (!ManifestRoot.EndsWith(TEXT("/")))
	{
		ManifestRoot += TEXT("/");
	}
	return OutPath.StartsWith(ProjectRoot, ESearchCase::IgnoreCase)
		|| OutPath.StartsWith(ManifestRoot, ESearchCase::IgnoreCase);
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

bool LoadAndVerifyBindingArtifact(
	const FString& Path,
	const FString& ExpectedSha256,
	const FString& Label,
	TArray<uint8>& OutBytes,
	FAvidScriptWasmReloadManifestLoadResult& OutResult)
{
	OutBytes.Reset();
	if (!FPaths::FileExists(Path))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_file_missing"),
			FString::Printf(TEXT("%s file does not exist: %s"), *Label, *Path),
			TEXT("republish the generated binding package and rebuild the script manifest"));
		return false;
	}
	if (!FFileHelper::LoadFileToArray(OutBytes, *Path))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_file_read_failed"),
			FString::Printf(TEXT("failed to read %s file: %s"), *Label, *Path),
			TEXT("verify binding package file permissions and retry after the writer closes it"));
		return false;
	}
	if (OutBytes.IsEmpty())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_file_empty"),
			FString::Printf(TEXT("%s file is empty: %s"), *Label, *Path),
			TEXT("republish the generated binding package before loading this script"));
		return false;
	}

	const FString ActualSha256 = ComputeSha256Hex(OutBytes);
	if (ActualSha256.IsEmpty() || ActualSha256 != ExpectedSha256)
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_hash_mismatch"),
			FString::Printf(
				TEXT("%s manifest sha256 %s does not match file sha256 %s"),
				*Label,
				*ExpectedSha256,
				ActualSha256.IsEmpty() ? TEXT("<failed>") : *ActualSha256),
			TEXT("rebuild the script against the current content-addressed binding package"));
		return false;
	}

	return true;
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

bool IsLowercaseSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character) && (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool TryGetInt32Field(const FJsonObject& Object, const TCHAR* FieldName, int32& OutValue)
{
	double Number = 0.0;
	if (!Object.TryGetNumberField(FieldName, Number)
		|| !FMath::IsFinite(Number)
		|| Number < static_cast<double>(MIN_int32)
		|| Number > static_cast<double>(MAX_int32)
		|| Number != FMath::TruncToDouble(Number))
	{
		return false;
	}

	OutValue = static_cast<int32>(Number);
	return true;
}

bool LoadStateMigrationManifest(
	const FJsonObject& Object,
	FAvidScriptWasmStateMigrationManifest& OutManifest,
	FAvidScriptWasmReloadManifestLoadResult& OutResult)
{
	OutManifest = FAvidScriptWasmStateMigrationManifest();
	int32 SchemaVersion = 0;
	FString Strategy;
	if (!TryGetInt32Field(Object, TEXT("schema_version"), SchemaVersion)
		|| SchemaVersion < FAvidScriptWasmStateMigrationManifest::LegacySchemaVersion
		|| SchemaVersion > FAvidScriptWasmStateMigrationManifest::SupportedSchemaVersion
		|| !RequireStringField(Object, TEXT("strategy"), Strategy, OutResult)
		|| Strategy != TEXT("host_snapshot")
		|| !RequireStringField(Object, TEXT("owner_type_id"), OutManifest.OwnerTypeId, OutResult))
	{
		if (OutResult.ErrorCategory.IsEmpty())
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("manifest_invalid"),
				TEXT("state_migration schema_version or strategy is unsupported"),
				TEXT("rebuild the C# script with the current AvidScript guest compiler"));
		}
		return false;
	}
	OutManifest.SchemaVersion = SchemaVersion;
	if (SchemaVersion == FAvidScriptWasmStateMigrationManifest::SupportedSchemaVersion)
	{
		if (!RequireStringField(Object, TEXT("policy"), OutManifest.Policy, OutResult)
			|| !TryGetInt32Field(Object, TEXT("contract_version"), OutManifest.ContractVersion)
			|| (OutManifest.Policy != TEXT("compatible") && OutManifest.Policy != TEXT("explicit"))
			|| OutManifest.ContractVersion < FAvidScriptWasmStateMigrationManifest::MinContractVersion
			|| OutManifest.ContractVersion > FAvidScriptWasmStateMigrationManifest::MaxContractVersion)
		{
			if (OutResult.ErrorCategory.IsEmpty())
			{
				SetManifestLoadFailure(
					OutResult,
					TEXT("manifest_invalid"),
					TEXT("state_migration v2 policy or contract_version is invalid"),
					TEXT("rebuild the C# script state schema with a supported policy and contract version"));
			}
			return false;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* SlotValues = nullptr;
	if (!Object.TryGetArrayField(TEXT("slots"), SlotValues) || SlotValues == nullptr
		|| SlotValues->Num() > FAvidScriptWasmStateMigrationManifest::MaxSlotCount)
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			TEXT("state_migration slots are missing or exceed the supported slot count"),
			TEXT("reduce persistent C# state or rebuild the script manifest"));
		return false;
	}

	uint64 TotalByteSize = 0;
	FString PreviousStableId;
	TArray<FAvidScriptWasmStateSlot> Slots;
	Slots.Reserve(SlotValues->Num());
	for (int32 Index = 0; Index < SlotValues->Num(); ++Index)
	{
		const TSharedPtr<FJsonObject> SlotObject = (*SlotValues)[Index].IsValid()
			? (*SlotValues)[Index]->AsObject()
			: nullptr;
		FAvidScriptWasmStateSlot Slot;
		int32 Offset = 0;
		int32 Size = 0;
		int32 Alignment = 0;
		const TArray<TSharedPtr<FJsonValue>>* AliasValues = nullptr;
		if (!SlotObject.IsValid()
			|| !RequireStringField(*SlotObject, TEXT("stable_id"), Slot.StableId, OutResult)
			|| !RequireStringField(*SlotObject, TEXT("type_fingerprint"), Slot.TypeFingerprint, OutResult)
			|| !TryGetInt32Field(*SlotObject, TEXT("offset"), Offset)
			|| !TryGetInt32Field(*SlotObject, TEXT("size"), Size)
			|| !TryGetInt32Field(*SlotObject, TEXT("alignment"), Alignment))
		{
			if (OutResult.ErrorCategory.IsEmpty())
			{
				SetManifestLoadFailure(
					OutResult,
					TEXT("manifest_invalid"),
					TEXT("state_migration slot fields are missing or invalid"),
					TEXT("rebuild the script state schema"));
			}
			return false;
		}
		if (SchemaVersion == FAvidScriptWasmStateMigrationManifest::LegacySchemaVersion)
		{
			if (SlotObject->HasField(TEXT("aliases")))
			{
				SetManifestLoadFailure(
					OutResult,
					TEXT("manifest_invalid"),
					TEXT("state_migration v1 slots must not declare aliases"),
					TEXT("remove aliases from v1 artifacts or rebuild the script manifest as schema v2"));
				return false;
			}
		}
		else if (!SlotObject->TryGetArrayField(TEXT("aliases"), AliasValues) || AliasValues == nullptr)
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("manifest_invalid"),
				TEXT("state_migration v2 slot aliases must be an array"),
				TEXT("rebuild the script state schema with deterministic aliases"));
			return false;
		}

		const bool bStableIdOrdered = Index == 0
			|| PreviousStableId.Compare(Slot.StableId, ESearchCase::CaseSensitive) < 0;
		const bool bAlignmentValid = Alignment > 0
			&& (Alignment & (Alignment - 1)) == 0
			&& Alignment <= static_cast<int32>(FAvidScriptWasmStateMigrationManifest::MaxSlotByteSize);
		const uint64 End = Offset >= 0 && Size > 0
			? static_cast<uint64>(Offset) + static_cast<uint64>(Size)
			: MAX_uint64;
		if (!bStableIdOrdered
			|| !IsLowercaseSha256(Slot.TypeFingerprint)
			|| Offset < 0
			|| Size <= 0
			|| Size > static_cast<int32>(FAvidScriptWasmStateMigrationManifest::MaxSlotByteSize)
			|| !bAlignmentValid
			|| (Offset % Alignment) != 0
			|| End > MAX_uint32)
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("manifest_invalid"),
				FString::Printf(TEXT("state_migration slot is unsafe or non-deterministic: %s"), *Slot.StableId),
				TEXT("rebuild the script state schema with the current guest compiler"));
			return false;
		}

		TotalByteSize += static_cast<uint64>(Size);
		if (TotalByteSize > FAvidScriptWasmStateMigrationManifest::MaxTotalByteSize)
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("manifest_invalid"),
				TEXT("state_migration total byte size exceeds the supported limit"),
				TEXT("reduce persistent C# state or use a future explicit serializer"));
			return false;
		}

		Slot.Offset = static_cast<uint32>(Offset);
		Slot.Size = static_cast<uint32>(Size);
		Slot.Alignment = static_cast<uint32>(Alignment);
		if (AliasValues != nullptr)
		{
			FString PreviousAlias;
			for (const TSharedPtr<FJsonValue>& AliasValue : *AliasValues)
			{
				FString Alias;
				if (!AliasValue.IsValid()
					|| !AliasValue->TryGetString(Alias)
					|| Alias.IsEmpty()
					|| (!PreviousAlias.IsEmpty()
						&& PreviousAlias.Compare(Alias, ESearchCase::CaseSensitive) >= 0))
				{
					SetManifestLoadFailure(
						OutResult,
						TEXT("manifest_invalid"),
						TEXT("state_migration v2 aliases must be non-empty strings in strict ordinal order"),
						TEXT("rebuild the script state schema with sorted unique aliases"));
					return false;
				}
				PreviousAlias = Alias;
				Slot.Aliases.Add(MoveTemp(Alias));
			}
		}
		PreviousStableId = Slot.StableId;
		Slots.Add(MoveTemp(Slot));
	}

	TSet<FString> StateIds;
	for (const FAvidScriptWasmStateSlot& Slot : Slots)
	{
		StateIds.Add(Slot.StableId);
	}
	for (const FAvidScriptWasmStateSlot& Slot : Slots)
	{
		for (const FString& Alias : Slot.Aliases)
		{
			if (StateIds.Contains(Alias))
			{
				SetManifestLoadFailure(
					OutResult,
					TEXT("manifest_invalid"),
					TEXT("state_migration aliases must be globally unambiguous with current ids and aliases"),
					TEXT("rename the conflicting state alias and rebuild the script manifest"));
				return false;
			}
			StateIds.Add(Alias);
		}
	}

	TArray<const FAvidScriptWasmStateSlot*> SlotsByAddress;
	SlotsByAddress.Reserve(Slots.Num());
	for (const FAvidScriptWasmStateSlot& Slot : Slots)
	{
		SlotsByAddress.Add(&Slot);
	}
	SlotsByAddress.Sort([](const FAvidScriptWasmStateSlot& Left, const FAvidScriptWasmStateSlot& Right)
	{
		return Left.Offset < Right.Offset;
	});
	for (int32 Index = 1; Index < SlotsByAddress.Num(); ++Index)
	{
		const FAvidScriptWasmStateSlot& Previous = *SlotsByAddress[Index - 1];
		const FAvidScriptWasmStateSlot& Current = *SlotsByAddress[Index];
		if (static_cast<uint64>(Current.Offset)
			< static_cast<uint64>(Previous.Offset) + Previous.Size)
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("manifest_invalid"),
				TEXT("state_migration slots overlap"),
				TEXT("rebuild the script state schema from deterministic Guest IR layout"));
			return false;
		}
	}

	OutManifest.Strategy = EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	OutManifest.Slots = MoveTemp(Slots);
	return true;
}

bool ValidateBindingPackageManifest(
	const FAvidScriptWasmReloadManifest& Manifest,
	bool& OutHasPackedOwnerCapability,
	FAvidScriptWasmReloadManifestLoadResult& OutResult)
{
	OutHasPackedOwnerCapability = false;
	FString PackageManifestJson;
	if (!FFileHelper::LoadFileToString(PackageManifestJson, *Manifest.BindingPackageManifestFile))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_file_read_failed"),
			FString::Printf(
				TEXT("failed to decode binding package manifest: %s"),
				*Manifest.BindingPackageManifestFile),
			TEXT("republish package.json as UTF-8 JSON and rebuild the script"));
		return false;
	}

	TSharedPtr<FJsonObject> PackageObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PackageManifestJson);
	if (!FJsonSerializer::Deserialize(Reader, PackageObject) || !PackageObject.IsValid())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_invalid"),
			TEXT("binding package manifest JSON could not be parsed as an object"),
			TEXT("republish the generated binding package and rebuild the script"));
		return false;
	}

	int32 SchemaVersion = 0;
	int32 DescriptorSchemaVersion = 0;
	FString PackageName;
	FString PackageHash;
	FString DescriptorSha256;
	if (!TryGetInt32Field(*PackageObject, TEXT("schema_version"), SchemaVersion)
		|| SchemaVersion != 1
		|| !TryGetInt32Field(*PackageObject, TEXT("descriptor_schema_version"), DescriptorSchemaVersion)
		|| (DescriptorSchemaVersion != 2
			&& DescriptorSchemaVersion != 3
			&& DescriptorSchemaVersion != 4
			&& DescriptorSchemaVersion != 5
			&& DescriptorSchemaVersion != 6
			&& DescriptorSchemaVersion != 7
			&& DescriptorSchemaVersion != 8
			&& DescriptorSchemaVersion != 9)
		|| !PackageObject->TryGetStringField(TEXT("package_name"), PackageName)
		|| !PackageObject->TryGetStringField(TEXT("package_hash"), PackageHash)
		|| !PackageObject->TryGetStringField(TEXT("descriptor_sha256"), DescriptorSha256))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_invalid"),
			TEXT("binding package manifest schema or identity fields are invalid"),
			TEXT("republish the package with the current Phase 42 binding emitter"));
		return false;
	}
	if (DescriptorSchemaVersion
		!= Manifest.BindingPackage->GetDescriptorSchemaVersion())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_identity_mismatch"),
			TEXT("package.json descriptor_schema_version does not match the descriptor"),
			TEXT("republish the binding package from one descriptor snapshot"));
		return false;
	}
	if (DescriptorSchemaVersion >= 7)
	{
		int32 ObjectFactoryCount = INDEX_NONE;
		if (!TryGetInt32Field(
				*PackageObject,
				TEXT("object_factory_count"),
				ObjectFactoryCount)
			|| ObjectFactoryCount < 0
			|| ObjectFactoryCount
				!= Manifest.BindingPackage->GetObjectFactoryCount())
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("binding_package_invalid"),
				TEXT("package.json object_factory_count does not match descriptor"),
				TEXT("republish the current binding package"));
			return false;
		}
	}
	else if (PackageObject->HasField(TEXT("object_factory_count")))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_invalid"),
			TEXT("package.json object_factory_count requires descriptor schema v7 or newer"),
			TEXT("republish the binding package with matching schema provenance"));
		return false;
	}

	PackageHash = PackageHash.ToLower();
	DescriptorSha256 = DescriptorSha256.ToLower();
	if (PackageName != Manifest.BindingPackageName
		|| PackageHash != Manifest.BindingPackageHash
		|| DescriptorSha256 != Manifest.BindingDescriptorSha256)
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_identity_mismatch"),
			TEXT("package.json identity does not match the script manifest and descriptor"),
			TEXT("rebuild the script and binding package as one transaction"));
		return false;
	}

	const TSharedPtr<FJsonObject>* FilesObjectPtr = nullptr;
	FString DescriptorRelativePath;
	if (!PackageObject->TryGetObjectField(TEXT("files"), FilesObjectPtr)
		|| FilesObjectPtr == nullptr
		|| !FilesObjectPtr->IsValid()
		|| !(*FilesObjectPtr)->TryGetStringField(TEXT("descriptor"), DescriptorRelativePath)
		|| DescriptorRelativePath.IsEmpty()
		|| !FPaths::IsRelative(DescriptorRelativePath))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_invalid"),
			TEXT("package.json files.descriptor must be a package-relative path"),
			TEXT("republish the generated binding package"));
		return false;
	}

	const FString PackageDirectory = NormalizeFullPath(FPaths::GetPath(Manifest.BindingPackageManifestFile));
	FString PackageDirectoryPrefix = PackageDirectory;
	if (!PackageDirectoryPrefix.EndsWith(TEXT("/")))
	{
		PackageDirectoryPrefix += TEXT("/");
	}
	const FString DescriptorFromPackage = NormalizeFullPath(FPaths::Combine(
		PackageDirectory,
		DescriptorRelativePath));
	if (!DescriptorFromPackage.StartsWith(PackageDirectoryPrefix, ESearchCase::IgnoreCase)
		|| !DescriptorFromPackage.Equals(
			NormalizeFullPath(Manifest.BindingDescriptorFile),
			ESearchCase::IgnoreCase))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_path_mismatch"),
			TEXT("package.json descriptor path escapes or differs from the script manifest"),
			TEXT("republish the content-addressed package and rebuild the script"));
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* RequiredImportValues = nullptr;
	if (!PackageObject->TryGetArrayField(TEXT("required_imports"), RequiredImportValues)
		|| RequiredImportValues == nullptr
		|| RequiredImportValues->IsEmpty())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_invalid"),
			TEXT("package.json required_imports must not be empty"),
			TEXT("republish the generated binding package"));
		return false;
	}

	TMap<FString, FAvidScriptVmDynamicImport> DeclaredImports;
	bool bSeenPackedOwner = false;
	for (const TSharedPtr<FJsonValue>& ImportValue : *RequiredImportValues)
	{
		const TSharedPtr<FJsonObject> ImportObject = ImportValue.IsValid()
			? ImportValue->AsObject()
			: nullptr;
		FString StableId;
		int32 Ordinal = INDEX_NONE;
		FString ModuleName;
		FString ImportName;
		FString Signature;
		if (!ImportObject.IsValid()
			|| !ImportObject->TryGetStringField(TEXT("stable_id"), StableId)
			|| !TryGetInt32Field(*ImportObject, TEXT("ordinal"), Ordinal)
			|| !ImportObject->TryGetStringField(TEXT("module"), ModuleName)
			|| !ImportObject->TryGetStringField(TEXT("name"), ImportName)
			|| !ImportObject->TryGetStringField(TEXT("signature"), Signature)
			|| StableId.IsEmpty()
			|| ModuleName.IsEmpty()
			|| ImportName.IsEmpty()
			|| Signature.IsEmpty())
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("binding_package_invalid"),
				TEXT("package.json required_imports contains an invalid import"),
				TEXT("republish the generated binding package"));
			return false;
		}

		const bool bIsPackedOwner =
			StableId == TEXT("avidscript.owner_get_handle.v1")
			&& Ordinal == INDEX_NONE
			&& ModuleName == TEXT("avidscript")
			&& ImportName == TEXT("avid_owner_get_handle")
			&& Signature == TEXT("()I");
		if (bIsPackedOwner)
		{
			if (bSeenPackedOwner)
			{
				SetManifestLoadFailure(
					OutResult,
					TEXT("binding_package_invalid"),
					TEXT("package.json required_imports contains a duplicate packed owner import"),
					TEXT("republish the generated binding package"));
				return false;
			}
			bSeenPackedOwner = true;
			continue;
		}
		if (Ordinal < 0)
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("binding_package_invalid"),
				TEXT("package.json required_imports contains an invalid dynamic import ordinal"),
				TEXT("republish the generated binding package"));
			return false;
		}

		const FString ImportKey = ModuleName + TEXT("\n") + ImportName;
		if (DeclaredImports.Contains(ImportKey))
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("binding_package_invalid"),
				TEXT("package.json required_imports contains a duplicate import"),
				TEXT("republish the generated binding package"));
			return false;
			}
		FAvidScriptVmDynamicImport DeclaredImport;
		DeclaredImport.StableId = MoveTemp(StableId);
		DeclaredImport.Ordinal = static_cast<uint32>(Ordinal);
		DeclaredImport.ModuleName = MoveTemp(ModuleName);
		DeclaredImport.ImportName = MoveTemp(ImportName);
		DeclaredImport.Signature = MoveTemp(Signature);
		DeclaredImports.Add(ImportKey, MoveTemp(DeclaredImport));
	}

	if (bSeenPackedOwner
		&& (DescriptorSchemaVersion < 6
			|| Manifest.BindingPackage->GetExpectedSelfClass() == nullptr))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_import_mismatch"),
			TEXT("package.json packed owner capability requires descriptor schema v6 or newer self_type_id"),
			TEXT("regenerate the binding package from one reflection snapshot"));
		return false;
	}

	const TArray<FAvidScriptVmDynamicImport>& RuntimeImports = Manifest.BindingPackage->GetVmPackage().Imports;
	if (DeclaredImports.Num() != RuntimeImports.Num())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_import_mismatch"),
			TEXT("package.json and descriptor declare different dynamic import counts"),
			TEXT("regenerate the binding package from one reflection snapshot"));
		return false;
	}
	for (const FAvidScriptVmDynamicImport& RuntimeImport : RuntimeImports)
	{
		const FAvidScriptVmDynamicImport* DeclaredImport =
			DeclaredImports.Find(RuntimeImport.ModuleName + TEXT("\n") + RuntimeImport.ImportName);
		if (DeclaredImport == nullptr
			|| DeclaredImport->StableId != RuntimeImport.StableId
			|| DeclaredImport->Ordinal != RuntimeImport.Ordinal
			|| DeclaredImport->Signature != RuntimeImport.Signature)
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("binding_package_import_mismatch"),
				FString::Printf(
					TEXT("descriptor dynamic import is missing or differs from package.json: %s.%s"),
					*RuntimeImport.ModuleName,
					*RuntimeImport.ImportName),
				TEXT("regenerate the binding package from one reflection snapshot"));
			return false;
		}
	}

	OutHasPackedOwnerCapability = bSeenPackedOwner;
	return true;
}

bool LoadManifestDebugMap(
	const FJsonObject& RootObject,
	const FString& ManifestFullPath,
	const FAvidScriptWasmModuleLayout& WasmLayout,
	FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadManifestLoadResult& OutResult)
{
	if (!RootObject.HasField(TEXT("debug_map")))
	{
		return true;
	}

	const TSharedPtr<FJsonObject>* DebugMapObjectPtr = nullptr;
	if (!RootObject.TryGetObjectField(TEXT("debug_map"), DebugMapObjectPtr)
		|| DebugMapObjectPtr == nullptr
		|| !DebugMapObjectPtr->IsValid())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("debug_map_manifest_invalid"),
			TEXT("debug_map must be an object"),
			TEXT("rebuild the C# script manifest and debug artifacts as one transaction"));
		return false;
	}

	const FJsonObject& DebugMapObject = *DebugMapObjectPtr->Get();
	FString DebugMapPathFromManifest;
	FString DebugVersion;
	FString DebugModuleId;
	int32 DebugSchemaVersion = 0;
	int32 DebugImportedFunctionCount = 0;
	int32 DebugDefinedFunctionCount = 0;
	if (!DebugMapObject.TryGetStringField(TEXT("file"), DebugMapPathFromManifest)
		|| DebugMapPathFromManifest.IsEmpty()
		|| !DebugMapObject.TryGetStringField(TEXT("sha256"), Manifest.DebugMapSha256)
		|| !DebugMapObject.TryGetStringField(TEXT("version"), DebugVersion)
		|| !DebugMapObject.TryGetStringField(TEXT("module_id"), DebugModuleId)
		|| !TryGetInt32Field(DebugMapObject, TEXT("schema_version"), DebugSchemaVersion)
		|| !TryGetInt32Field(DebugMapObject, TEXT("imported_function_count"), DebugImportedFunctionCount)
		|| !TryGetInt32Field(DebugMapObject, TEXT("defined_function_count"), DebugDefinedFunctionCount)
		|| DebugSchemaVersion != 1
		|| DebugVersion != TEXT("1.0")
		|| DebugImportedFunctionCount < 0
		|| DebugDefinedFunctionCount <= 0
		|| DebugDefinedFunctionCount > 65536
		|| !IsLowercaseSha256(Manifest.DebugMapSha256))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("debug_map_manifest_invalid"),
			TEXT("debug_map file/hash/schema/version/module/function-range fields are invalid"),
			TEXT("rebuild the C# script manifest with the current debug map schema"));
		return false;
	}

	if (DebugImportedFunctionCount != static_cast<int32>(WasmLayout.ImportedFunctionCount)
		|| DebugDefinedFunctionCount != static_cast<int32>(WasmLayout.DefinedFunctionCount)
		|| Manifest.RequiredImports.Num() != static_cast<int32>(WasmLayout.ImportedFunctionCount))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("debug_map_wasm_layout_mismatch"),
			FString::Printf(
				TEXT("manifest_imports=%d debug_imports=%d wasm_imports=%u debug_defined=%d wasm_defined=%u"),
				Manifest.RequiredImports.Num(),
				DebugImportedFunctionCount,
				WasmLayout.ImportedFunctionCount,
				DebugDefinedFunctionCount,
				WasmLayout.DefinedFunctionCount),
			TEXT("republish manifest, WASM, Guest IR, and debug map as one transaction"));
		return false;
	}
	if (!TryResolveDebugMapPathFromManifest(ManifestFullPath, DebugMapPathFromManifest, Manifest.DebugMapFile))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("debug_map_path_invalid"),
			DebugMapPathFromManifest,
			TEXT("publish the debug map under the project or beside its manifest using a canonical relative path"));
		return false;
	}
	OutResult.DebugMapPath = Manifest.DebugMapFile;

	const TSharedPtr<FJsonObject>* SourceObjectPtr = nullptr;
	const TSharedPtr<FJsonObject>* GuestIrObjectPtr = nullptr;
	if (!RootObject.TryGetObjectField(TEXT("source"), SourceObjectPtr)
		|| SourceObjectPtr == nullptr
		|| !SourceObjectPtr->IsValid()
		|| !RootObject.TryGetObjectField(TEXT("guest_ir"), GuestIrObjectPtr)
		|| GuestIrObjectPtr == nullptr
		|| !GuestIrObjectPtr->IsValid())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("debug_map_manifest_invalid"),
			TEXT("debug_map requires source and guest_ir manifest provenance"),
			TEXT("rebuild the C# script manifest and all intermediate artifacts"));
		return false;
	}

	const FJsonObject& SourceObject = *SourceObjectPtr->Get();
	const FJsonObject& GuestIrObject = *GuestIrObjectPtr->Get();
	FString FrontendArtifactSha256;
	if (!SourceObject.TryGetStringField(TEXT("file"), Manifest.DebugProvenance.SourceFile)
		|| !SourceObject.TryGetStringField(TEXT("sha256"), Manifest.DebugProvenance.SourceSha256)
		|| !SourceObject.TryGetStringField(TEXT("frontend_sha256"), FrontendArtifactSha256)
		|| !SourceObject.TryGetStringField(TEXT("semantic_sha256"), Manifest.DebugProvenance.SemanticSha256)
		|| !GuestIrObject.TryGetStringField(TEXT("module_id"), Manifest.DebugProvenance.GuestModuleId)
		|| !GuestIrObject.TryGetStringField(TEXT("sha256"), Manifest.DebugProvenance.GuestIrSha256)
		|| !IsLowercaseSha256(Manifest.DebugProvenance.SourceSha256)
		|| !IsLowercaseSha256(FrontendArtifactSha256)
		|| !IsLowercaseSha256(Manifest.DebugProvenance.SemanticSha256)
		|| !IsLowercaseSha256(Manifest.DebugProvenance.GuestIrSha256))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("debug_map_manifest_invalid"),
			TEXT("source or guest_ir provenance is incomplete or not lowercase SHA-256"),
			TEXT("republish frontend, semantic, Guest IR, debug map, and manifest together"));
		return false;
	}

	Manifest.DebugProvenance.FrontendArtifactSha256 = FrontendArtifactSha256;
	Manifest.DebugProvenance.ImportedFunctionCount = static_cast<uint32>(DebugImportedFunctionCount);
	Manifest.DebugProvenance.DefinedFunctionCount = static_cast<uint32>(DebugDefinedFunctionCount);
	if (DebugModuleId != Manifest.DebugProvenance.GuestModuleId)
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("debug_map_module_mismatch"),
			DebugModuleId,
			TEXT("rebuild Guest IR and debug map from the same semantic artifact"));
		return false;
	}

	FString DebugMapErrorCategory;
	FString DebugMapErrorSource;
	if (!FAvidScriptWasmDebugMap::LoadAndValidate(
		Manifest.DebugMapFile,
		Manifest.DebugMapSha256,
		Manifest.DebugProvenance,
		MakeArrayView(WasmLayout.FunctionExports),
		Manifest.DebugMap,
		DebugMapErrorCategory,
		DebugMapErrorSource))
	{
		SetManifestLoadFailure(
			OutResult,
			DebugMapErrorCategory.IsEmpty() ? FString(TEXT("debug_map_invalid")) : DebugMapErrorCategory,
			DebugMapErrorSource,
			TEXT("rebuild and republish the C# debug map with its matching manifest provenance"));
		return false;
	}
	return true;
}
} // namespace

bool ValidateAvidScriptWasmImportContract(
	const FAvidScriptWasmModuleLayout& WasmLayout,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmImportContractResult& OutResult)
{
	OutResult = FAvidScriptWasmImportContractResult();
	TArray<FAvidScriptVmExpectedImport> ExpectedImports;
	ExpectedImports.Reserve(Manifest.RequiredImports.Num());
	for (const FAvidScriptWasmRequiredImport& RequiredImport : Manifest.RequiredImports)
	{
		ExpectedImports.Add({
			RequiredImport.ModuleName,
			RequiredImport.ImportName
		});
	}
	const FAvidScriptVmBindingPackage* VmBindingPackage =
		Manifest.BindingPackage.IsValid()
			? &Manifest.BindingPackage->GetVmPackage()
			: nullptr;
	FAvidScriptVmError VmError;
	if (ValidateAvidScriptVmImportContract(
		WasmLayout,
		VmBindingPackage,
		ExpectedImports,
		true,
		VmError))
	{
		return true;
	}

	OutResult.ErrorCategory = VmError.Category;
	OutResult.ErrorDetails = VmError.Details;
	if (VmError.Category == TEXT("manifest_wasm_import_mismatch"))
	{
		OutResult.NextAction = TEXT("rebuild the script manifest from the actual WASM import section");
	}
	else if (VmError.Category == TEXT("binding_package_missing"))
	{
		OutResult.NextAction = TEXT("rebuild the script and binding package as one transaction");
	}
	else if (VmError.Category == TEXT("binding_package_import_mismatch"))
	{
		OutResult.NextAction = TEXT("rebuild the script from the current binding package");
	}
	else
	{
		OutResult.NextAction = TEXT("rebuild and republish the current binding package");
	}
	return false;
}

bool InspectAndValidateAvidScriptWasmImportContract(
	TConstArrayView<uint8> Bytecode,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmImportContractResult& OutResult)
{
	FAvidScriptWasmModuleLayout WasmLayout;
	FString WasmLayoutError;
	if (!InspectAvidScriptWasmModuleLayout(Bytecode, WasmLayout, WasmLayoutError))
	{
		OutResult = FAvidScriptWasmImportContractResult();
		OutResult.ErrorCategory = TEXT("wasm_layout_invalid");
		OutResult.ErrorDetails = MoveTemp(WasmLayoutError);
		OutResult.NextAction = TEXT("rebuild the WASM module with the supported backend");
		return false;
	}
	return ValidateAvidScriptWasmImportContract(WasmLayout, Manifest, OutResult);
}

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
	if (!TryGetInt32Field(*RootObject, TEXT("schema_version"), SchemaVersion) ||
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

	if (!TryGetInt32Field(*RootObject, TEXT("abi_version"), Manifest.AbiVersion))
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

	if (RootObject->HasField(TEXT("state_migration")))
	{
		TSharedPtr<FJsonObject> StateMigrationObject;
		if (!RequireJsonObjectField(*RootObject, TEXT("state_migration"), StateMigrationObject, OutResult)
			|| !LoadStateMigrationManifest(*StateMigrationObject, Manifest.StateMigration, OutResult))
		{
			return false;
		}
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
	Manifest.WasmFile = ResolveArtifactPathFromManifest(ManifestFullPath, WasmPathFromManifest);
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

	FAvidScriptWasmModuleLayout WasmLayout;
	FString WasmLayoutError;
	if (!InspectAvidScriptWasmModuleLayout(Bytecode, WasmLayout, WasmLayoutError))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("wasm_layout_invalid"),
			WasmLayoutError,
			TEXT("rebuild the WASM module with the supported backend"));
		return false;
	}

	bool bBindingPackageHasPackedOwnerCapability = false;
	const TSharedPtr<FJsonObject>* BindingPackageObjectPtr = nullptr;
	if (RootObject->TryGetObjectField(TEXT("binding_package"), BindingPackageObjectPtr)
		&& BindingPackageObjectPtr != nullptr
		&& BindingPackageObjectPtr->IsValid())
	{
		const TSharedPtr<FJsonObject> BindingPackageObject = *BindingPackageObjectPtr;
		FString BindingPackageManifestPathFromManifest;
		FString BindingDescriptorPathFromManifest;
		if (!RequireStringField(*BindingPackageObject, TEXT("package_name"), Manifest.BindingPackageName, OutResult)
			|| !RequireStringField(*BindingPackageObject, TEXT("package_hash"), Manifest.BindingPackageHash, OutResult)
			|| !RequireStringField(*BindingPackageObject, TEXT("manifest_file"), BindingPackageManifestPathFromManifest, OutResult)
			|| !RequireStringField(*BindingPackageObject, TEXT("manifest_sha256"), Manifest.BindingPackageManifestSha256, OutResult)
			|| !RequireStringField(*BindingPackageObject, TEXT("descriptor_file"), BindingDescriptorPathFromManifest, OutResult)
			|| !RequireStringField(*BindingPackageObject, TEXT("descriptor_sha256"), Manifest.BindingDescriptorSha256, OutResult))
		{
			return false;
		}

		Manifest.BindingPackageManifestSha256 = Manifest.BindingPackageManifestSha256.ToLower();
		Manifest.BindingDescriptorSha256 = Manifest.BindingDescriptorSha256.ToLower();
		Manifest.BindingPackageHash = Manifest.BindingPackageHash.ToLower();
		Manifest.BindingPackageManifestFile = ResolveArtifactPathFromManifest(
			ManifestFullPath,
			BindingPackageManifestPathFromManifest);
		Manifest.BindingDescriptorFile = ResolveArtifactPathFromManifest(
			ManifestFullPath,
			BindingDescriptorPathFromManifest);
		OutResult.BindingPackageManifestPath = Manifest.BindingPackageManifestFile;
		OutResult.BindingDescriptorPath = Manifest.BindingDescriptorFile;

		TArray<uint8> BindingManifestBytes;
		if (!LoadAndVerifyBindingArtifact(
			Manifest.BindingPackageManifestFile,
			Manifest.BindingPackageManifestSha256,
			TEXT("binding package manifest"),
			BindingManifestBytes,
			OutResult))
		{
			return false;
		}

		TArray<uint8> BindingDescriptorBytes;
		if (!LoadAndVerifyBindingArtifact(
			Manifest.BindingDescriptorFile,
			Manifest.BindingDescriptorSha256,
			TEXT("binding descriptor"),
			BindingDescriptorBytes,
			OutResult))
		{
			return false;
		}

		FString BindingDescriptorJson;
		if (!FFileHelper::LoadFileToString(BindingDescriptorJson, *Manifest.BindingDescriptorFile))
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("binding_package_file_read_failed"),
				FString::Printf(TEXT("failed to decode binding descriptor: %s"), *Manifest.BindingDescriptorFile),
				TEXT("republish the descriptor as UTF-8 JSON and rebuild the script"));
			return false;
		}

		FAvidScriptBindingPackageLoadResult BindingLoadResult;
		if (!FAvidScriptBindingPackage::LoadDescriptor(
			BindingDescriptorJson,
			Manifest.BindingPackage,
			BindingLoadResult))
		{
			SetManifestLoadFailure(
				OutResult,
				BindingLoadResult.ErrorCategory.IsEmpty()
					? FString(TEXT("binding_package_invalid"))
					: BindingLoadResult.ErrorCategory,
				BindingLoadResult.ErrorDetails.IsEmpty()
					? BindingLoadResult.ErrorSource
					: BindingLoadResult.ErrorDetails,
				TEXT("regenerate the binding package from the current UE reflection snapshot"));
			return false;
		}
		if (!Manifest.BindingPackage.IsValid()
			|| Manifest.BindingPackage->GetPackageName() != Manifest.BindingPackageName
			|| Manifest.BindingPackage->GetPackageHash() != Manifest.BindingPackageHash)
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("binding_package_identity_mismatch"),
				TEXT("script manifest package identity does not match the loaded descriptor"),
				TEXT("rebuild the script and binding package as one transaction"));
			return false;
		}
		if (!ValidateBindingPackageManifest(
				Manifest,
				bBindingPackageHasPackedOwnerCapability,
				OutResult))
		{
			return false;
		}
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
		RequiredImportValues == nullptr)
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("manifest_invalid"),
			TEXT("required_imports must be an array"),
			TEXT("declare the complete host import array before activating this module"));
		return false;
	}

	bool bRequiresPackedOwnerCapability = false;
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

		const bool bIsPackedOwner =
			RequiredImport.ModuleName == TEXT("avidscript")
			&& RequiredImport.ImportName == TEXT("avid_owner_get_handle");
		if (bIsPackedOwner && bRequiresPackedOwnerCapability)
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("manifest_invalid"),
				TEXT("required_imports contains a duplicate packed owner import"),
				TEXT("rebuild the script manifest from the actual WASM import table"));
			return false;
		}
		bRequiresPackedOwnerCapability |= bIsPackedOwner;
		Manifest.RequiredImports.Add(MoveTemp(RequiredImport));
	}

	FAvidScriptWasmImportContractResult ImportContractResult;
	if (!ValidateAvidScriptWasmImportContract(WasmLayout, Manifest, ImportContractResult))
	{
		SetManifestLoadFailure(
			OutResult,
			ImportContractResult.ErrorCategory,
			ImportContractResult.ErrorDetails,
			ImportContractResult.NextAction);
		return false;
	}

	if (bRequiresPackedOwnerCapability && !Manifest.BindingPackage.IsValid())
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_missing"),
			TEXT("packed owner import requires a verified schema v6 or newer binding package"),
			TEXT("rebuild the script and binding package as one transaction"));
		return false;
	}
	if (bRequiresPackedOwnerCapability
		&& (!bBindingPackageHasPackedOwnerCapability
			|| Manifest.BindingPackage->GetExpectedSelfClass() == nullptr))
	{
		SetManifestLoadFailure(
			OutResult,
			TEXT("binding_package_import_mismatch"),
			TEXT("script required_imports claims packed owner capability that the package does not authorize"),
			TEXT("rebuild the script and binding package as one transaction"));
		return false;
	}

	if (!LoadManifestDebugMap(*RootObject, ManifestFullPath, WasmLayout, Manifest, OutResult))
	{
		return false;
	}

	OutManifest = MoveTemp(Manifest);
	OutBytecode = MoveTemp(Bytecode);
	OutResult.bSucceeded = true;
	OutResult.ByteSize = OutBytecode.Num();
	return true;
}
