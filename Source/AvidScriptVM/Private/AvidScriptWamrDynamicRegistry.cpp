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
	EAvidScriptWamrRawResultKind& OutResultKind,
	const bool bAllowScalarOrVoid = false)
{
	OutParameterCount = 0;
	OutResultKind = EAvidScriptWamrRawResultKind::I32;
	FAvidScriptVmAbiSignature ParsedSignature;
	FString ParseError;
	// Prepared scalar imports use 'd' for f64, while the generic ABI parser uses 'F'.
	const FString AbiSignature = bAllowScalarOrVoid ? Signature.Replace(TEXT("d"), TEXT("F")) : Signature;
	if (!ParseAvidScriptVmAbiSignature(AbiSignature, ParsedSignature, ParseError))
	{
		return false;
	}
	if (!bAllowScalarOrVoid && (!ParsedSignature.bHasResult
		|| (ParsedSignature.Result != EAvidScriptVmValueKind::I32
			&& ParsedSignature.Result != EAvidScriptVmValueKind::I64)))
	{
		return false;
	}
	OutParameterCount = static_cast<uint32>(ParsedSignature.Parameters.Num());
	if (!ParsedSignature.bHasResult) { OutResultKind = EAvidScriptWamrRawResultKind::Void; return true; }
	switch (ParsedSignature.Result)
	{
	case EAvidScriptVmValueKind::I32: OutResultKind = EAvidScriptWamrRawResultKind::I32; return true;
	case EAvidScriptVmValueKind::I64: OutResultKind = EAvidScriptWamrRawResultKind::I64; return true;
	case EAvidScriptVmValueKind::F32: OutResultKind = EAvidScriptWamrRawResultKind::F32; return true;
	case EAvidScriptVmValueKind::F64: OutResultKind = EAvidScriptWamrRawResultKind::F64; return true;
	default: return false;
	}
}

const TCHAR* GetWamrSupplementalSignature(const EAvidScriptVmTypedHostShape Shape)
{
	switch (Shape)
	{
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get: return TEXT("(I)i");
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Set: return TEXT("(Ii)");
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Get: return TEXT("(I)I");
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Set: return TEXT("(II)");
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Get: return TEXT("(I)f");
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Set: return TEXT("(If)");
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Get: return TEXT("(I)d");
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Set: return TEXT("(Id)");
	default: return nullptr;
	}
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
	FString FailureCategory;
	FString FailureDetails;
	const bool bSucceeded = Attachment != nullptr
		&& Bridge != nullptr
		&& Bridge->DispatchDynamicHostCall(
			*Attachment,
			MakeArrayView(Arguments, static_cast<int32>(Attachment->ParameterCount)),
			ReturnValue,
			FailureCategory,
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
			Bridge->RecordHostImportFailure(
				ImportNameUtf8.Get(),
				FailureCategory.IsEmpty()
					? FString(TEXT("host_import_failed"))
					: FailureCategory,
				Details);
		}
		SetDynamicRawException(ExecEnv);
		ReturnValue = 0;
	}
	if (Attachment == nullptr || Attachment->ResultKind == EAvidScriptWamrRawResultKind::Void) return;
	if (Attachment->ResultKind == EAvidScriptWamrRawResultKind::I64
		|| Attachment->ResultKind == EAvidScriptWamrRawResultKind::F64)
	{
		FMemory::Memcpy(Arguments, &ReturnValue, sizeof(ReturnValue));
	}
	else
	{
		const uint32 LowBits = static_cast<uint32>(ReturnValue);
		FMemory::Memcpy(Arguments, &LowBits, sizeof(LowBits));
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

static bool AcquireWamrValidatedImports(
	TConstArrayView<FAvidScriptVmDynamicImport> Imports,
	TArray<FAvidScriptWamrDynamicRegistration>& OutRegistrations,
	FAvidScriptVmError& OutError)
{
	OutRegistrations.Reset();
#if !AVIDSCRIPT_WITH_WAMR
	SetDynamicRegistryError(
		OutError,
		TEXT("backend_unavailable"),
		TEXT("WAMR artifacts are unavailable for this target."));
	return false;
#else
	FScopeLock Lock(&GDynamicRegistryCriticalSection);
	for (const FAvidScriptVmDynamicImport& Import : Imports)
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
			Entry->Attachment.ResultKind, true);
		CopyAvidScriptDynamicUtf8(Import.ModuleName, Entry->ModuleNameUtf8);
		CopyAvidScriptDynamicUtf8(Import.ImportName, Entry->ImportNameUtf8);
		// Prepared scalar imports spell f64 'd'; WAMR's native registry spells it 'F'.
		// Keep the canonical signature in Attachment for identity/contract checks.
		CopyAvidScriptDynamicUtf8(Import.Signature.Replace(TEXT("d"), TEXT("F")), Entry->SignatureUtf8);
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

bool AcquireAvidScriptWamrDynamicImports(
	const FAvidScriptVmBindingPackage& Package,
	TArray<FAvidScriptWamrDynamicRegistration>& OutRegistrations,
	FAvidScriptVmError& OutError)
{
	OutRegistrations.Reset();
	return ValidateAvidScriptVmBindingPackage(Package, OutError)
		&& AcquireWamrValidatedImports(Package.Imports, OutRegistrations, OutError);
}

bool AcquireAvidScriptWamrSupplementalImports(
	TConstArrayView<FAvidScriptVmTypedHostImport> Imports,
	TArray<FAvidScriptWamrDynamicRegistration>& OutRegistrations,
	FAvidScriptVmError& OutError)
{
	OutRegistrations.Reset();
	TArray<FAvidScriptVmDynamicImport> RawImports;
	TSet<FString> Identities;
	for (const FAvidScriptVmTypedHostImport& Import : Imports)
	{
		const FString Identity = MakeAvidScriptDynamicRegistryKey(Import.ModuleName, Import.ImportName);
		const TCHAR* ExpectedSignature = GetWamrSupplementalSignature(Import.Shape);
		if (!Import.bSupplementalRuntimeAuthority || Import.BindingOrdinal != MAX_uint32
			|| Import.StableId.IsEmpty() || Import.ModuleName != TEXT("avidscript")
			|| !IsAvidScriptDynamicSafeToken(Import.ImportName) || Identities.Contains(Identity)
			|| IsAvidScriptVmStaticHostImport(Import.ModuleName, Import.ImportName)
			|| ExpectedSignature == nullptr || Import.Signature != ExpectedSignature
			|| !Import.PreparedTarget.IsBoundForShape(Import.Shape))
		{
			SetDynamicRegistryError(OutError, TEXT("supplemental_import_unsupported"),
				TEXT("WAMR supplemental imports require a unique prepared packed-receiver scalar capability with an exact signature."),
				Import.ModuleName, Import.ImportName);
			return false;
		}
		Identities.Add(Identity);
		FAvidScriptVmDynamicImport& Raw = RawImports.AddDefaulted_GetRef();
		// The global registry owns only the ABI stub. Context and authority remain per VM.
		Raw.StableId = TEXT("supplemental:") + Identity + TEXT(":") + Import.Signature;
		Raw.ModuleName = Import.ModuleName;
		Raw.ImportName = Import.ImportName;
		Raw.Signature = Import.Signature;
		Raw.Ordinal = static_cast<uint32>(RawImports.Num() - 1);
	}
	return AcquireWamrValidatedImports(RawImports, OutRegistrations, OutError);
}

template <typename ValueType>
static bool InvokeWamrSupplementalGetter(
	EAvidScriptVmTypedHostStatus (*Target)(void*, int64, ValueType&),
	void* Context, TConstArrayView<uint64> Arguments, int64& OutResultBits)
{
	if (Arguments.Num() != 1 || Target == nullptr) return false;
	ValueType Value{};
	if (Target(Context, static_cast<int64>(Arguments[0]), Value) != EAvidScriptVmTypedHostStatus::Succeeded) return false;
	using BitsType = std::conditional_t<sizeof(ValueType) == 4, uint32, uint64>;
	BitsType Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Value));
	OutResultBits = static_cast<int64>(static_cast<uint64>(Bits));
	return true;
}

