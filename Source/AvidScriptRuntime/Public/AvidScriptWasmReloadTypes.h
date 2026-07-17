#pragma once

#include "AvidScriptWasmRuntime.h"

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmRequiredImport
{
	FString ModuleName;
	FString ImportName;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadManifest
{
	static constexpr int32 SupportedSchemaVersion = 1;
	static constexpr int32 SupportedAbiVersion = 1;

	FString ModuleId;
	int32 AbiVersion = SupportedAbiVersion;
	FString Language;
	FString WasmFile;
	FString WasmSha256;
	TArray<FString> RequiredExports;
	TArray<FAvidScriptWasmRequiredImport> RequiredImports;
	FString BindingPackageName;
	FString BindingPackageHash;
	FString BindingPackageManifestFile;
	FString BindingPackageManifestSha256;
	FString BindingDescriptorFile;
	FString BindingDescriptorSha256;
	TSharedPtr<const FAvidScriptBindingPackage> BindingPackage;

	static FAvidScriptWasmReloadManifest MakeSmoke(const FString& InModuleId);
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadManifestLoadResult
{
	bool bSucceeded = false;
	FString ManifestPath;
	FString ModulePath;
	FString BindingPackageManifestPath;
	FString BindingDescriptorPath;
	int64 ByteSize = 0;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
};

class AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadManifestLoader
{
public:
	static bool LoadFromFile(
		const FString& ManifestPath,
		FAvidScriptWasmReloadManifest& OutManifest,
		TArray<uint8>& OutBytecode,
		FAvidScriptWasmReloadManifestLoadResult& OutResult);
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmReloadResult
{
	bool bSucceeded = false;
	bool bReloadApplied = false;
	bool bRollbackPreservedLiveRuntime = false;
	FString PreviousModuleId;
	FString CandidateModuleId;
	FString ActiveModuleId;
	FString ExportName;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
	FAvidScriptWasmSmokeResult RuntimeResult;
};
