#include "AvidScriptWamrDynamicRegistry.h"

#include "AvidScriptWamrHostBindings.h"
#include "Containers/StringConv.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

#ifndef AVIDSCRIPT_WITH_WAMR
#define AVIDSCRIPT_WITH_WAMR 0
#endif

#if AVIDSCRIPT_WITH_WAMR
extern "C"
{
#include "wasm_export.h"
}
#endif

namespace
{
void SetDynamicRegistryError(
	FAvidScriptVmError& OutError,
	const TCHAR* Category,
	const FString& Details,
	const FString& ModuleName = FString(),
	const FString& ImportName = FString())
{
	OutError.Reset();
	OutError.Category = Category;
	OutError.Details = Details;
	OutError.ImportModuleName = ModuleName;
	OutError.ImportName = ImportName;
}

bool IsAvidScriptLowerSha256(const FString& Value)
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

bool IsAvidScriptDynamicSafeToken(const FString& Value)
{
	if (Value.IsEmpty())
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		if (!FChar::IsAlnum(Character)
			&& Character != TEXT('_')
			&& Character != TEXT('-')
			&& Character != TEXT('.'))
		{
			return false;
		}
	}
	return true;
}

bool ParseAvidScriptRawSignature(
	const FString& Signature,
	uint32& OutParameterCount,
	EAvidScriptWamrRawResultKind& OutResultKind)
{
	OutParameterCount = 0;
	OutResultKind = EAvidScriptWamrRawResultKind::I32;
	FAvidScriptVmAbiSignature ParsedSignature;
	FString ParseError;
	if (!ParseAvidScriptVmAbiSignature(Signature, ParsedSignature, ParseError)
		|| !ParsedSignature.bHasResult)
	{
		return false;
	}
	if (ParsedSignature.Result != EAvidScriptVmValueKind::I32
		&& ParsedSignature.Result != EAvidScriptVmValueKind::I64)
	{
		return false;
	}
	OutParameterCount = static_cast<uint32>(ParsedSignature.Parameters.Num());
	OutResultKind = ParsedSignature.Result == EAvidScriptVmValueKind::I64
		? EAvidScriptWamrRawResultKind::I64
		: EAvidScriptWamrRawResultKind::I32;
	return true;
}

FString MakeAvidScriptDynamicRegistryKey(const FString& ModuleName, const FString& ImportName)
{
	return ModuleName + TEXT("\n") + ImportName;
}

#if AVIDSCRIPT_WITH_WAMR
struct FAvidScriptWamrDynamicRegistryEntry
{
	FAvidScriptWamrRawImportAttachment Attachment;
	TArray<ANSICHAR> ModuleNameUtf8;
	TArray<ANSICHAR> ImportNameUtf8;
	TArray<ANSICHAR> SignatureUtf8;
	NativeSymbol Symbol = {};
	uint32 ReferenceCount = 0;
};

FCriticalSection GDynamicRegistryCriticalSection;
TMap<FString, TUniquePtr<FAvidScriptWamrDynamicRegistryEntry>> GDynamicRegistry;

void CopyAvidScriptDynamicUtf8(const FString& Value, TArray<ANSICHAR>& OutBytes)
{
	const FTCHARToUTF8 Converted(*Value);
	OutBytes.Reset(Converted.Length() + 1);
	OutBytes.Append(Converted.Get(), Converted.Length());
	OutBytes.Add('\0');
}

void SetDynamicRawException(wasm_exec_env_t ExecEnv)
{
	if (ExecEnv == nullptr)
	{
		return;
	}
	if (wasm_module_inst_t ModuleInstance = wasm_runtime_get_module_inst(ExecEnv))
	{
		wasm_runtime_set_exception(ModuleInstance, "avidscript_dynamic_host_import_failed");
	}
}

