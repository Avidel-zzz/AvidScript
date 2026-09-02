#include "Packages/AvidScriptModulePackageSchema.h"

#include "AvidScriptHash.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace AvidScript::ModulePackage
{
namespace
{
bool HasExactFields(
	const FJsonObject& Object,
	std::initializer_list<const TCHAR*> Required,
	std::initializer_list<const TCHAR*> Optional = {})
{
	TSet<FString> Allowed;
	for (const TCHAR* Field : Required)
	{
		Allowed.Add(Field);
		if (!Object.HasField(Field))
		{
			return false;
		}
	}
	for (const TCHAR* Field : Optional)
	{
		Allowed.Add(Field);
	}
	if (Object.Values.Num() != Allowed.Num()
		&& Object.Values.Num() != static_cast<int32>(Required.size()))
	{
		return false;
	}
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Object.Values)
	{
		if (!Allowed.Contains(Pair.Key))
		{
			return false;
		}
	}
	return true;
}

bool TryGetExactInteger(
	const FJsonObject& Object,
	const TCHAR* FieldName,
	int32& OutValue)
{
	double Number = 0.0;
	if (!Object.TryGetNumberField(FieldName, Number)
		|| !FMath::IsFinite(Number)
		|| Number < static_cast<double>(MIN_int32)
		|| Number > static_cast<double>(MAX_int32))
	{
		return false;
	}
	const int32 Value = static_cast<int32>(Number);
	if (Number != static_cast<double>(Value))
	{
		return false;
	}
	OutValue = Value;
	return true;
}

bool ReadArtifactEntry(
	const TSharedPtr<FJsonObject>& Artifacts,
	const TCHAR* Name,
	const TCHAR* ExpectedFile,
	FArtifactEntry& OutEntry)
{
	const TSharedPtr<FJsonObject>* Entry = nullptr;
	return Artifacts.IsValid()
		&& Artifacts->TryGetObjectField(Name, Entry)
		&& Entry != nullptr
		&& Entry->IsValid()
		&& HasExactFields(*Entry->Get(), { TEXT("file"), TEXT("sha256") })
		&& (*Entry)->TryGetStringField(TEXT("file"), OutEntry.File)
		&& OutEntry.File == ExpectedFile
		&& (*Entry)->TryGetStringField(TEXT("sha256"), OutEntry.Sha256)
		&& IsLowercaseSha256(OutEntry.Sha256);
}

bool LoadJsonObject(
	const FString& Path,
	TSharedPtr<FJsonObject>& OutObject)
{
	FString Json;
	return FFileHelper::LoadFileToString(Json, *Path)
		&& FJsonSerializer::Deserialize(
			TJsonReaderFactory<>::Create(Json),
			OutObject)
		&& OutObject.IsValid();
}
} // namespace

bool IsLowercaseSha256(const FString& Value)
{
	if (Value.Len() != 64)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsDigit(Character)
			&& (Character < TEXT('a') || Character > TEXT('f')))
		{
			return false;
		}
	}
	return true;
}

bool IsNormalizedModuleId(const FString& Value)
{
	if (Value.IsEmpty() || Value.Len() > 64
		|| Value[0] < TEXT('a') || Value[0] > TEXT('z'))
	{
		return false;
	}
	for (int32 Index = 1; Index < Value.Len(); ++Index)
	{
		const TCHAR Character = Value[Index];
		if ((Character < TEXT('a') || Character > TEXT('z'))
			&& !FChar::IsDigit(Character)
			&& Character != TEXT('_')
			&& Character != TEXT('-')
			&& Character != TEXT('.'))
		{
			return false;
		}
	}
	return true;
}

bool TryParseSimpleSemanticVersion(
	const FString& Value,
	TStaticArray<int32, 3>& OutParts)
{
	TArray<FString> Parts;
	Value.ParseIntoArray(Parts, TEXT("."), false);
	if (Parts.Num() != 3)
	{
		return false;
	}
	for (int32 Index = 0; Index < Parts.Num(); ++Index)
	{
		const FString& Part = Parts[Index];
		if (Part.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Character : Part)
		{
			if (!FChar::IsDigit(Character))
			{
				return false;
			}
		}
		if (!LexTryParseString(OutParts[Index], *Part)
			|| OutParts[Index] < 0)
		{
			return false;
		}
	}
	return true;
}