template <typename ValueType>
static bool InvokeWamrSupplementalSetter(
	EAvidScriptVmTypedHostStatus (*Target)(void*, int64, ValueType),
	void* Context, TConstArrayView<uint64> Arguments)
{
	if (Arguments.Num() != 2 || Target == nullptr) return false;
	using BitsType = std::conditional_t<sizeof(ValueType) == 4, uint32, uint64>;
	const BitsType Bits = static_cast<BitsType>(Arguments[1]);
	ValueType Value{};
	FMemory::Memcpy(&Value, &Bits, sizeof(Value));
	return Target(Context, static_cast<int64>(Arguments[0]), Value) == EAvidScriptVmTypedHostStatus::Succeeded;
}

bool DispatchAvidScriptWamrSupplementalImport(
	const FAvidScriptVmTypedHostImport& Import, TConstArrayView<uint64> Arguments, int64& OutResultBits)
{
	OutResultBits = 0;
	const auto& Target = Import.PreparedTarget;
	if (Target.Context == nullptr) return false;
	switch (Import.Shape)
	{
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get:
		return InvokeWamrSupplementalGetter(Target.PackedSelfPropertyI32Get, Target.Context, Arguments, OutResultBits);
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Set:
		return InvokeWamrSupplementalSetter(Target.PackedSelfPropertyI32Set, Target.Context, Arguments);
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Get:
		return InvokeWamrSupplementalGetter(Target.PackedSelfPropertyI64Get, Target.Context, Arguments, OutResultBits);
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Set:
		return InvokeWamrSupplementalSetter(Target.PackedSelfPropertyI64Set, Target.Context, Arguments);
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Get:
		return InvokeWamrSupplementalGetter(Target.PackedSelfPropertyF32Get, Target.Context, Arguments, OutResultBits);
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Set:
		return InvokeWamrSupplementalSetter(Target.PackedSelfPropertyF32Set, Target.Context, Arguments);
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Get:
		return InvokeWamrSupplementalGetter(Target.PackedSelfPropertyF64Get, Target.Context, Arguments, OutResultBits);
	case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Set:
		return InvokeWamrSupplementalSetter(Target.PackedSelfPropertyF64Set, Target.Context, Arguments);
	default: return false;
	}
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