void InvokeAvidScriptDynamicRawImport(wasm_exec_env_t ExecEnv, uint64* Arguments)
{
	if (Arguments == nullptr)
	{
		SetDynamicRawException(ExecEnv);
		return;
	}

	const FAvidScriptWamrRawImportAttachment* Attachment = ExecEnv != nullptr
		? static_cast<const FAvidScriptWamrRawImportAttachment*>(wasm_runtime_get_function_attachment(ExecEnv))
		: nullptr;
	IAvidScriptWamrHostBridge* Bridge = ExecEnv != nullptr
		? static_cast<IAvidScriptWamrHostBridge*>(wasm_runtime_get_user_data(ExecEnv))
		: nullptr;
	int64 ReturnValue = 0;
	FString FailureDetails;
	const bool bSucceeded = Attachment != nullptr
		&& Bridge != nullptr
		&& Bridge->DispatchDynamicHostCall(
			*Attachment,
			MakeArrayView(Arguments, static_cast<int32>(Attachment->ParameterCount)),
			ReturnValue,
			FailureDetails);
	if (!bSucceeded)
	{
		const FString ImportName = Attachment != nullptr ? Attachment->ImportName : TEXT("<unknown>");
		const FString Details = FailureDetails.IsEmpty()
			? FString::Printf(TEXT("Dynamic host dispatcher rejected avidscript.%s."), *ImportName)
			: FailureDetails;
		if (Bridge != nullptr)
		{
			FTCHARToUTF8 ImportNameUtf8(*ImportName);
			Bridge->RecordHostImportFailure(ImportNameUtf8.Get(), Details);
		}
		SetDynamicRawException(ExecEnv);
		ReturnValue = 0;
	}
	if (Attachment != nullptr
		&& Attachment->ResultKind == EAvidScriptWamrRawResultKind::I64)
	{
		*reinterpret_cast<int64*>(Arguments) = ReturnValue;
	}
	else
	{
		*reinterpret_cast<int32*>(Arguments) = static_cast<int32>(ReturnValue);
	}
}

void ReleaseAvidScriptDynamicRegistryEntry(const FAvidScriptWamrDynamicRegistration& Registration)
{
	if (Registration.Attachment == nullptr)
	{
		return;
	}
	const FString Key = MakeAvidScriptDynamicRegistryKey(
		Registration.Attachment->ModuleName,
		Registration.Attachment->ImportName);
	TUniquePtr<FAvidScriptWamrDynamicRegistryEntry>* EntryValue = GDynamicRegistry.Find(Key);
	if (EntryValue == nullptr || EntryValue->Get()->Attachment.StableId != Registration.Attachment->StableId)
	{
		return;
	}
	FAvidScriptWamrDynamicRegistryEntry& Entry = *EntryValue->Get();
	if (Entry.ReferenceCount > 1)
	{
		--Entry.ReferenceCount;
		return;
	}
	wasm_runtime_unregister_natives(Entry.ModuleNameUtf8.GetData(), &Entry.Symbol);
	GDynamicRegistry.Remove(Key);
}
#endif
} // namespace

bool ValidateAvidScriptVmBindingPackage(
	const FAvidScriptVmBindingPackage& Package,
	FAvidScriptVmError& OutError)
{
	OutError.Reset();
	if (!IsAvidScriptDynamicSafeToken(Package.PackageName)
		|| !IsAvidScriptLowerSha256(Package.PackageHash)
		|| Package.Imports.IsEmpty())
	{
		SetDynamicRegistryError(
			OutError,
			TEXT("dynamic_package_invalid"),
			TEXT("Dynamic binding packages require a safe name, lowercase SHA-256, and at least one import."));
		return false;
	}

	TSet<FString> StableIds;
	TSet<FString> ImportKeys;
	for (int32 Index = 0; Index < Package.Imports.Num(); ++Index)
	{
		const FAvidScriptVmDynamicImport& Import = Package.Imports[Index];
		uint32 ParameterCount = 0;
		EAvidScriptWamrRawResultKind ResultKind = EAvidScriptWamrRawResultKind::I32;
		const FString ImportKey = MakeAvidScriptDynamicRegistryKey(Import.ModuleName, Import.ImportName);
		if (!IsAvidScriptLowerSha256(Import.StableId)
			|| Import.Ordinal != static_cast<uint32>(Index)
			|| Import.ModuleName != TEXT("avidscript")
			|| !IsAvidScriptDynamicSafeToken(Import.ImportName)
			|| !ParseAvidScriptRawSignature(Import.Signature, ParameterCount, ResultKind)
			|| StableIds.Contains(Import.StableId)
			|| ImportKeys.Contains(ImportKey)
			|| IsAvidScriptVmStaticHostImport(Import.ModuleName, Import.ImportName))
		{
			SetDynamicRegistryError(
				OutError,
				TEXT("dynamic_package_invalid"),
				FString::Printf(TEXT("Dynamic import %d violates the VM package contract."), Index),
				Import.ModuleName,
				Import.ImportName);
			return false;
		}
		StableIds.Add(Import.StableId);
		ImportKeys.Add(ImportKey);
	}
	return true;
}