bool ParseCatalogEntry(
	const TSharedPtr<FJsonObject>& Object,
	FCatalogEntry& OutEntry)
{
	return Object.IsValid()
		&& HasExactFields(
			*Object,
			{
				TEXT("module_id"),
				TEXT("package_id"),
				TEXT("descriptor_file"),
				TEXT("descriptor_sha256"),
				TEXT("platform"),
				TEXT("configuration")
			})
		&& Object->TryGetStringField(TEXT("module_id"), OutEntry.ModuleId)
		&& IsNormalizedModuleId(OutEntry.ModuleId)
		&& Object->TryGetStringField(TEXT("package_id"), OutEntry.PackageId)
		&& IsLowercaseSha256(OutEntry.PackageId)
		&& Object->TryGetStringField(TEXT("descriptor_file"), OutEntry.DescriptorFile)
		&& OutEntry.DescriptorFile
			== FString::Printf(
				TEXT("%s/%s/package.json"),
				*OutEntry.ModuleId,
				*OutEntry.PackageId)
		&& Object->TryGetStringField(
			TEXT("descriptor_sha256"),
			OutEntry.DescriptorSha256)
		&& IsLowercaseSha256(OutEntry.DescriptorSha256)
		&& Object->TryGetStringField(TEXT("platform"), OutEntry.Platform)
		&& OutEntry.Platform == TEXT("win64")
		&& Object->TryGetStringField(TEXT("configuration"), OutEntry.Configuration)
		&& (OutEntry.Configuration == TEXT("development")
			|| OutEntry.Configuration == TEXT("shipping"));
}

FString MakeIdentityPayload(const FDocument& Package)
{
	const TArray<FString> Fields{
		FString::FromInt(PackageSchemaVersion),
		Package.ModuleId,
		FString::FromInt(Package.AbiVersion),
		Package.Platform,
		Package.Configuration,
		Package.MinimumRuntimeVersion,
		Package.Backend,
		Package.Format,
		Package.Policy,
		Package.CompilerBuildIdentity,
		Package.TargetTriple,
		Package.CpuFeatures,
		Package.RuntimeManifest.Sha256,
		Package.CanonicalWasm.Sha256,
		Package.Precompiled.Sha256,
		Package.BindingManifest.Sha256,
		Package.BindingDescriptor.Sha256,
		Package.DebugMap.IsSet() ? Package.DebugMap->Sha256 : FString()
	};
	return FString::Join(Fields, TEXT("\n"));
}

