#pragma once

#include "AvidScriptWasmRuntime.h"

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmRequiredImport
{
	FString ModuleName;
	FString ImportName;
};

enum class EAvidScriptWasmStateMigrationStrategy : uint8
{
	None,
	HostSnapshot
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmStateSlot
{
	FString StableId;
	TArray<FString> Aliases;
	FString TypeFingerprint;
	uint32 Offset = 0;
	uint32 Size = 0;
	uint32 Alignment = 1;
};

struct AVIDSCRIPTRUNTIME_API FAvidScriptWasmStateMigrationManifest
{
	static constexpr int32 LegacySchemaVersion = 1;
	static constexpr int32 SupportedSchemaVersion = 2;
	static constexpr int32 MinContractVersion = 1;
	static constexpr int32 MaxContractVersion = 65535;
	static constexpr int32 MaxSlotCount = 1024;
	static constexpr uint32 MaxSlotByteSize = 64 * 1024;
	static constexpr uint32 MaxTotalByteSize = 1024 * 1024;

	EAvidScriptWasmStateMigrationStrategy Strategy = EAvidScriptWasmStateMigrationStrategy::None;
	int32 SchemaVersion = LegacySchemaVersion;
	FString Policy = TEXT("compatible");
	int32 ContractVersion = MinContractVersion;
	FString OwnerTypeId;
	TArray<FAvidScriptWasmStateSlot> Slots;

	bool IsEnabled() const
	{
		return Strategy == EAvidScriptWasmStateMigrationStrategy::HostSnapshot;
	}
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
	FAvidScriptWasmStateMigrationManifest StateMigration;
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
	bool bHostEffectTransactionAttempted = false;
	bool bHostEffectTransactionCommitted = false;
	bool bHostEffectRollbackAttempted = false;
	bool bHostEffectRollbackSucceeded = false;
	int32 HostEffectCapturedObjectCount = 0;
	int32 HostEffectRestoredObjectCount = 0;
	int32 HostEffectFailedObjectCount = 0;
	FString HostEffectErrorSource;
	bool bStateMigrationAttempted = false;
	bool bStateMigrationApplied = false;
	int32 StateMigrationMigratedSlotCount = 0;
	int32 StateMigrationMigratedByteCount = 0;
	int32 StateMigrationSkippedSlotCount = 0;
	int32 StateMigrationAliasedSlotCount = 0;
	FString StateMigrationStableId;
	FString PreviousModuleId;
	FString CandidateModuleId;
	FString ActiveModuleId;
	FString ExportName;
	FString ErrorCategory;
	FString NextAction;
	FString ErrorMessage;
	FAvidScriptWasmSmokeResult RuntimeResult;
};