bool AcquireAvidScriptWamrDynamicImports(
	const FAvidScriptVmBindingPackage& Package,
	TArray<FAvidScriptWamrDynamicRegistration>& OutRegistrations,
	FAvidScriptVmError& OutError)
{
	OutRegistrations.Reset();
	if (!ValidateAvidScriptVmBindingPackage(Package, OutError))
	{
		return false;
	}
#if !AVIDSCRIPT_WITH_WAMR
	SetDynamicRegistryError(
		OutError,
		TEXT("backend_unavailable"),
		TEXT("WAMR artifacts are unavailable for this target."));
	return false;
#else
	FScopeLock Lock(&GDynamicRegistryCriticalSection);
	for (const FAvidScriptVmDynamicImport& Import : Package.Imports)
	{
		const FString Key = MakeAvidScriptDynamicRegistryKey(Import.ModuleName, Import.ImportName);
		if (TUniquePtr<FAvidScriptWamrDynamicRegistryEntry>* ExistingValue = GDynamicRegistry.Find(Key))
		{
			FAvidScriptWamrDynamicRegistryEntry& Existing = *ExistingValue->Get();
			if (Existing.Attachment.StableId != Import.StableId
				|| Existing.Attachment.Signature != Import.Signature)
			{
				for (const FAvidScriptWamrDynamicRegistration& Registration : OutRegistrations)
				{
					ReleaseAvidScriptDynamicRegistryEntry(Registration);
				}
				OutRegistrations.Reset();
				SetDynamicRegistryError(
					OutError,
					TEXT("dynamic_import_conflict"),
					TEXT("A registered raw import cannot change stable identity or signature."),
					Import.ModuleName,
					Import.ImportName);
				return false;
			}
			++Existing.ReferenceCount;
			OutRegistrations.Add({ &Existing.Attachment, Import.Ordinal });
			continue;
		}

		TUniquePtr<FAvidScriptWamrDynamicRegistryEntry> Entry = MakeUnique<FAvidScriptWamrDynamicRegistryEntry>();
		Entry->Attachment.StableId = Import.StableId;
		Entry->Attachment.ModuleName = Import.ModuleName;
		Entry->Attachment.ImportName = Import.ImportName;
		Entry->Attachment.Signature = Import.Signature;
		ParseAvidScriptRawSignature(
			Import.Signature,
			Entry->Attachment.ParameterCount,
			Entry->Attachment.ResultKind);
		CopyAvidScriptDynamicUtf8(Import.ModuleName, Entry->ModuleNameUtf8);
		CopyAvidScriptDynamicUtf8(Import.ImportName, Entry->ImportNameUtf8);
		CopyAvidScriptDynamicUtf8(Import.Signature, Entry->SignatureUtf8);
		Entry->Symbol.symbol = Entry->ImportNameUtf8.GetData();
		Entry->Symbol.func_ptr = reinterpret_cast<void*>(InvokeAvidScriptDynamicRawImport);
		Entry->Symbol.signature = Entry->SignatureUtf8.GetData();
		Entry->Symbol.attachment = &Entry->Attachment;
		Entry->ReferenceCount = 1;
		if (!wasm_runtime_register_natives_raw(
			Entry->ModuleNameUtf8.GetData(),
			&Entry->Symbol,
			1))
		{
			for (const FAvidScriptWamrDynamicRegistration& Registration : OutRegistrations)
			{
				ReleaseAvidScriptDynamicRegistryEntry(Registration);
			}
			OutRegistrations.Reset();
			SetDynamicRegistryError(
				OutError,
				TEXT("dynamic_import_registration_failed"),
				TEXT("WAMR rejected a generated raw native import."),
				Import.ModuleName,
				Import.ImportName);
			return false;
		}
		FAvidScriptWamrDynamicRegistryEntry* EntryPointer = Entry.Get();
		GDynamicRegistry.Add(Key, MoveTemp(Entry));
		OutRegistrations.Add({ &EntryPointer->Attachment, Import.Ordinal });
	}
	return true;
#endif
}

FAvidScriptWamrNativeRegistryScope::FAvidScriptWamrNativeRegistryScope()
{
#if AVIDSCRIPT_WITH_WAMR
	GDynamicRegistryCriticalSection.Lock();
#endif
}

FAvidScriptWamrNativeRegistryScope::~FAvidScriptWamrNativeRegistryScope()
{
#if AVIDSCRIPT_WITH_WAMR
	GDynamicRegistryCriticalSection.Unlock();
#endif
}

void ReleaseAvidScriptWamrDynamicImports(
	TArray<FAvidScriptWamrDynamicRegistration>& Registrations)
{
#if AVIDSCRIPT_WITH_WAMR
	FScopeLock Lock(&GDynamicRegistryCriticalSection);
	for (const FAvidScriptWamrDynamicRegistration& Registration : Registrations)
	{
		ReleaseAvidScriptDynamicRegistryEntry(Registration);
	}
#endif
	Registrations.Reset();
}

bool IsAvidScriptWamrDynamicRegistryEmpty()
{
#if !AVIDSCRIPT_WITH_WAMR
	return true;
#else
	FScopeLock Lock(&GDynamicRegistryCriticalSection);
	return GDynamicRegistry.IsEmpty();
#endif
}
