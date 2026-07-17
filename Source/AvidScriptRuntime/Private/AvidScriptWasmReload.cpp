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

bool ValidateBindingPackageManifest(
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmReloadManifestLoadResult& OutResult)
{
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
	if (!PackageObject->TryGetNumberField(TEXT("schema_version"), SchemaVersion)
		|| SchemaVersion != 1
		|| !PackageObject->TryGetNumberField(TEXT("descriptor_schema_version"), DescriptorSchemaVersion)
		|| DescriptorSchemaVersion != 2
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

	TSet<FString> DeclaredImports;
	for (const TSharedPtr<FJsonValue>& ImportValue : *RequiredImportValues)
	{
		const TSharedPtr<FJsonObject> ImportObject = ImportValue.IsValid()
			? ImportValue->AsObject()
			: nullptr;
		FString ModuleName;
		FString ImportName;
		if (!ImportObject.IsValid()
			|| !ImportObject->TryGetStringField(TEXT("module"), ModuleName)
			|| !ImportObject->TryGetStringField(TEXT("name"), ImportName)
			|| ModuleName.IsEmpty()
			|| ImportName.IsEmpty())
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("binding_package_invalid"),
				TEXT("package.json required_imports contains an invalid import"),
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
		DeclaredImports.Add(ImportKey);
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
		if (!DeclaredImports.Contains(RuntimeImport.ModuleName + TEXT("\n") + RuntimeImport.ImportName))
		{
			SetManifestLoadFailure(
				OutResult,
				TEXT("binding_package_import_mismatch"),
				FString::Printf(
					TEXT("descriptor dynamic import is missing from package.json: %s.%s"),
					*RuntimeImport.ModuleName,
					*RuntimeImport.ImportName),
				TEXT("regenerate the binding package from one reflection snapshot"));
			return false;
		}
	}

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
		if (!ValidateBindingPackageManifest(Manifest, OutResult))
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