bool ParseDocument(
	const TSharedPtr<FJsonObject>& Root,
	FDocument& OutPackage)
{
	if (!Root.IsValid()
		|| !HasExactFields(
			*Root,
			{
				TEXT("schema_version"),
				TEXT("package_id"),
				TEXT("module_id"),
				TEXT("abi_version"),
				TEXT("platform"),
				TEXT("configuration"),
				TEXT("minimum_runtime_version"),
				TEXT("execution"),
				TEXT("artifacts")
			}))
	{
		return false;
	}
	int32 SchemaVersion = 0;
	TStaticArray<int32, 3> VersionParts;
	const TSharedPtr<FJsonObject>* Execution = nullptr;
	const TSharedPtr<FJsonObject>* Artifacts = nullptr;
	if (!TryGetExactInteger(*Root, TEXT("schema_version"), SchemaVersion)
		|| SchemaVersion != PackageSchemaVersion
		|| !Root->TryGetStringField(TEXT("package_id"), OutPackage.PackageId)
		|| !IsLowercaseSha256(OutPackage.PackageId)
		|| !Root->TryGetStringField(TEXT("module_id"), OutPackage.ModuleId)
		|| !IsNormalizedModuleId(OutPackage.ModuleId)
		|| !TryGetExactInteger(*Root, TEXT("abi_version"), OutPackage.AbiVersion)
		|| OutPackage.AbiVersion <= 0
		|| !Root->TryGetStringField(TEXT("platform"), OutPackage.Platform)
		|| OutPackage.Platform != TEXT("win64")
		|| !Root->TryGetStringField(TEXT("configuration"), OutPackage.Configuration)
		|| (OutPackage.Configuration != TEXT("development")
			&& OutPackage.Configuration != TEXT("shipping"))
		|| !Root->TryGetStringField(
			TEXT("minimum_runtime_version"),
			OutPackage.MinimumRuntimeVersion)
		|| !TryParseSimpleSemanticVersion(
			OutPackage.MinimumRuntimeVersion,
			VersionParts)
		|| !Root->TryGetObjectField(TEXT("execution"), Execution)
		|| Execution == nullptr
		|| !Execution->IsValid()
		|| !HasExactFields(
			*Execution->Get(),
			{
				TEXT("backend"),
				TEXT("format"),
				TEXT("policy"),
				TEXT("compiler_build_identity"),
				TEXT("target_triple"),
				TEXT("cpu_features")
			})
		|| !(*Execution)->TryGetStringField(TEXT("backend"), OutPackage.Backend)
		|| OutPackage.Backend != TEXT("wasmtime")
		|| !(*Execution)->TryGetStringField(TEXT("format"), OutPackage.Format)
		|| OutPackage.Format != TEXT("wasmtime_serialized_v1")
		|| !(*Execution)->TryGetStringField(TEXT("policy"), OutPackage.Policy)
		|| (OutPackage.Policy != TEXT("require_precompiled")
			&& OutPackage.Policy != TEXT("prefer_precompiled"))
		|| (OutPackage.Configuration == TEXT("shipping")
			&& OutPackage.Policy != TEXT("require_precompiled"))
		|| !(*Execution)->TryGetStringField(
			TEXT("compiler_build_identity"),
			OutPackage.CompilerBuildIdentity)
		|| OutPackage.CompilerBuildIdentity.IsEmpty()
		|| !(*Execution)->TryGetStringField(
			TEXT("target_triple"),
			OutPackage.TargetTriple)
		|| OutPackage.TargetTriple != TEXT("x86_64-pc-windows-msvc")
		|| !(*Execution)->TryGetStringField(
			TEXT("cpu_features"),
			OutPackage.CpuFeatures)
		|| OutPackage.CpuFeatures.IsEmpty()
		|| !Root->TryGetObjectField(TEXT("artifacts"), Artifacts)
		|| Artifacts == nullptr
		|| !Artifacts->IsValid()
		|| !HasExactFields(
			*Artifacts->Get(),
			{
				TEXT("runtime_manifest"),
				TEXT("canonical_wasm"),
				TEXT("precompiled"),
				TEXT("binding_manifest"),
				TEXT("binding_descriptor")
			},
			{ TEXT("debug_map") })
		|| !ReadArtifactEntry(
			*Artifacts,
			TEXT("runtime_manifest"),
			TEXT("runtime.avidscript.json"),
			OutPackage.RuntimeManifest)
		|| !ReadArtifactEntry(
			*Artifacts,
			TEXT("canonical_wasm"),
			TEXT("module.wasm"),
			OutPackage.CanonicalWasm)
		|| !ReadArtifactEntry(
			*Artifacts,
			TEXT("precompiled"),
			TEXT("module.wasmtime.cwasm"),
			OutPackage.Precompiled)
		|| !ReadArtifactEntry(
			*Artifacts,
			TEXT("binding_manifest"),
			TEXT("bindings/package.json"),
			OutPackage.BindingManifest)
		|| !ReadArtifactEntry(
			*Artifacts,
			TEXT("binding_descriptor"),
			TEXT("bindings/bindings.json"),
			OutPackage.BindingDescriptor))
	{
		return false;
	}
	if ((*Artifacts)->HasField(TEXT("debug_map")))
	{
		if (OutPackage.Configuration == TEXT("shipping"))
		{
			return false;
		}
		FArtifactEntry DebugMap;
		if (!ReadArtifactEntry(
				*Artifacts,
				TEXT("debug_map"),
				TEXT("diagnostics/debug-map.json"),
				DebugMap))
		{
			return false;
		}
		OutPackage.DebugMap = MoveTemp(DebugMap);
	}
	return OutPackage.PackageId
		== FAvidScriptHash::Sha256HexUtf8(MakeIdentityPayload(OutPackage));
}

