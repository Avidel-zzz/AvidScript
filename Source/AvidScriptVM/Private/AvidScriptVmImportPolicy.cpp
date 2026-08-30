#include "AvidScriptVmBackend.h"

#include "AvidScriptWamrDynamicRegistry.h"
#include "AvidScriptWasmModuleLayout.h"

namespace
{
FString MakeAvidScriptVmImportIdentityKey(const FString& ModuleName, const FString& ImportName)
{
	return FString::Printf(TEXT("%d:"), ModuleName.Len())
		+ ModuleName
		+ FString::Printf(TEXT("%d:"), ImportName.Len())
		+ ImportName;
}

bool SetAvidScriptVmImportPolicyError(
	FAvidScriptVmError& OutError,
	const FString& Category,
	const FString& Details,
	const FString& ModuleName = FString(),
	const FString& ImportName = FString())
{
	OutError.Reset();
	OutError.Category = Category;
	OutError.Details = Details;
	OutError.ImportModuleName = ModuleName;
	OutError.ImportName = ImportName;
	return false;
}
} // namespace

bool ValidateAvidScriptVmImportContract(
	const FAvidScriptWasmModuleLayout& ActualLayout,
	const FAvidScriptVmBindingPackage* BindingPackage,
	TConstArrayView<FAvidScriptVmExpectedImport> ExpectedImports,
	bool bEnforceExpectedImports,
	FAvidScriptVmError& OutError,
	TConstArrayView<FAvidScriptVmTypedHostImport> SupplementalTypedImports,
	TConstArrayView<FAvidScriptVmExpectedImport> RuntimeAuthorizedImports)
{
	OutError.Reset();
	if (BindingPackage != nullptr && !ValidateAvidScriptVmBindingPackage(*BindingPackage, OutError))
	{
		return false;
	}

	if (bEnforceExpectedImports)
	{
		TMap<FString, int32> ActualImportCounts;
		for (const FAvidScriptWasmFunctionImport& ActualImport : ActualLayout.FunctionImports)
		{
			++ActualImportCounts.FindOrAdd(MakeAvidScriptVmImportIdentityKey(
				ActualImport.ModuleName,
				ActualImport.ImportName));
		}

		TMap<FString, int32> ExpectedImportCounts;
		for (const FAvidScriptVmExpectedImport& ExpectedImport : ExpectedImports)
		{
			++ExpectedImportCounts.FindOrAdd(MakeAvidScriptVmImportIdentityKey(
				ExpectedImport.ModuleName,
				ExpectedImport.ImportName));
		}

		bool bIdentitiesMatch =
			ActualImportCounts.Num() == ExpectedImportCounts.Num()
			&& ActualLayout.FunctionImports.Num() == ExpectedImports.Num();
		if (bIdentitiesMatch)
		{
			for (const TPair<FString, int32>& Pair : ActualImportCounts)
			{
				const int32* ExpectedCount = ExpectedImportCounts.Find(Pair.Key);
				if (ExpectedCount == nullptr || *ExpectedCount != Pair.Value)
				{
					bIdentitiesMatch = false;
					break;
				}
			}
		}
		if (!bIdentitiesMatch)
		{
			return SetAvidScriptVmImportPolicyError(
				OutError,
				TEXT("manifest_wasm_import_mismatch"),
				FString::Printf(
					TEXT("script manifest imports=%d differ from WASM function imports=%d"),
					ExpectedImports.Num(),
					ActualLayout.FunctionImports.Num()));
		}
	}

	TSet<FString> AuthorizedDynamicImports;
	if (BindingPackage != nullptr)
	{
		AuthorizedDynamicImports.Reserve(BindingPackage->Imports.Num());
		for (const FAvidScriptVmDynamicImport& Import : BindingPackage->Imports)
		{
			AuthorizedDynamicImports.Add(MakeAvidScriptVmImportIdentityKey(
				Import.ModuleName,
				Import.ImportName));
		}
	}
	for (const FAvidScriptVmTypedHostImport& Import : SupplementalTypedImports)
	{
		if (!Import.bSupplementalRuntimeAuthority
			|| Import.BindingOrdinal != MAX_uint32
			|| Import.ModuleName.IsEmpty()
			|| Import.ImportName.IsEmpty()
			|| !Import.PreparedTarget.IsBoundForShape(Import.Shape))
		{
			return SetAvidScriptVmImportPolicyError(
				OutError,
				TEXT("supplemental_import_authority_invalid"),
				TEXT("supplemental typed imports require an exact prepared runtime capability"),
				Import.ModuleName,
				Import.ImportName);
		}
		AuthorizedDynamicImports.Add(MakeAvidScriptVmImportIdentityKey(
			Import.ModuleName,
			Import.ImportName));
	}
	for (const FAvidScriptVmExpectedImport& Import : RuntimeAuthorizedImports)
	{
		if (Import.ModuleName.IsEmpty() || Import.ImportName.IsEmpty())
		{
			return SetAvidScriptVmImportPolicyError(
				OutError,
				TEXT("runtime_import_authority_invalid"),
				TEXT("runtime-authorized import identities cannot be empty"));
		}
		AuthorizedDynamicImports.Add(MakeAvidScriptVmImportIdentityKey(
			Import.ModuleName,
			Import.ImportName));
	}

	for (const FAvidScriptWasmFunctionImport& ActualImport : ActualLayout.FunctionImports)
	{
		if (IsAvidScriptVmStaticHostImport(ActualImport.ModuleName, ActualImport.ImportName))
		{
			continue;
		}
		const FString IdentityKey = MakeAvidScriptVmImportIdentityKey(
			ActualImport.ModuleName,
			ActualImport.ImportName);
		if (BindingPackage == nullptr
			&& !AuthorizedDynamicImports.Contains(IdentityKey))
		{
			return SetAvidScriptVmImportPolicyError(
				OutError,
				TEXT("binding_package_missing"),
				FString::Printf(
					TEXT("dynamic import %s.%s requires a verified binding package"),
					*ActualImport.ModuleName,
					*ActualImport.ImportName),
				ActualImport.ModuleName,
				ActualImport.ImportName);
		}
		if (!AuthorizedDynamicImports.Contains(IdentityKey))
		{
			return SetAvidScriptVmImportPolicyError(
				OutError,
				TEXT("binding_package_import_mismatch"),
				FString::Printf(
					TEXT("dynamic import %s.%s is not authorized by the current binding package"),
					*ActualImport.ModuleName,
					*ActualImport.ImportName),
				ActualImport.ModuleName,
				ActualImport.ImportName);
		}
	}
	return true;
}
