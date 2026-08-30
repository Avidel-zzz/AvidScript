#pragma once

#include "AvidScriptVmBackend.h"
#include "CoreMinimal.h"

struct FAvidScriptWasmModuleLayout;
struct FAvidScriptWasmReloadManifest;
struct FAvidScriptVmTypedHostImport;

class FScopedAvidScriptRuntimeImportAuthority final
{
public:
	explicit FScopedAvidScriptRuntimeImportAuthority(
		TConstArrayView<FAvidScriptVmExpectedImport> Imports);
	~FScopedAvidScriptRuntimeImportAuthority();

	FScopedAvidScriptRuntimeImportAuthority(
		const FScopedAvidScriptRuntimeImportAuthority&) = delete;
	FScopedAvidScriptRuntimeImportAuthority& operator=(
		const FScopedAvidScriptRuntimeImportAuthority&) = delete;
	TConstArrayView<FAvidScriptVmExpectedImport> GetImports() const;

private:
	const FScopedAvidScriptRuntimeImportAuthority* Previous = nullptr;
	TArray<FAvidScriptVmExpectedImport> Imports;
};

struct FAvidScriptWasmImportContractResult
{
	FString ErrorCategory;
	FString ErrorDetails;
	FString NextAction;
};

bool ValidateAvidScriptWasmImportContract(
	const FAvidScriptWasmModuleLayout& WasmLayout,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmImportContractResult& OutResult,
	TConstArrayView<FAvidScriptVmTypedHostImport> SupplementalTypedImports = {},
	TConstArrayView<FAvidScriptVmExpectedImport> RuntimeAuthorizedImports = {});

bool InspectAndValidateAvidScriptWasmImportContract(
	TConstArrayView<uint8> Bytecode,
	const FAvidScriptWasmReloadManifest& Manifest,
	FAvidScriptWasmImportContractResult& OutResult,
	TConstArrayView<FAvidScriptVmTypedHostImport> SupplementalTypedImports = {},
	TConstArrayView<FAvidScriptVmExpectedImport> RuntimeAuthorizedImports = {});