bool ValidateRuntimeManifest(
	const FString& RuntimeManifestPath,
	const FDocument& Package)
{
	TSharedPtr<FJsonObject> Root;
	if (!LoadJsonObject(RuntimeManifestPath, Root))
	{
		return false;
	}
	FString ModuleId;
	int32 AbiVersion = 0;
	const TSharedPtr<FJsonObject>* Wasm = nullptr;
	const TSharedPtr<FJsonObject>* Execution = nullptr;
	const TSharedPtr<FJsonObject>* BindingPackage = nullptr;
	FString WasmFile;
	FString WasmSha256;
	FString ExecutionFile;
	FString ExecutionSha256;
	FString ExecutionFormat;
	FString ExecutionPolicy;
	FString CompilerBuildIdentity;
	FString TargetTriple;
	FString BindingManifestFile;
	FString BindingManifestSha256;
	FString BindingDescriptorFile;
	FString BindingDescriptorSha256;
	if (!Root->TryGetStringField(TEXT("module_id"), ModuleId)
		|| ModuleId != Package.ModuleId
		|| !TryGetExactInteger(*Root, TEXT("abi_version"), AbiVersion)
		|| AbiVersion != Package.AbiVersion
		|| !Root->TryGetObjectField(TEXT("wasm"), Wasm)
		|| Wasm == nullptr
		|| !Wasm->IsValid()
		|| !(*Wasm)->TryGetStringField(TEXT("file"), WasmFile)
		|| WasmFile != Package.CanonicalWasm.File
		|| !(*Wasm)->TryGetStringField(TEXT("sha256"), WasmSha256)
		|| WasmSha256 != Package.CanonicalWasm.Sha256
		|| !Root->TryGetObjectField(TEXT("execution"), Execution)
		|| Execution == nullptr
		|| !Execution->IsValid()
		|| !(*Execution)->TryGetStringField(TEXT("file"), ExecutionFile)
		|| ExecutionFile != Package.Precompiled.File
		|| !(*Execution)->TryGetStringField(TEXT("sha256"), ExecutionSha256)
		|| ExecutionSha256 != Package.Precompiled.Sha256
		|| !(*Execution)->TryGetStringField(TEXT("format"), ExecutionFormat)
		|| ExecutionFormat != Package.Format
		|| !(*Execution)->TryGetStringField(TEXT("policy"), ExecutionPolicy)
		|| ExecutionPolicy != Package.Policy
		|| !(*Execution)->TryGetStringField(
			TEXT("compiler_build_identity"),
			CompilerBuildIdentity)
		|| CompilerBuildIdentity != Package.CompilerBuildIdentity
		|| !(*Execution)->TryGetStringField(TEXT("target_triple"), TargetTriple)
		|| TargetTriple != Package.TargetTriple
		|| !Root->TryGetObjectField(TEXT("binding_package"), BindingPackage)
		|| BindingPackage == nullptr
		|| !BindingPackage->IsValid()
		|| !(*BindingPackage)->TryGetStringField(
			TEXT("manifest_file"),
			BindingManifestFile)
		|| BindingManifestFile != Package.BindingManifest.File
		|| !(*BindingPackage)->TryGetStringField(
			TEXT("manifest_sha256"),
			BindingManifestSha256)
		|| BindingManifestSha256 != Package.BindingManifest.Sha256
		|| !(*BindingPackage)->TryGetStringField(
			TEXT("descriptor_file"),
			BindingDescriptorFile)
		|| BindingDescriptorFile != Package.BindingDescriptor.File
		|| !(*BindingPackage)->TryGetStringField(
			TEXT("descriptor_sha256"),
			BindingDescriptorSha256)
		|| BindingDescriptorSha256 != Package.BindingDescriptor.Sha256)
	{
		return false;
	}
	if (Package.Configuration == TEXT("shipping"))
	{
		return !Root->HasField(TEXT("source"))
			&& !Root->HasField(TEXT("guest_ir"))
			&& !Root->HasField(TEXT("debug_map"));
	}
	if (Package.DebugMap.IsSet())
	{
		const TSharedPtr<FJsonObject>* DebugMap = nullptr;
		FString File;
		FString Sha256;
		return Root->TryGetObjectField(TEXT("debug_map"), DebugMap)
			&& DebugMap != nullptr
			&& DebugMap->IsValid()
			&& (*DebugMap)->TryGetStringField(TEXT("file"), File)
			&& File == Package.DebugMap->File
			&& (*DebugMap)->TryGetStringField(TEXT("sha256"), Sha256)
			&& Sha256 == Package.DebugMap->Sha256;
	}
	return !Root->HasField(TEXT("debug_map"));
}
}
