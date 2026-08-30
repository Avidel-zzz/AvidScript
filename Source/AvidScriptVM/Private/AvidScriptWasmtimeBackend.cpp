#include "AvidScriptVmBackend.h"

#include "AvidScriptHash.h"
#include "AvidScriptVmStaticHostImports.h"
#include "AvidScriptWasmtimeApi.h"
#include "AvidScriptWasmtimeRuntimeSupport.h"
#include "AvidScriptWasmtimeTypedHostApi.h"
#include "AvidScriptWasmModuleLayout.h"

#include "Containers/StringConv.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

#ifndef AVIDSCRIPT_WITH_WASMTIME
#define AVIDSCRIPT_WITH_WASMTIME 0
#endif

namespace
{
FCriticalSection GWasmtimeIdentityCriticalSection;
uint64 GNextWasmtimeInstanceIdentity = 1ull << 63;

uint64 AllocateWasmtimeInstanceIdentity()
{
	FScopeLock Lock(&GWasmtimeIdentityCriticalSection);
	const uint64 Identity = GNextWasmtimeInstanceIdentity++;
	if (GNextWasmtimeInstanceIdentity == 0)
	{
		GNextWasmtimeInstanceIdentity = 1ull << 63;
	}
	return Identity;
}

double MeasureWasmtimeElapsedMs(double StartSeconds)
{
	return FMath::Max((FPlatformTime::Seconds() - StartSeconds) * 1000.0, 0.0001);
}

void SetWasmtimeError(FAvidScriptVmError& OutError, const TCHAR* Category, const FString& Details)
{
	OutError.Reset();
	OutError.Category = Category;
	OutError.Details = Details;
}

FString MakeWasmtimeImportIdentityKey(const FString& ModuleName, const FString& ImportName)
{
	return FString::Printf(TEXT("%d:"), ModuleName.Len())
		+ ModuleName
		+ FString::Printf(TEXT("%d:"), ImportName.Len())
		+ ImportName;
}

FAvidScriptVmBackendInfo MakeWasmtimeBackendInfo(
	EAvidScriptVmArtifactFormat ArtifactFormat)
{
	FAvidScriptVmBackendInfo Info;
	Info.Kind = EAvidScriptVmBackendKind::Wasmtime;
	Info.ExecutionMode = ArtifactFormat == EAvidScriptVmArtifactFormat::WasmtimeSerialized
		? EAvidScriptVmExecutionMode::Aot
		: EAvidScriptVmExecutionMode::Jit;
	Info.ArtifactFormat = ArtifactFormat;
	Info.Capabilities = EAvidScriptVmCapability::GuestMemory
		| EAvidScriptVmCapability::StructuredStack;
	if (ArtifactFormat == EAvidScriptVmArtifactFormat::WasmtimeSerialized)
	{
		Info.Capabilities |= EAvidScriptVmCapability::Aot
			| EAvidScriptVmCapability::PrecompiledArtifact;
		Info.StableBackendId = TEXT("wasmtime.cranelift.precompiled");
	}
	else
	{
		Info.Capabilities |= EAvidScriptVmCapability::Jit;
		Info.StableBackendId = TEXT("wasmtime.cranelift.jit");
	}
	InitializeAvidScriptWasmtimeRuntimeDescriptor(Info);
	return Info;
}

#if AVIDSCRIPT_WITH_WASMTIME
FString ConvertWasmtimeUtf8(const char* Data, size_t Size)
{
	if (Data == nullptr || Size == 0)
	{
		return FString();
	}
	if (Data[Size - 1] == '\0')
	{
		--Size;
	}
	const FUTF8ToTCHAR Converted(Data, static_cast<int32>(FMath::Min<size_t>(Size, MAX_int32)));
	return FString(Converted.Length(), Converted.Get());
}

FString ConsumeWasmtimeFailure(
	AvidScriptWasmtimeFailure* Failure,
	TArray<FAvidScriptVmStackFrame>& OutFrames,
	bool& bOutWasTrap)
{
	OutFrames.Reset();
	bOutWasTrap = avidscript_wasmtime_failure_is_trap(Failure);
	if (Failure == nullptr)
	{
		return FString();
	}
	size_t MessageSize = 0;
	const char* Message = avidscript_wasmtime_failure_message(Failure, &MessageSize);
	const FString Details = ConvertWasmtimeUtf8(Message, MessageSize);
	const size_t FrameCount = avidscript_wasmtime_failure_frame_count(Failure);
	OutFrames.Reserve(static_cast<int32>(FMath::Min<size_t>(FrameCount, MAX_int32)));
	for (size_t Index = 0; Index < FrameCount; ++Index)
	{
		uint32 FunctionIndex = 0;
		size_t FunctionOffset = 0;
		const char* FunctionName = nullptr;
		size_t FunctionNameSize = 0;
		if (!avidscript_wasmtime_failure_frame(
			Failure,
			Index,
			&FunctionIndex,
			&FunctionOffset,
			&FunctionName,
			&FunctionNameSize))
		{
			continue;
		}
		FAvidScriptVmStackFrame& OutFrame = OutFrames.AddDefaulted_GetRef();
		OutFrame.FunctionIndex = FunctionIndex;
		OutFrame.FunctionOffset = static_cast<uint32>(FMath::Min<size_t>(
			FunctionOffset,
			MAX_uint32));
		OutFrame.RawFunctionToken = ConvertWasmtimeUtf8(FunctionName, FunctionNameSize);
		if (OutFrame.RawFunctionToken.IsEmpty())
		{
			OutFrame.RawFunctionToken = FString::Printf(TEXT("$f%u"), OutFrame.FunctionIndex);
		}
	}
	avidscript_wasmtime_failure_delete(Failure);
	return Details;
}

AvidScriptWasmtimeValueKind ToWasmtimeValueKind(EAvidScriptVmValueKind Kind)
{
	switch (Kind)
	{
	case EAvidScriptVmValueKind::I32:
		return AVIDSCRIPT_WASMTIME_I32;
	case EAvidScriptVmValueKind::I64:
		return AVIDSCRIPT_WASMTIME_I64;
	case EAvidScriptVmValueKind::F32:
		return AVIDSCRIPT_WASMTIME_F32;
	case EAvidScriptVmValueKind::F64:
		return AVIDSCRIPT_WASMTIME_F64;
	default:
		return AVIDSCRIPT_WASMTIME_I32;
	}
}
#endif

class FAvidScriptWasmtimeBackend;

#if AVIDSCRIPT_WITH_WASMTIME
struct FAvidScriptWasmtimeHostContext
{
	FAvidScriptWasmtimeBackend* Backend = nullptr;
	const FAvidScriptVmStaticHostImport* Import = nullptr;
	FAvidScriptVmAbiSignature Signature;
};

struct FAvidScriptWasmtimeDynamicHostContext
{
	FAvidScriptWasmtimeBackend* Backend = nullptr;
	uint32 Ordinal = MAX_uint32;
	FString StableId;
	FString ModuleName;
	FString ImportName;
	FString CompactSignature;
	FAvidScriptVmAbiSignature Signature;
	FAvidScriptVmPreparedDynamicHostTarget PreparedTarget;
};

struct FAvidScriptWasmtimeTypedHostContext
{
	FAvidScriptWasmtimeBackend* Backend = nullptr;
	uint32 BindingOrdinal = MAX_uint32;
	FString StableId;
	FString ModuleName;
	FString ImportName;
	EAvidScriptVmTypedHostShape Shape = EAvidScriptVmTypedHostShape::None;
	FAvidScriptVmPreparedTypedHostTarget PreparedTarget;
};

struct FAvidScriptWasmtimeExportEntry
{
	FString Name;
	AvidScriptWasmtimeFunction* Function = nullptr;
	AvidScriptWasmtimePreparedCallShape PreparedCallShape =
		AVIDSCRIPT_WASMTIME_PREPARED_CALL_GENERIC;
	uint32 Generation = 0;
	uint32 CellCount = 0;
	uint32 ResultCellCount = 0;
};
#endif

class FAvidScriptWasmtimeBackend final : public IAvidScriptVmBackend, public IAvidScriptVmGuestMemory
{
public:
	explicit FAvidScriptWasmtimeBackend(
		EAvidScriptVmArtifactFormat ArtifactFormat)
		: BackendInfo(MakeWasmtimeBackendInfo(ArtifactFormat))
	{
		AdvanceBackendInstanceIdentity();
	}

	~FAvidScriptWasmtimeBackend() override
	{
		Unload();
	}

	const FAvidScriptVmBackendInfo& GetBackendInfo() const override
	{
		return BackendInfo;
	}

	bool Load(
		TArrayView<const uint8> Bytecode,
		const FString& InModuleId,
		const FAvidScriptVmLoadConfig& Config,
		FAvidScriptVmError& OutError) override
	{
		if (BackendInfo.ArtifactFormat != EAvidScriptVmArtifactFormat::WasmBytecode)
		{
			SetWasmtimeError(
				OutError,
				TEXT("artifact_format_mismatch"),
				TEXT("A precompiled Wasmtime backend requires LoadArtifact with canonical WASM provenance."));
			return false;
		}
		return LoadArtifact(
			FAvidScriptVmArtifactView::FromWasmBytecode(Bytecode),
			InModuleId,
			Config,
			OutError);
	}

	bool LoadArtifact(
		const FAvidScriptVmArtifactView& Artifact,
		const FString& InModuleId,
		const FAvidScriptVmLoadConfig& Config,
		FAvidScriptVmError& OutError) override
	{
		OutError.Reset();
		if (ActiveCallDepth > 0)
		{
			SetWasmtimeError(
				OutError,
				TEXT("reentrant_operation"),
				TEXT("A VM module cannot be replaced while a guest call is active."));
			return false;
		}
		Unload();
		LoadMetrics = FAvidScriptVmLoadMetrics();

#if !AVIDSCRIPT_WITH_WASMTIME
		SetWasmtimeError(OutError, TEXT("backend_unavailable"), TEXT("Wasmtime is unavailable for this target."));
		return false;
#else
		if (Artifact.ArtifactFormat != BackendInfo.ArtifactFormat)
		{
			SetWasmtimeError(
				OutError,
				TEXT("artifact_format_mismatch"),
				TEXT("The artifact format does not match the selected Wasmtime backend."));
			return false;
		}
		if (Artifact.ExecutionBytes.IsEmpty()
			|| Artifact.CanonicalWasmBytes.IsEmpty())
		{
			SetWasmtimeError(
				OutError,
				TEXT("invalid_artifact"),
				TEXT("Execution and canonical WASM bytes must both be present."));
			return false;
		}
		const bool bSerialized =
			Artifact.ArtifactFormat == EAvidScriptVmArtifactFormat::WasmtimeSerialized;
		if (!bSerialized
			&& (Artifact.ExecutionBytes.Num() != Artifact.CanonicalWasmBytes.Num()
				|| FMemory::Memcmp(
					Artifact.ExecutionBytes.GetData(),
					Artifact.CanonicalWasmBytes.GetData(),
					Artifact.ExecutionBytes.Num()) != 0))
		{
			SetWasmtimeError(
				OutError,
				TEXT("artifact_identity_mismatch"),
				TEXT("WASM execution bytes must match the canonical bytes that were validated."));
			return false;
		}
		if (bSerialized)
		{
			if (Artifact.Trust != EAvidScriptVmArtifactTrust::VerifiedPackage)
			{
				SetWasmtimeError(
					OutError,
					TEXT("artifact_untrusted"),
					TEXT("Wasmtime serialized modules are accepted only from a verified package manifest."));
				return false;
			}
			if (Artifact.TargetTriple != BackendInfo.TargetTriple)
			{
				SetWasmtimeError(
					OutError,
					TEXT("artifact_target_mismatch"),
					TEXT("The serialized module target triple does not match this backend."));
				return false;
			}
			if (Artifact.ExecutionIdentity !=
					FAvidScriptHash::Sha256Hex(Artifact.ExecutionBytes)
				|| Artifact.CanonicalWasmIdentity !=
					FAvidScriptHash::Sha256Hex(Artifact.CanonicalWasmBytes))
			{
				SetWasmtimeError(
					OutError,
					TEXT("artifact_identity_mismatch"),
					TEXT("The serialized or canonical WASM SHA-256 differs from its package identity."));
				return false;
			}
		}
		if (Config.StackSize == 0 || Config.HeapSize == 0)
		{
			SetWasmtimeError(OutError, TEXT("invalid_config"), TEXT("VM stack and heap sizes must be non-zero."));
			return false;
		}
		FAvidScriptWasmModuleLayout ModuleLayout;
		FString LayoutError;
		if (!InspectAvidScriptWasmModuleLayout(
			Artifact.CanonicalWasmBytes,
			ModuleLayout,
			LayoutError))
		{
			SetWasmtimeError(OutError, TEXT("wasm_layout_invalid"), LayoutError);
			return false;
		}
		TArray<FAvidScriptVmTypedHostImport, TInlineAllocator<16>> SupplementalTypedImports;
		for (const FAvidScriptVmTypedHostImport& Import : Config.TypedHostImports)
		{
			if (Import.bSupplementalRuntimeAuthority)
			{
				SupplementalTypedImports.Add(Import);
			}
		}
		if (!ValidateAvidScriptVmImportContract(
			ModuleLayout,
			Config.BindingPackage,
			TConstArrayView<FAvidScriptVmExpectedImport>(),
			false,
			OutError,
			SupplementalTypedImports))
		{
			return false;
		}

		ModuleId = InModuleId;
		HostDispatcher = Config.HostDispatcher;
		TypedHostDispatcher = Config.TypedHostDispatcher;
		if (!ValidateTypedImports(
			ModuleLayout,
			Config.BindingPackage,
			Config.TypedHostImports,
			OutError))
		{
			PerformUnload();
			return false;
		}

		const double RuntimeInitStart = FPlatformTime::Seconds();
#if PLATFORM_WINDOWS
		FString DllLoadError;
		FString CompilerProfileErrorCategory;
		AvidScriptWasmtimeEngineProfile CompilerProfile = {};
		if (!ResolveAvidScriptWasmtimeCompilerProfile(
				BackendInfo,
				CompilerProfile,
				DllLoadError,
				&CompilerProfileErrorCategory))
		{
			LoadMetrics.RuntimeInitMs = MeasureWasmtimeElapsedMs(RuntimeInitStart);
			SetWasmtimeError(
				OutError,
				CompilerProfileErrorCategory.IsEmpty()
					? TEXT("runtime_init_failed")
					: *CompilerProfileErrorCategory,
				DllLoadError);
			return false;
		}
#else
		if (bSerialized)
		{
			LoadMetrics.RuntimeInitMs = MeasureWasmtimeElapsedMs(RuntimeInitStart);
			SetWasmtimeError(
				OutError,
				TEXT("artifact_target_mismatch"),
				TEXT("Wasmtime serialized artifacts currently support Win64 only."));
			return false;
		}
#endif
		if (bSerialized
			&& Artifact.CompilerBuildIdentity != BackendInfo.RuntimeBuildIdentity)
		{
			LoadMetrics.RuntimeInitMs = MeasureWasmtimeElapsedMs(RuntimeInitStart);
			SetWasmtimeError(
				OutError,
				TEXT("artifact_compiler_mismatch"),
				TEXT("The serialized module compiler identity does not match the active Wasmtime engine."));
			return false;
		}
		Engine = avidscript_wasmtime_engine_new_with_profile(
			&CompilerProfile);
		LoadMetrics.RuntimeInitMs = MeasureWasmtimeElapsedMs(RuntimeInitStart);
		if (Engine == nullptr)
		{
			SetWasmtimeError(
				OutError,
				TEXT("compiler_profile_invalid"),
				TEXT("Wasmtime could not create the controlled Cranelift engine."));
			return false;
		}

		const double ModuleLoadStart = FPlatformTime::Seconds();
		AvidScriptWasmtimeFailure* ModuleFailure = bSerialized
			? avidscript_wasmtime_module_deserialize(
				Engine,
				Artifact.ExecutionBytes.GetData(),
				static_cast<size_t>(Artifact.ExecutionBytes.Num()),
				&Module)
			: avidscript_wasmtime_module_new(
				Engine,
				Artifact.CanonicalWasmBytes.GetData(),
				static_cast<size_t>(Artifact.CanonicalWasmBytes.Num()),
				&Module);
		LoadMetrics.ModuleLoadMs = MeasureWasmtimeElapsedMs(ModuleLoadStart);
		if (ModuleFailure != nullptr || Module == nullptr)
		{
			TArray<FAvidScriptVmStackFrame> Frames;
			bool bWasTrap = false;
			const FString Details = ModuleFailure != nullptr
				? ConsumeWasmtimeFailure(ModuleFailure, Frames, bWasTrap)
				: TEXT("Wasmtime module allocation failed.");
			SetWasmtimeError(OutError, TEXT("load_failed"), Details);
			PerformUnload();
			return false;
		}

		const double StoreCreateStart = FPlatformTime::Seconds();
		Store = avidscript_wasmtime_store_new(Engine);
		LoadMetrics.ExecEnvCreateMs = MeasureWasmtimeElapsedMs(StoreCreateStart);
		if (Store == nullptr)
		{
			SetWasmtimeError(OutError, TEXT("exec_env_failed"), TEXT("Wasmtime could not create a store."));
			PerformUnload();
			return false;
		}

		const double InstantiateStart = FPlatformTime::Seconds();
		Linker = avidscript_wasmtime_linker_new(Engine);
		if (Linker == nullptr)
		{
			LoadMetrics.ModuleInstantiateMs = MeasureWasmtimeElapsedMs(InstantiateStart);
			SetWasmtimeError(OutError, TEXT("instantiate_failed"), TEXT("wasmtime_linker_new returned null."));
			PerformUnload();
			return false;
		}
		if (!DefineStaticImports(OutError))
		{
			LoadMetrics.ModuleInstantiateMs = MeasureWasmtimeElapsedMs(InstantiateStart);
			PerformUnload();
			return false;
		}
		if (!DefineTypedImports(Config.TypedHostImports, OutError))
		{
			LoadMetrics.ModuleInstantiateMs = MeasureWasmtimeElapsedMs(InstantiateStart);
			PerformUnload();
			return false;
		}
		if (Config.BindingPackage != nullptr && !DefineDynamicImports(*Config.BindingPackage, OutError))
		{
			LoadMetrics.ModuleInstantiateMs = MeasureWasmtimeElapsedMs(InstantiateStart);
			PerformUnload();
			return false;
		}

		AvidScriptWasmtimeFailure* InstantiateFailure = avidscript_wasmtime_linker_instantiate(
			Linker,
			Store,
			Module,
			&Instance);
		LoadMetrics.ModuleInstantiateMs = MeasureWasmtimeElapsedMs(InstantiateStart);
		if (InstantiateFailure != nullptr || Instance == nullptr)
		{
			TArray<FAvidScriptVmStackFrame> Frames;
			bool bWasTrap = false;
			const FString Details = InstantiateFailure != nullptr
				? ConsumeWasmtimeFailure(InstantiateFailure, Frames, bWasTrap)
				: TEXT("Wasmtime instance allocation failed.");
			SetWasmtimeError(OutError, TEXT("instantiate_failed"), Details);
			OutError.StackFrames = MoveTemp(Frames);
			PerformUnload();
			return false;
		}
		return true;
#endif
	}

	bool ResolveExport(
		const FString& ExportName,
		FAvidScriptVmExportHandle& OutHandle,
		FAvidScriptVmError& OutError) override
	{
		OutError.Reset();
		OutHandle = {};
#if !AVIDSCRIPT_WITH_WASMTIME
		SetWasmtimeError(OutError, TEXT("backend_unavailable"), TEXT("Wasmtime is unavailable for this target."));
		return false;
#else
		if (!IsLoaded())
		{
			SetWasmtimeError(OutError, TEXT("invalid_state"), TEXT("ResolveExport requires a loaded VM instance."));
			return false;
		}
		if (const uint32* ExistingIndex = ExportNameToIndex.Find(ExportName))
		{
			const FAvidScriptWasmtimeExportEntry& Existing =
				*ExportEntries[*ExistingIndex];
			OutHandle.Slot = *ExistingIndex + 1;
			OutHandle.Generation = Existing.Generation;
			OutHandle.BackendInstanceIdentity = BackendInstanceIdentity;
			return true;
		}

		++ExportLookupCount;
		FTCHARToUTF8 ExportNameUtf8(*ExportName);
		AvidScriptWasmtimeFunction* Function = nullptr;
		uint32 CellCount = 0;
		uint32 ResultCellCount = 0;
		const int ResolveResult = avidscript_wasmtime_instance_resolve_event_export(
			Store,
			Instance,
			ExportNameUtf8.Get(),
			static_cast<size_t>(ExportNameUtf8.Length()),
			&Function,
			&CellCount,
			&ResultCellCount);
		if (ResolveResult == 1)
		{
			SetWasmtimeError(
				OutError,
				TEXT("missing_export"),
				FString::Printf(TEXT("Required export '%s' was not found."), *ExportName));
			return false;
		}
		if (ResolveResult == 2)
		{
			SetWasmtimeError(
				OutError,
				TEXT("invalid_export"),
				FString::Printf(TEXT("Export '%s' is not a function."), *ExportName));
			return false;
		}
		if (ResolveResult == 3)
		{
			SetWasmtimeError(
				OutError,
				TEXT("invalid_arguments"),
				TEXT("Wasmtime exports must use only core numeric ABI parameters and results."));
			return false;
		}
		if (ResolveResult != 0 || Function == nullptr)
		{
			SetWasmtimeError(
				OutError,
				TEXT("invalid_export"),
				TEXT("Wasmtime could not inspect or retain the export function."));
			return false;
		}
		if (CellCount > FAvidScriptVmCallFrame::MaxCells)
		{
			avidscript_wasmtime_function_delete(Function);
			SetWasmtimeError(
				OutError,
				TEXT("invalid_export"),
				TEXT("Wasmtime export parameter cells exceed the fixed VM call-frame capacity."));
			return false;
		}
		if (ResultCellCount > FAvidScriptVmCallResult::MaxCells)
		{
			avidscript_wasmtime_function_delete(Function);
			SetWasmtimeError(
				OutError,
				TEXT("invalid_export"),
				TEXT("Wasmtime export result cells exceed the fixed VM result capacity."));
			return false;
		}

		TUniquePtr<FAvidScriptWasmtimeExportEntry> Entry =
			MakeUnique<FAvidScriptWasmtimeExportEntry>();
		Entry->Name = ExportName;
		Entry->Function = Function;
		Entry->PreparedCallShape =
			avidscript_wasmtime_function_prepared_call_shape(Function);
		Entry->Generation = ExportGeneration;
		Entry->CellCount = CellCount;
		Entry->ResultCellCount = ResultCellCount;
		const uint32 EntryIndex = static_cast<uint32>(ExportEntries.Num());
		const uint32 EntryGeneration = Entry->Generation;
		ExportEntries.Add(MoveTemp(Entry));
		ExportNameToIndex.Add(ExportName, EntryIndex);

		OutHandle.Slot = EntryIndex + 1;
		OutHandle.Generation = EntryGeneration;
		OutHandle.BackendInstanceIdentity = BackendInstanceIdentity;
		return true;
#endif
	}

	bool PrepareExportCall(
		const FAvidScriptVmExportHandle& Handle,
		FAvidScriptVmPreparedExportCall& OutCall,
		FAvidScriptVmError& OutError) override
	{
		OutCall = FAvidScriptVmPreparedExportCall();
		OutError.Reset();
#if !AVIDSCRIPT_WITH_WASMTIME
		SetWasmtimeError(
			OutError,
			TEXT("backend_unavailable"),
			TEXT("Wasmtime is unavailable for this target."));
		return false;
#else
		if (!IsLoaded())
		{
			SetWasmtimeError(
				OutError,
				TEXT("invalid_state"),
				TEXT("PrepareExportCall requires a loaded VM instance."));
			return false;
		}
		if (Handle.BackendInstanceIdentity != BackendInstanceIdentity
			|| Handle.Slot == 0
			|| Handle.Slot > static_cast<uint32>(ExportEntries.Num())
			|| Handle.Generation != ExportGeneration)
		{
			SetWasmtimeError(
				OutError,
				TEXT("stale_export"),
				TEXT("The export handle cannot be prepared for this VM instance."));
			return false;
		}
		FAvidScriptWasmtimeExportEntry* Entry =
			ExportEntries[Handle.Slot - 1].Get();
		if (Entry == nullptr
			|| Entry->Generation != ExportGeneration
			|| Entry->Function == nullptr)
		{
			SetWasmtimeError(
				OutError,
				TEXT("stale_export"),
				TEXT("The resolved Wasmtime export entry is no longer active."));
			return false;
		}
		if (Entry->CellCount > FAvidScriptVmCallFrame::MaxCells
			|| Entry->ResultCellCount > FAvidScriptVmCallResult::MaxCells)
		{
			SetWasmtimeError(
				OutError,
				TEXT("invalid_export"),
				TEXT("The resolved Wasmtime export exceeds the fixed VM call capacity."));
			return false;
		}

		OutCall.Owner = this;
		OutCall.Target = Entry;
		OutCall.InvokeFunction =
			Entry->PreparedCallShape
				== AVIDSCRIPT_WASMTIME_PREPARED_CALL_I32_I32_TO_I32
			? &InvokePreparedI32I32ToI32ExportCall
			: &InvokePreparedExportCall;
		OutCall.ParameterCellCount = Entry->CellCount;
		OutCall.ResultCellCount = Entry->ResultCellCount;
		return true;
#endif
	}

	bool Call(
		const FAvidScriptVmExportHandle& Handle,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult) override
	{
		OutError.Reset();
		if (OutResult != nullptr)
		{
			*OutResult = FAvidScriptVmCallResult();
		}
		bHasPendingHostFailure = false;
		PendingHostImportModuleName.Reset();
		PendingHostImportName.Reset();
		PendingHostFailureDetails.Reset();
		if (Frame.CellCount > FAvidScriptVmCallFrame::MaxCells)
		{
			SetWasmtimeError(OutError, TEXT("invalid_arguments"), TEXT("VM call frame exceeds its fixed cell capacity."));
			return false;
		}
		if (Handle.BackendInstanceIdentity != BackendInstanceIdentity)
		{
			const bool bStale = OwnedBackendInstanceIdentities.Contains(Handle.BackendInstanceIdentity);
			SetWasmtimeError(
				OutError,
				bStale ? TEXT("stale_export") : TEXT("foreign_export"),
				bStale
					? TEXT("The export handle belongs to an unloaded VM instance.")
					: TEXT("The export handle belongs to a different VM backend instance."));
			return false;
		}
#if !AVIDSCRIPT_WITH_WASMTIME
		SetWasmtimeError(OutError, TEXT("backend_unavailable"), TEXT("Wasmtime is unavailable for this target."));
		return false;
#else
		if (!IsLoaded())
		{
			SetWasmtimeError(OutError, TEXT("invalid_state"), TEXT("Call requires a loaded VM instance."));
			return false;
		}
		if (Handle.Slot == 0
			|| Handle.Slot > static_cast<uint32>(ExportEntries.Num())
			|| Handle.Generation != ExportGeneration)
		{
			SetWasmtimeError(OutError, TEXT("stale_export"), TEXT("The export handle is no longer valid."));
			return false;
		}
		const FAvidScriptWasmtimeExportEntry* Entry =
			ExportEntries[Handle.Slot - 1].Get();
		if (Entry == nullptr
			|| Entry->Generation != ExportGeneration
			|| Entry->Function == nullptr)
		{
			SetWasmtimeError(
				OutError,
				TEXT("stale_export"),
				TEXT("The cached Wasmtime export entry is no longer active."));
			return false;
		}
		if (Frame.CellCount != Entry->CellCount)
		{
			SetWasmtimeError(OutError, TEXT("invalid_arguments"), TEXT("VM call frame does not match the cached export signature."));
			return false;
		}
		return InvokeResolvedExport(*Entry, Frame, OutError, OutResult);
#endif
	}

	void Unload() override
	{
		if (ActiveCallDepth > 0)
		{
			bUnloadDeferred = true;
			return;
		}
		PerformUnload();
	}

	bool IsLoaded() const override
	{
#if AVIDSCRIPT_WITH_WASMTIME
		return Engine != nullptr
			&& Store != nullptr
			&& Linker != nullptr
			&& Module != nullptr
			&& Instance != nullptr;
#else
		return false;
#endif
	}

	IAvidScriptVmGuestMemory* GetGuestMemory() override
	{
		return this;
	}

	uint32 GetExportLookupCount() const override
	{
		return ExportLookupCount;
	}

	const FAvidScriptVmLoadMetrics& GetLoadMetrics() const override
	{
		return LoadMetrics;
	}

	bool ReadBytes(uint32 GuestAddress, TArrayView<uint8> OutBytes, FString& OutError) override
	{
		OutError.Reset();
		if (OutBytes.IsEmpty())
		{
			return true;
		}
#if !AVIDSCRIPT_WITH_WASMTIME
		OutError = TEXT("guest_memory_invalid: Wasmtime is unavailable.");
		return false;
#else
		uint8* Data = nullptr;
		size_t MemorySize = 0;
		if (!TryGetGuestMemory(Data, MemorySize))
		{
			OutError = TEXT("guest_memory_invalid: exported memory 'memory' is unavailable.");
			return false;
		}
		const size_t Address = GuestAddress;
		const size_t ByteCount = static_cast<size_t>(OutBytes.Num());
		if (Address > MemorySize || ByteCount > MemorySize - Address)
		{
			OutError = FString::Printf(
				TEXT("guest_memory_invalid: read range at %u for %d bytes is out of bounds."),
				GuestAddress,
				OutBytes.Num());
			return false;
		}
		FMemory::Memcpy(OutBytes.GetData(), Data + Address, ByteCount);
		return true;
#endif
	}

	bool BorrowReadOnlyBytes(
		uint32 GuestAddress,
		uint32 ByteCount,
		uint32 Alignment,
		TConstArrayView<uint8>& OutBytes,
		FString& OutError) override
	{
		OutBytes = TConstArrayView<uint8>();
		OutError.Reset();
#if !AVIDSCRIPT_WITH_WASMTIME
		OutError = TEXT("guest_memory_borrow_unavailable: Wasmtime is unavailable.");
		return false;
#else
		if (Alignment == 0
			|| !FMath::IsPowerOfTwo(Alignment)
			|| GuestAddress % Alignment != 0
			|| ByteCount > static_cast<uint32>(MAX_int32))
		{
			OutError = TEXT("guest_memory_invalid: read-only borrow alignment or size is invalid.");
			return false;
		}
		if (ByteCount == 0)
		{
			return true;
		}
		uint8* Data = nullptr;
		size_t MemorySize = 0;
		if (!TryGetGuestMemory(Data, MemorySize)
			|| static_cast<size_t>(GuestAddress) > MemorySize
			|| static_cast<size_t>(ByteCount) > MemorySize - static_cast<size_t>(GuestAddress))
		{
			OutError = FString::Printf(
				TEXT("guest_memory_invalid: read-only borrow range is invalid at %u for %u bytes."),
				GuestAddress,
				ByteCount);
			return false;
		}
		OutBytes = MakeArrayView(Data + GuestAddress, static_cast<int32>(ByteCount));
		return true;
#endif
	}

	bool BorrowMutableBytes(
		uint32 GuestAddress,
		uint32 ByteCount,
		uint32 Alignment,
		TArrayView<uint8>& OutBytes,
		FString& OutError) override
	{
		OutBytes = TArrayView<uint8>();
		OutError.Reset();
#if !AVIDSCRIPT_WITH_WASMTIME
		OutError = TEXT("guest_memory_borrow_unavailable: Wasmtime is unavailable.");
		return false;
#else
		if (Alignment == 0
			|| !FMath::IsPowerOfTwo(Alignment)
			|| GuestAddress % Alignment != 0
			|| ByteCount > static_cast<uint32>(MAX_int32))
		{
			OutError = TEXT("guest_memory_invalid: mutable borrow alignment or size is invalid.");
			return false;
		}
		if (ByteCount == 0)
		{
			return true;
		}
		uint8* Data = nullptr;
		size_t MemorySize = 0;
		if (!TryGetGuestMemory(Data, MemorySize)
			|| static_cast<size_t>(GuestAddress) > MemorySize
			|| static_cast<size_t>(ByteCount) > MemorySize - static_cast<size_t>(GuestAddress))
		{
			OutError = FString::Printf(
				TEXT("guest_memory_invalid: mutable borrow range is invalid at %u for %u bytes."),
				GuestAddress,
				ByteCount);
			return false;
		}
		OutBytes = MakeArrayView(Data + GuestAddress, static_cast<int32>(ByteCount));
		return true;
#endif
	}

	bool WriteBytes(uint32 GuestAddress, TConstArrayView<uint8> Bytes, FString& OutError) override
	{
		OutError.Reset();
		if (Bytes.IsEmpty())
		{
			return true;
		}
#if !AVIDSCRIPT_WITH_WASMTIME
		OutError = TEXT("guest_memory_invalid: Wasmtime is unavailable.");
		return false;
#else
		uint8* Data = nullptr;
		size_t MemorySize = 0;
		if (!TryGetGuestMemory(Data, MemorySize))
		{
			OutError = TEXT("guest_memory_invalid: exported memory 'memory' is unavailable.");
			return false;
		}
		const size_t Address = GuestAddress;
		const size_t ByteCount = static_cast<size_t>(Bytes.Num());
		if (Address > MemorySize || ByteCount > MemorySize - Address)
		{
			OutError = FString::Printf(
				TEXT("guest_memory_invalid: write range at %u for %d bytes is out of bounds."),
				GuestAddress,
				Bytes.Num());
			return false;
		}
		FMemory::Memcpy(Data + Address, Bytes.GetData(), ByteCount);
		return true;
#endif
	}

#if AVIDSCRIPT_WITH_WASMTIME
	bool InvokeStaticHostImport(
		FAvidScriptWasmtimeHostContext& HostContext,
		AvidScriptWasmtimeCaller* Caller,
		const AvidScriptWasmtimeValue* Arguments,
		size_t ArgumentCount,
		AvidScriptWasmtimeValue* Results,
		size_t ResultCount)
	{
		if (ArgumentCount != static_cast<size_t>(HostContext.Signature.Parameters.Num())
			|| ResultCount != (HostContext.Signature.bHasResult ? 1u : 0u))
		{
			RecordPendingHostFailure(
				TEXT("avidscript"),
				UTF8_TO_TCHAR(HostContext.Import->ImportName),
				TEXT("Wasmtime host callback shape does not match the static import catalog."));
			return false;
		}
		TArray<FAvidScriptVmStaticValue, TInlineAllocator<8>> StaticArguments;
		StaticArguments.Reserve(static_cast<int32>(ArgumentCount));
		for (size_t Index = 0; Index < ArgumentCount; ++Index)
		{
			const EAvidScriptVmValueKind ExpectedKind =
				HostContext.Signature.Parameters[static_cast<int32>(Index)];
			if (Arguments[Index].kind != ToWasmtimeValueKind(ExpectedKind))
			{
				RecordPendingHostFailure(
					TEXT("avidscript"),
					UTF8_TO_TCHAR(HostContext.Import->ImportName),
					TEXT("Wasmtime host callback value kind does not match the static import catalog."));
				return false;
			}
			FAvidScriptVmStaticValue& Value = StaticArguments.AddDefaulted_GetRef();
			Value.Kind = ExpectedKind;
			switch (Value.Kind)
			{
			case EAvidScriptVmValueKind::I32:
				Value.I32 = Arguments[Index].of.i32;
				break;
			case EAvidScriptVmValueKind::I64:
				Value.I64 = Arguments[Index].of.i64;
				break;
			case EAvidScriptVmValueKind::F32:
				Value.F32 = Arguments[Index].of.f32;
				break;
			case EAvidScriptVmValueKind::F64:
				Value.F64 = Arguments[Index].of.f64;
				break;
			}
		}

		AvidScriptWasmtimeCaller* PreviousCaller = ActiveCaller;
		ActiveCaller = Caller;
		FAvidScriptVmStaticCallResult StaticResult;
		FString FailureDetails;
		const bool bSucceeded = InvokeAvidScriptVmStaticHostImport(
			*HostContext.Import,
			HostContext.Signature,
			StaticArguments,
			HostDispatcher,
			*this,
			StaticResult,
			FailureDetails);
		ActiveCaller = PreviousCaller;
		if (!bSucceeded)
		{
			RecordPendingHostFailure(
				TEXT("avidscript"),
				UTF8_TO_TCHAR(HostContext.Import->ImportName),
				FailureDetails.IsEmpty()
					? TEXT("Wasmtime static host import adapter rejected the call.")
					: MoveTemp(FailureDetails));
			return false;
		}

		if (ResultCount == 1)
		{
			switch (StaticResult.Kind)
			{
			case EAvidScriptVmValueKind::I32:
				Results[0].kind = AVIDSCRIPT_WASMTIME_I32;
				Results[0].of.i32 = StaticResult.I32;
				break;
			case EAvidScriptVmValueKind::I64:
				Results[0].kind = AVIDSCRIPT_WASMTIME_I64;
				Results[0].of.i64 = StaticResult.I64;
				break;
			case EAvidScriptVmValueKind::F32:
				Results[0].kind = AVIDSCRIPT_WASMTIME_F32;
				Results[0].of.f32 = StaticResult.F32;
				break;
			case EAvidScriptVmValueKind::F64:
				Results[0].kind = AVIDSCRIPT_WASMTIME_F64;
				Results[0].of.f64 = StaticResult.F64;
				break;
			}
		}
		return true;
	}

	bool InvokeDynamicHostImport(
		FAvidScriptWasmtimeDynamicHostContext& HostContext,
		AvidScriptWasmtimeCaller* Caller,
		const AvidScriptWasmtimeValue* Arguments,
		size_t ArgumentCount,
		AvidScriptWasmtimeValue* Results,
		size_t ResultCount)
	{
		const size_t ExpectedResultCount = HostContext.Signature.bHasResult ? 1u : 0u;
		if (ArgumentCount != static_cast<size_t>(HostContext.Signature.Parameters.Num())
			|| ResultCount != ExpectedResultCount)
		{
			RecordPendingHostFailure(
				HostContext.ModuleName,
				HostContext.ImportName,
				TEXT("Wasmtime dynamic host callback shape does not match its binding package."));
			return false;
		}

		uint64 ArgumentCells[64];
		for (size_t Index = 0; Index < ArgumentCount; ++Index)
		{
			const EAvidScriptVmValueKind ExpectedKind =
				HostContext.Signature.Parameters[static_cast<int32>(Index)];
			if (Arguments[Index].kind != ToWasmtimeValueKind(ExpectedKind))
			{
				RecordPendingHostFailure(
					HostContext.ModuleName,
					HostContext.ImportName,
					TEXT("Wasmtime dynamic host callback value kind does not match its binding package."));
				return false;
			}
			switch (ExpectedKind)
			{
			case EAvidScriptVmValueKind::I32:
				ArgumentCells[Index] = static_cast<uint32>(Arguments[Index].of.i32);
				break;
			case EAvidScriptVmValueKind::I64:
				ArgumentCells[Index] = static_cast<uint64>(Arguments[Index].of.i64);
				break;
			case EAvidScriptVmValueKind::F32:
			{
				uint32 Bits = 0;
				FMemory::Memcpy(&Bits, &Arguments[Index].of.f32, sizeof(Bits));
				ArgumentCells[Index] = Bits;
				break;
			}
			case EAvidScriptVmValueKind::F64:
				FMemory::Memcpy(&ArgumentCells[Index], &Arguments[Index].of.f64, sizeof(uint64));
				break;
			}
		}

		const bool bHasPreparedTarget = HostContext.PreparedTarget.IsBound();
		if (!bHasPreparedTarget && HostDispatcher == nullptr)
		{
			RecordPendingHostFailure(
				HostContext.ModuleName,
				HostContext.ImportName,
				TEXT("No host dispatcher is attached to the VM instance."));
			return false;
		}

		const TConstArrayView<uint64> ArgumentView = MakeArrayView(
			ArgumentCells,
			static_cast<int32>(ArgumentCount));
		FAvidScriptDynamicHostCallResult Result;
		TGuardValue<AvidScriptWasmtimeCaller*> CallerGuard(ActiveCaller, Caller);
		bool bSucceeded = false;
		if (bHasPreparedTarget)
		{
			bSucceeded = HostContext.PreparedTarget.Invoke(
				HostContext.PreparedTarget.Context,
				ArgumentView,
				*this,
				Result) && Result.bSucceeded;
		}
		else
		{
			FAvidScriptDynamicHostCall Call;
			Call.BindingOrdinal = HostContext.Ordinal;
			Call.Arguments = ArgumentView;
			Call.GuestMemory = this;
			bSucceeded =
				HostDispatcher->DispatchDynamicHostCall(Call, Result)
				&& Result.bSucceeded;
		}
		if (!bSucceeded)
		{
			RecordPendingHostFailure(
				HostContext.ModuleName,
				HostContext.ImportName,
				Result.Details.IsEmpty()
					? bHasPreparedTarget
						? TEXT("Wasmtime prepared dynamic host target rejected the call.")
						: TEXT("Wasmtime dynamic host dispatcher rejected the call.")
					: MoveTemp(Result.Details));
			return false;
		}

		if (ResultCount == 1)
		{
			if (HostContext.Signature.Result == EAvidScriptVmValueKind::I64)
			{
				Results[0].kind = AVIDSCRIPT_WASMTIME_I64;
				Results[0].of.i64 = Result.ReturnValueI64;
			}
			else
			{
				Results[0].kind = AVIDSCRIPT_WASMTIME_I32;
				Results[0].of.i32 = Result.ReturnValue;
			}
		}
		return true;
	}

	int32 InvokeTypedEmptyI32(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		int32& OutValue)
	{
		if (TypedHostDispatcher == nullptr)
		{
			RecordPendingHostFailure(
				HostContext.ModuleName,
				HostContext.ImportName,
				TEXT("The typed host dispatcher is unavailable."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			TypedHostDispatcher->DispatchEmptyI32(HostContext.BindingOrdinal, OutValue);
		if (Status == EAvidScriptVmTypedHostStatus::Succeeded)
		{
			return 0;
		}
		RecordPendingHostFailure(
			HostContext.ModuleName,
			HostContext.ImportName,
			Status == EAvidScriptVmTypedHostStatus::FallbackRequired
				? TEXT("The typed host import requires semantic fallback, but its dedicated ABI cannot fall back in place.")
				: TEXT("The typed host dispatcher rejected the call."));
		return 1;
	}

	int32 InvokeTypedI32Pair(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		int32 Left,
		int32 Right,
		int32& OutValue)
	{
		if (TypedHostDispatcher == nullptr)
		{
			RecordPendingHostFailure(
				HostContext.ModuleName,
				HostContext.ImportName,
				TEXT("The typed host dispatcher is unavailable."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			TypedHostDispatcher->DispatchI32PairToI32(
				HostContext.BindingOrdinal,
				Left,
				Right,
				OutValue);
		if (Status == EAvidScriptVmTypedHostStatus::Succeeded)
		{
			return 0;
		}
		RecordPendingHostFailure(
			HostContext.ModuleName,
			HostContext.ImportName,
			Status == EAvidScriptVmTypedHostStatus::FallbackRequired
				? TEXT("The typed host import requires semantic fallback, but its dedicated ABI cannot fall back in place.")
				: TEXT("The typed host dispatcher rejected the call."));
		return 1;
	}

	int32 InvokeTypedSelfI32Pair(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32& OutValue)
	{
		if (HostContext.PreparedTarget.IsBoundForShape(HostContext.Shape))
		{
			const EAvidScriptVmTypedHostStatus Status =
				HostContext.PreparedTarget.SelfI32Pair(
					HostContext.PreparedTarget.Context,
					SelfSlot,
					SelfGeneration,
					Left,
					Right,
					OutValue);
			return CompleteTypedInvocation(HostContext, Status);
		}
		if (TypedHostDispatcher == nullptr)
		{
			RecordPendingHostFailure(HostContext.ModuleName, HostContext.ImportName, TEXT("The typed host dispatcher is unavailable."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status = TypedHostDispatcher->DispatchSelfI32PairToI32(
			HostContext.BindingOrdinal, SelfSlot, SelfGeneration, Left, Right, OutValue);
		return CompleteTypedInvocation(HostContext, Status);
	}

	int32 InvokeTypedSelfI32PairGuestResult(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 Left,
		const int32 Right,
		const int32 GuestAddress,
		int32& OutStatus)
	{
		if (!HostContext.PreparedTarget.IsBoundForShape(HostContext.Shape))
		{
			RecordPendingHostFailure(
				HostContext.ModuleName,
				HostContext.ImportName,
				TEXT("The typed guest-result import has no prepared target."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext.PreparedTarget.SelfI32PairGuestResult(
				HostContext.PreparedTarget.Context,
				SelfSlot,
				SelfGeneration,
				Left,
				Right,
				GuestAddress,
				OutStatus);
		return CompleteTypedInvocation(HostContext, Status);
	}

	int32 InvokeTypedSelfF32TripleGuestVector(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const float X,
		const float Y,
		const float Z,
		const int32 GuestAddress,
		int32& OutStatus)
	{
		if (!HostContext.PreparedTarget.IsBoundForShape(HostContext.Shape))
		{
			RecordPendingHostFailure(
				HostContext.ModuleName,
				HostContext.ImportName,
				TEXT("The typed FVector guest-result import has no prepared target."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext.PreparedTarget.SelfF32TripleGuestVector(
				HostContext.PreparedTarget.Context,
				SelfSlot,
				SelfGeneration,
				X,
				Y,
				Z,
				GuestAddress,
				OutStatus);
		return CompleteTypedInvocation(HostContext, Status);
	}

	int32 InvokeTypedSelfPropertyI32Get(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		int32& OutValue)
	{
		if (!HostContext.PreparedTarget.IsBoundForShape(HostContext.Shape))
		{
			RecordPendingHostFailure(
				HostContext.ModuleName,
				HostContext.ImportName,
				TEXT("The split property getter has no prepared target."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext.PreparedTarget.SelfPropertyI32Get(
				HostContext.PreparedTarget.Context,
				SelfSlot,
				SelfGeneration,
				OutValue);
		return CompleteTypedInvocation(HostContext, Status);
	}

	int32 InvokeTypedSelfPropertyI32Set(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 Value,
		int32& OutValue)
	{
		OutValue = 0;
		if (!HostContext.PreparedTarget.IsBoundForShape(HostContext.Shape))
		{
			RecordPendingHostFailure(
				HostContext.ModuleName,
				HostContext.ImportName,
				TEXT("The split property setter has no prepared target."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext.PreparedTarget.SelfPropertyI32Set(
				HostContext.PreparedTarget.Context,
				SelfSlot,
				SelfGeneration,
				Value);
		if (Status == EAvidScriptVmTypedHostStatus::Succeeded)
		{
			OutValue = 1;
		}
		return CompleteTypedInvocation(HostContext, Status);
	}

	int32 InvokeTypedSelfPropertyI32GetSet(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue)
	{
		if (HostContext.PreparedTarget.IsBoundForShape(HostContext.Shape))
		{
			const EAvidScriptVmTypedHostStatus Status =
				HostContext.PreparedTarget.SelfGuestAddress(
					HostContext.PreparedTarget.Context,
					SelfSlot,
					SelfGeneration,
					GuestAddress,
					OutValue);
			return CompleteTypedInvocation(HostContext, Status);
		}
		if (TypedHostDispatcher == nullptr)
		{
			RecordPendingHostFailure(HostContext.ModuleName, HostContext.ImportName, TEXT("The typed host dispatcher is unavailable."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status = TypedHostDispatcher->DispatchSelfPropertyI32GetSet(
			HostContext.BindingOrdinal, SelfSlot, SelfGeneration, GuestAddress, OutValue);
		return CompleteTypedInvocation(HostContext, Status);
	}

	int32 InvokeTypedSelfVectorValue(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32& OutValue)
	{
		if (HostContext.PreparedTarget.IsBoundForShape(HostContext.Shape))
		{
			const EAvidScriptVmTypedHostStatus Status =
				HostContext.PreparedTarget.SelfGuestAddress(
					HostContext.PreparedTarget.Context,
					SelfSlot,
					SelfGeneration,
					GuestAddress,
					OutValue);
			return CompleteTypedInvocation(HostContext, Status);
		}
		if (TypedHostDispatcher == nullptr)
		{
			RecordPendingHostFailure(HostContext.ModuleName, HostContext.ImportName, TEXT("The typed host dispatcher is unavailable."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status = TypedHostDispatcher->DispatchSelfVectorValue(
			HostContext.BindingOrdinal, SelfSlot, SelfGeneration, GuestAddress, OutValue);
		return CompleteTypedInvocation(HostContext, Status);
	}

	int32 InvokeTypedStableObjectRoundtrip(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 ObjectSlot,
		int32 ObjectGeneration,
		int32 GuestAddress,
		int32& OutValue)
	{
		if (HostContext.PreparedTarget.IsBoundForShape(HostContext.Shape))
		{
			const EAvidScriptVmTypedHostStatus Status =
				HostContext.PreparedTarget.StableObjectRoundtrip(
					HostContext.PreparedTarget.Context,
					SelfSlot,
					SelfGeneration,
					ObjectSlot,
					ObjectGeneration,
					GuestAddress,
					OutValue);
			return CompleteTypedInvocation(HostContext, Status);
		}
		if (TypedHostDispatcher == nullptr)
		{
			RecordPendingHostFailure(HostContext.ModuleName, HostContext.ImportName, TEXT("The typed host dispatcher is unavailable."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status = TypedHostDispatcher->DispatchStableObjectRoundtrip(
			HostContext.BindingOrdinal,
			SelfSlot,
			SelfGeneration,
			ObjectSlot,
			ObjectGeneration,
			GuestAddress,
			OutValue);
		return CompleteTypedInvocation(HostContext, Status);
	}

	int32 InvokeTypedCommandBufferSubmit(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		int32 GuestAddress,
		int32 ByteCount,
		int32& OutValue)
	{
		if (TypedHostDispatcher == nullptr)
		{
			RecordPendingHostFailure(HostContext.ModuleName, HostContext.ImportName, TEXT("The typed host dispatcher is unavailable."));
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status = TypedHostDispatcher->DispatchCommandBufferSubmit(
			HostContext.BindingOrdinal, GuestAddress, ByteCount, OutValue);
		return CompleteTypedInvocation(HostContext, Status);
	}
#endif

private:
#if AVIDSCRIPT_WITH_WASMTIME
	static bool InvokePreparedExportCall(
		void* Owner,
		void* Target,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult)
	{
		FAvidScriptWasmtimeBackend* Backend =
			static_cast<FAvidScriptWasmtimeBackend*>(Owner);
		FAvidScriptWasmtimeExportEntry* Entry =
			static_cast<FAvidScriptWasmtimeExportEntry*>(Target);
		if (Backend == nullptr || Entry == nullptr)
		{
			OutError.Reset();
			OutError.Category = TEXT("prepared_export_invalid");
			OutError.Details =
				TEXT("The prepared Wasmtime export target is invalid.");
			if (OutResult != nullptr)
			{
				*OutResult = FAvidScriptVmCallResult();
			}
			return false;
		}
		return Backend->CallPreparedExport(
			*Entry,
			Frame,
			OutError,
			OutResult);
	}

	static bool InvokePreparedI32I32ToI32ExportCall(
		void* Owner,
		void* Target,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult)
	{
		FAvidScriptWasmtimeBackend* Backend =
			static_cast<FAvidScriptWasmtimeBackend*>(Owner);
		FAvidScriptWasmtimeExportEntry* Entry =
			static_cast<FAvidScriptWasmtimeExportEntry*>(Target);
		if (Backend == nullptr || Entry == nullptr)
		{
			OutError.Reset();
			OutError.Category = TEXT("prepared_export_invalid");
			OutError.Details =
				TEXT("The prepared Wasmtime export target is invalid.");
			if (OutResult != nullptr)
			{
				*OutResult = FAvidScriptVmCallResult();
			}
			return false;
		}
		return Backend->CallPreparedI32I32ToI32Export(
			*Entry,
			Frame,
			OutError,
			OutResult);
	}

	void ResetCallState(
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult)
	{
		OutError.Reset();
		if (OutResult != nullptr)
		{
			*OutResult = FAvidScriptVmCallResult();
		}
		bHasPendingHostFailure = false;
		PendingHostImportModuleName.Reset();
		PendingHostImportName.Reset();
		PendingHostFailureDetails.Reset();
	}

	bool CallPreparedExport(
		const FAvidScriptWasmtimeExportEntry& Entry,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult)
	{
		ResetCallState(OutError, OutResult);
		if (!IsLoaded()
			|| Entry.Generation != ExportGeneration
			|| Entry.Function == nullptr)
		{
			SetWasmtimeError(
				OutError,
				TEXT("stale_export"),
				TEXT("The prepared Wasmtime export is no longer active."));
			return false;
		}
		return InvokeResolvedExport(Entry, Frame, OutError, OutResult);
	}

	bool CallPreparedI32I32ToI32Export(
		const FAvidScriptWasmtimeExportEntry& Entry,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult)
	{
		ResetCallState(OutError, OutResult);
		if (Entry.Generation != ExportGeneration
			|| Entry.Function == nullptr)
		{
			SetWasmtimeError(
				OutError,
				TEXT("stale_export"),
				TEXT("The prepared Wasmtime export is no longer active."));
			return false;
		}

		++ActiveCallDepth;
		int32 Result = 0;
		AvidScriptWasmtimeFailure* CallFailure = nullptr;
		const AvidScriptWasmtimeCallStatus CallStatus =
			avidscript_wasmtime_function_call_i32_i32_to_i32_prepared_unchecked(
				Store,
				Entry.Function,
				static_cast<int32>(Frame.Cells[0]),
				static_cast<int32>(Frame.Cells[1]),
				&Result,
				&CallFailure);
		const uint32 ResultCell = static_cast<uint32>(Result);
		return CompleteResolvedExportCall(
			Entry,
			CallStatus,
			CallFailure,
			&ResultCell,
			CallStatus == AVIDSCRIPT_WASMTIME_CALL_SUCCESS ? 1 : 0,
			OutError,
			OutResult);
	}

	bool InvokeResolvedExport(
		const FAvidScriptWasmtimeExportEntry& Entry,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult)
	{
		++ActiveCallDepth;
		uint32 ResultCells[FAvidScriptVmCallResult::MaxCells] = {};
		size_t ResultCellCount = 0;
		AvidScriptWasmtimeFailure* CallFailure = nullptr;
		const AvidScriptWasmtimeCallStatus CallStatus =
			avidscript_wasmtime_function_call_event_prepared(
				Store,
				Entry.Function,
				Frame.Cells,
				Entry.ResultCellCount == 0 ? nullptr : ResultCells,
				FAvidScriptVmCallResult::MaxCells,
				&ResultCellCount,
				&CallFailure);
		return CompleteResolvedExportCall(
			Entry,
			CallStatus,
			CallFailure,
			ResultCells,
			ResultCellCount,
			OutError,
			OutResult);
	}

	bool CompleteResolvedExportCall(
		const FAvidScriptWasmtimeExportEntry& Entry,
		const AvidScriptWasmtimeCallStatus CallStatus,
		AvidScriptWasmtimeFailure* CallFailure,
		const uint32* ResultCells,
		const size_t ResultCellCount,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult)
	{
		const bool bCallFailed =
			CallStatus != AVIDSCRIPT_WASMTIME_CALL_SUCCESS;
		const bool bUnloadRequestedDuringCall = bUnloadDeferred;
		FString FailureDetails;
		TArray<FAvidScriptVmStackFrame> StackFrames;
		bool bWasTrap = false;
		if (CallFailure != nullptr)
		{
			FailureDetails =
				ConsumeWasmtimeFailure(CallFailure, StackFrames, bWasTrap);
		}
		else if (bCallFailed)
		{
			FailureDetails =
				CallStatus == AVIDSCRIPT_WASMTIME_CALL_LOCAL_FAILURE
					? TEXT("Wasmtime rejected the local call ABI.")
					: TEXT("Wasmtime failed without allocating a diagnostic.");
		}

		--ActiveCallDepth;
		if (ActiveCallDepth == 0 && bUnloadDeferred)
		{
			PerformUnload();
		}
		if (bUnloadRequestedDuringCall)
		{
			SetWasmtimeError(
				OutError,
				TEXT("reentrant_unload"),
				TEXT("VM unload was deferred until the active guest call unwound."));
			return false;
		}
		if (bCallFailed)
		{
			if (bHasPendingHostFailure)
			{
				OutError.Reset();
				OutError.Category = TEXT("host_import_failed");
				OutError.ImportModuleName = PendingHostImportModuleName;
				OutError.ImportName = PendingHostImportName;
				OutError.Details = PendingHostFailureDetails;
			}
			else
			{
				SetWasmtimeError(
					OutError,
					bWasTrap ? TEXT("trap") : TEXT("invalid_arguments"),
					FailureDetails.IsEmpty()
						? TEXT("Wasmtime call failed without a diagnostic message.")
						: FailureDetails);
			}
			OutError.StackFrames = MoveTemp(StackFrames);
			return false;
		}
		if (ResultCellCount != Entry.ResultCellCount)
		{
			SetWasmtimeError(
				OutError,
				TEXT("invalid_result"),
				TEXT("Wasmtime returned a result cell count that differs from the resolved export ABI."));
			return false;
		}
		if (OutResult != nullptr)
		{
			if (ResultCellCount > 0)
			{
				FMemory::Memcpy(
					OutResult->Cells,
					ResultCells,
					ResultCellCount * sizeof(uint32));
			}
			OutResult->CellCount = static_cast<uint32>(ResultCellCount);
		}
		return true;
	}

	int32 CompleteTypedInvocation(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		EAvidScriptVmTypedHostStatus Status)
	{
		if (Status == EAvidScriptVmTypedHostStatus::Succeeded)
		{
			return 0;
		}
		RecordPendingHostFailure(
			HostContext.ModuleName,
			HostContext.ImportName,
			Status == EAvidScriptVmTypedHostStatus::FallbackRequired
				? TEXT("The typed host import requires semantic fallback, but its dedicated ABI cannot fall back in place.")
				: TEXT("The typed host dispatcher rejected the call."));
		return 1;
	}

	static FORCEINLINE int32 CompletePreparedTypedInvocation(
		FAvidScriptWasmtimeTypedHostContext& HostContext,
		const EAvidScriptVmTypedHostStatus Status)
	{
		if (Status == EAvidScriptVmTypedHostStatus::Succeeded)
		{
			return 0;
		}
		return HostContext.Backend != nullptr
			? HostContext.Backend->CompleteTypedInvocation(
				HostContext,
				Status)
			: 1;
	}

	void RecordPendingHostFailure(
		const FString& ModuleName,
		const FString& ImportName,
		FString Details)
	{
		bHasPendingHostFailure = true;
		PendingHostImportModuleName = ModuleName;
		PendingHostImportName = ImportName;
		PendingHostFailureDetails = MoveTemp(Details);
	}

	static bool StaticHostCallback(
		void* Environment,
		AvidScriptWasmtimeCaller* Caller,
		const AvidScriptWasmtimeValue* Arguments,
		size_t ArgumentCount,
		AvidScriptWasmtimeValue* Results,
		size_t ResultCount)
	{
		FAvidScriptWasmtimeHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeHostContext*>(Environment);
		if (HostContext == nullptr || HostContext->Backend == nullptr || HostContext->Import == nullptr)
		{
			return false;
		}
		return HostContext->Backend->InvokeStaticHostImport(
			*HostContext,
			Caller,
			Arguments,
			ArgumentCount,
			Results,
			ResultCount);
	}

	static bool DynamicHostCallback(
		void* Environment,
		AvidScriptWasmtimeCaller* Caller,
		const AvidScriptWasmtimeValue* Arguments,
		size_t ArgumentCount,
		AvidScriptWasmtimeValue* Results,
		size_t ResultCount)
	{
		FAvidScriptWasmtimeDynamicHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeDynamicHostContext*>(Environment);
		if (HostContext == nullptr || HostContext->Backend == nullptr)
		{
			return false;
		}
		return HostContext->Backend->InvokeDynamicHostImport(
			*HostContext,
			Caller,
			Arguments,
			ArgumentCount,
			Results,
			ResultCount);
	}

	static int32 TypedEmptyI32Callback(void* Environment, int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || HostContext->Backend == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		return HostContext->Backend->InvokeTypedEmptyI32(*HostContext, *OutValue);
	}

	static int32 TypedI32PairCallback(
		void* Environment,
		int32 Left,
		int32 Right,
		int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || HostContext->Backend == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		return HostContext->Backend->InvokeTypedI32Pair(
			*HostContext,
			Left,
			Right,
			*OutValue);
	}

	static int32 TypedSelfI32PairCallback(
		void* Environment,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 Left,
		int32 Right,
		int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		if (HostContext->PreparedTarget.SelfI32Pair != nullptr
			&& HostContext->PreparedTarget.Context != nullptr)
		{
			const EAvidScriptVmTypedHostStatus Status =
				HostContext->PreparedTarget.SelfI32Pair(
					HostContext->PreparedTarget.Context,
					SelfSlot,
					SelfGeneration,
					Left,
					Right,
					*OutValue);
			return CompletePreparedTypedInvocation(
				*HostContext,
				Status);
		}
		if (HostContext->Backend == nullptr)
		{
			return 1;
		}
		return HostContext->Backend->InvokeTypedSelfI32Pair(
			*HostContext,
			SelfSlot,
			SelfGeneration,
			Left,
			Right,
			*OutValue);
	}

	static int32 TypedSelfI32PairGuestResultCallback(
		void* Environment,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 Left,
		const int32 Right,
		const int32 GuestAddress,
		int32* OutStatus)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr
			|| HostContext->Backend == nullptr
			|| OutStatus == nullptr)
		{
			return 1;
		}
		return HostContext->Backend->InvokeTypedSelfI32PairGuestResult(
			*HostContext,
			SelfSlot,
			SelfGeneration,
			Left,
			Right,
			GuestAddress,
			*OutStatus);
	}

	static int32 TypedSelfF32TripleGuestVectorCallback(
		void* Environment,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const float X,
		const float Y,
		const float Z,
		const int32 GuestAddress,
		int32* OutStatus)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr
			|| HostContext->Backend == nullptr
			|| OutStatus == nullptr)
		{
			return 1;
		}
		return HostContext->Backend->InvokeTypedSelfF32TripleGuestVector(
			*HostContext,
			SelfSlot,
			SelfGeneration,
			X,
			Y,
			Z,
			GuestAddress,
			*OutStatus);
	}

	static int32 TypedSelfPropertyI32GetSetCallback(
		void* Environment,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext = static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || HostContext->Backend == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		return HostContext->Backend->InvokeTypedSelfPropertyI32GetSet(
			*HostContext, SelfSlot, SelfGeneration, GuestAddress, *OutValue);
	}

	static int32 TypedSelfPropertyI32GetCallback(
		void* Environment,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		const FAvidScriptVmPreparedSelfPropertyI32GetTarget Target =
			HostContext->PreparedTarget.SelfPropertyI32Get;
		if (Target == nullptr
			|| HostContext->PreparedTarget.Context == nullptr)
		{
			return HostContext->Backend != nullptr
				? HostContext->Backend->InvokeTypedSelfPropertyI32Get(
					*HostContext,
					SelfSlot,
					SelfGeneration,
					*OutValue)
				: 1;
		}
		const EAvidScriptVmTypedHostStatus Status = Target(
			HostContext->PreparedTarget.Context,
			SelfSlot,
			SelfGeneration,
			*OutValue);
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedSelfPropertyI32SetCallback(
		void* Environment,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 Value,
		int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		*OutValue = 0;
		const FAvidScriptVmPreparedSelfPropertyI32SetTarget Target =
			HostContext->PreparedTarget.SelfPropertyI32Set;
		if (Target == nullptr
			|| HostContext->PreparedTarget.Context == nullptr)
		{
			return HostContext->Backend != nullptr
				? HostContext->Backend->InvokeTypedSelfPropertyI32Set(
					*HostContext,
					SelfSlot,
					SelfGeneration,
					Value,
					*OutValue)
				: 1;
		}
		const EAvidScriptVmTypedHostStatus Status = Target(
			HostContext->PreparedTarget.Context,
			SelfSlot,
			SelfGeneration,
			Value);
		if (Status == EAvidScriptVmTypedHostStatus::Succeeded)
		{
			*OutValue = 1;
		}
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedPackedSelfPropertyF32GetCallback(
		void* Environment,
		const int64 PackedSelf,
		float* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || OutValue == nullptr
			|| HostContext->PreparedTarget.Context == nullptr
			|| HostContext->PreparedTarget.PackedSelfPropertyF32Get == nullptr)
		{
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext->PreparedTarget.PackedSelfPropertyF32Get(
				HostContext->PreparedTarget.Context,
				PackedSelf,
				*OutValue);
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedPackedSelfPropertyF32SetCallback(
		void* Environment,
		const int64 PackedSelf,
		const float Value)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr
			|| HostContext->PreparedTarget.Context == nullptr
			|| HostContext->PreparedTarget.PackedSelfPropertyF32Set == nullptr)
		{
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext->PreparedTarget.PackedSelfPropertyF32Set(
				HostContext->PreparedTarget.Context,
				PackedSelf,
				Value);
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedPackedSelfPropertyI32GetCallback(
		void* Environment,
		const int64 PackedSelf,
		int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || OutValue == nullptr
			|| HostContext->PreparedTarget.Context == nullptr
			|| HostContext->PreparedTarget.PackedSelfPropertyI32Get == nullptr)
		{
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext->PreparedTarget.PackedSelfPropertyI32Get(
				HostContext->PreparedTarget.Context,
				PackedSelf,
				*OutValue);
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedPackedSelfPropertyI32SetCallback(
		void* Environment,
		const int64 PackedSelf,
		const int32 Value)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr
			|| HostContext->PreparedTarget.Context == nullptr
			|| HostContext->PreparedTarget.PackedSelfPropertyI32Set == nullptr)
		{
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext->PreparedTarget.PackedSelfPropertyI32Set(
				HostContext->PreparedTarget.Context,
				PackedSelf,
				Value);
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedPackedSelfPropertyI64GetCallback(
		void* Environment,
		const int64 PackedSelf,
		int64* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || OutValue == nullptr
			|| HostContext->PreparedTarget.Context == nullptr
			|| HostContext->PreparedTarget.PackedSelfPropertyI64Get == nullptr)
		{
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext->PreparedTarget.PackedSelfPropertyI64Get(
				HostContext->PreparedTarget.Context,
				PackedSelf,
				*OutValue);
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedPackedSelfPropertyI64SetCallback(
		void* Environment,
		const int64 PackedSelf,
		const int64 Value)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr
			|| HostContext->PreparedTarget.Context == nullptr
			|| HostContext->PreparedTarget.PackedSelfPropertyI64Set == nullptr)
		{
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext->PreparedTarget.PackedSelfPropertyI64Set(
				HostContext->PreparedTarget.Context,
				PackedSelf,
				Value);
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedPackedSelfPropertyF64GetCallback(
		void* Environment,
		const int64 PackedSelf,
		double* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || OutValue == nullptr
			|| HostContext->PreparedTarget.Context == nullptr
			|| HostContext->PreparedTarget.PackedSelfPropertyF64Get == nullptr)
		{
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext->PreparedTarget.PackedSelfPropertyF64Get(
				HostContext->PreparedTarget.Context,
				PackedSelf,
				*OutValue);
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedPackedSelfPropertyF64SetCallback(
		void* Environment,
		const int64 PackedSelf,
		const double Value)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext =
			static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr
			|| HostContext->PreparedTarget.Context == nullptr
			|| HostContext->PreparedTarget.PackedSelfPropertyF64Set == nullptr)
		{
			return 1;
		}
		const EAvidScriptVmTypedHostStatus Status =
			HostContext->PreparedTarget.PackedSelfPropertyF64Set(
				HostContext->PreparedTarget.Context,
				PackedSelf,
				Value);
		return CompletePreparedTypedInvocation(*HostContext, Status);
	}

	static int32 TypedSelfVectorValueCallback(
		void* Environment,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 GuestAddress,
		int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext = static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || HostContext->Backend == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		return HostContext->Backend->InvokeTypedSelfVectorValue(
			*HostContext, SelfSlot, SelfGeneration, GuestAddress, *OutValue);
	}

	static int32 TypedStableObjectRoundtripCallback(
		void* Environment,
		int32 SelfSlot,
		int32 SelfGeneration,
		int32 ObjectSlot,
		int32 ObjectGeneration,
		int32 GuestAddress,
		int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext = static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || HostContext->Backend == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		return HostContext->Backend->InvokeTypedStableObjectRoundtrip(
			*HostContext,
			SelfSlot,
			SelfGeneration,
			ObjectSlot,
			ObjectGeneration,
			GuestAddress,
			*OutValue);
	}

	static int32 TypedCommandBufferSubmitCallback(
		void* Environment,
		int32 GuestAddress,
		int32 ByteCount,
		int32* OutValue)
	{
		FAvidScriptWasmtimeTypedHostContext* HostContext = static_cast<FAvidScriptWasmtimeTypedHostContext*>(Environment);
		if (HostContext == nullptr || HostContext->Backend == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		return HostContext->Backend->InvokeTypedCommandBufferSubmit(
			*HostContext, GuestAddress, ByteCount, *OutValue);
	}

	bool ValidateTypedImports(
		const FAvidScriptWasmModuleLayout& ModuleLayout,
		const FAvidScriptVmBindingPackage* BindingPackage,
		TConstArrayView<FAvidScriptVmTypedHostImport> Imports,
		FAvidScriptVmError& OutError)
	{
		TypedImportIdentityKeys.Reset();
		if (Imports.IsEmpty())
		{
			return true;
		}
		if (TypedHostDispatcher == nullptr)
		{
			SetWasmtimeError(
				OutError,
				TEXT("typed_host_config_invalid"),
				TEXT("Typed host imports require a dispatcher."));
			return false;
		}

		TMap<FString, const FAvidScriptVmDynamicImport*> PackageImports;
		if (BindingPackage != nullptr)
		{
			PackageImports.Reserve(BindingPackage->Imports.Num());
			for (const FAvidScriptVmDynamicImport& Import : BindingPackage->Imports)
			{
				PackageImports.Add(
					MakeWasmtimeImportIdentityKey(Import.ModuleName, Import.ImportName),
					&Import);
			}
		}
		TMap<FString, int32> ActualImportCounts;
		ActualImportCounts.Reserve(ModuleLayout.FunctionImports.Num());
		for (const FAvidScriptWasmFunctionImport& Import : ModuleLayout.FunctionImports)
		{
			++ActualImportCounts.FindOrAdd(
				MakeWasmtimeImportIdentityKey(Import.ModuleName, Import.ImportName));
		}

		for (const FAvidScriptVmTypedHostImport& Import : Imports)
		{
			const FString IdentityKey =
				MakeWasmtimeImportIdentityKey(Import.ModuleName, Import.ImportName);
			const TCHAR* ExpectedSignature = nullptr;
			switch (Import.Shape)
			{
			case EAvidScriptVmTypedHostShape::EmptyI32:
				ExpectedSignature = TEXT("()i");
				break;
			case EAvidScriptVmTypedHostShape::I32PairToI32:
				ExpectedSignature = TEXT("(ii)i");
				break;
			case EAvidScriptVmTypedHostShape::SelfI32PairToI32:
				ExpectedSignature = TEXT("(iiii)i");
				break;
			case EAvidScriptVmTypedHostShape::SelfI32PairToGuestI32:
				ExpectedSignature = TEXT("(iiiii)i");
				break;
			case EAvidScriptVmTypedHostShape::SelfF32TripleToGuestVector:
				ExpectedSignature = TEXT("(iifffi)i");
				break;
			case EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet:
			case EAvidScriptVmTypedHostShape::SelfVectorValue:
				ExpectedSignature = TEXT("(iii)i");
				break;
			case EAvidScriptVmTypedHostShape::SelfPropertyI32Get:
				ExpectedSignature = TEXT("(ii)i");
				break;
			case EAvidScriptVmTypedHostShape::SelfPropertyI32Set:
				ExpectedSignature = TEXT("(iii)i");
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get:
				ExpectedSignature = TEXT("(I)i");
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Set:
				ExpectedSignature = TEXT("(Ii)");
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Get:
				ExpectedSignature = TEXT("(I)I");
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Set:
				ExpectedSignature = TEXT("(II)");
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Get:
				ExpectedSignature = TEXT("(I)f");
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Set:
				ExpectedSignature = TEXT("(If)");
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Get:
				ExpectedSignature = TEXT("(I)d");
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Set:
				ExpectedSignature = TEXT("(Id)");
				break;
			case EAvidScriptVmTypedHostShape::StableObjectRoundtrip:
				ExpectedSignature = TEXT("(iiiii)i");
				break;
			case EAvidScriptVmTypedHostShape::CommandBufferSubmit:
				ExpectedSignature = TEXT("(ii)i");
				break;
			default:
				OutError.Reset();
				OutError.Category = TEXT("typed_host_shape_unavailable");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details = TEXT("The requested typed host shape is not implemented by this backend stage.");
				return false;
			}
			const bool bSupplemental = Import.bSupplementalRuntimeAuthority;
			if (Import.StableId.IsEmpty()
				|| (bSupplemental
					? Import.BindingOrdinal != MAX_uint32
					: Import.BindingOrdinal == MAX_uint32)
				|| Import.ModuleName.IsEmpty()
				|| Import.ImportName.IsEmpty()
				|| Import.Signature != ExpectedSignature)
			{
				OutError.Reset();
				OutError.Category = TEXT("typed_host_contract_invalid");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details = TEXT("Typed host metadata is incomplete or does not match its fixed shape signature.");
				return false;
			}
			const int32 PreparedFunctionCount =
				(Import.PreparedTarget.SelfI32Pair != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.SelfI32PairGuestResult != nullptr
					? 1
					: 0)
				+ (Import.PreparedTarget.SelfF32TripleGuestVector != nullptr
					? 1
					: 0)
				+ (Import.PreparedTarget.SelfGuestAddress != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.StableObjectRoundtrip != nullptr
					? 1
					: 0)
				+ (Import.PreparedTarget.SelfPropertyI32Get != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.SelfPropertyI32Set != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.PackedSelfPropertyI32Get != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.PackedSelfPropertyI32Set != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.PackedSelfPropertyI64Get != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.PackedSelfPropertyI64Set != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.PackedSelfPropertyF32Get != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.PackedSelfPropertyF32Set != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.PackedSelfPropertyF64Get != nullptr ? 1 : 0)
				+ (Import.PreparedTarget.PackedSelfPropertyF64Set != nullptr ? 1 : 0);
			const bool bHasPreparedContext =
				Import.PreparedTarget.Context != nullptr;
			const bool bRequiresPreparedTarget =
				Import.Shape
					== EAvidScriptVmTypedHostShape::SelfI32PairToGuestI32
				|| Import.Shape
					== EAvidScriptVmTypedHostShape::SelfF32TripleToGuestVector
				|| Import.Shape
					== EAvidScriptVmTypedHostShape::SelfPropertyI32Get
				|| Import.Shape
					== EAvidScriptVmTypedHostShape::SelfPropertyI32Set;
			const bool bRequiresSupplementalPreparedTarget =
				Import.Shape == EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get
				|| Import.Shape == EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Set
				|| Import.Shape == EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Get
				|| Import.Shape == EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Set
				|| Import.Shape == EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Get
				|| Import.Shape == EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Set
				|| Import.Shape == EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Get
				|| Import.Shape == EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Set;
			if (bHasPreparedContext != (PreparedFunctionCount == 1)
				|| (PreparedFunctionCount == 1
					&& !Import.PreparedTarget.IsBoundForShape(Import.Shape))
				|| ((bRequiresPreparedTarget || bRequiresSupplementalPreparedTarget)
					&& !Import.PreparedTarget.IsBoundForShape(Import.Shape)))
			{
				OutError.Reset();
				OutError.Category = TEXT("typed_host_prepared_target_invalid");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details = TEXT("Prepared typed host targets must be complete and match their fixed shape.");
				return false;
			}
			if (IsAvidScriptVmStaticHostImport(Import.ModuleName, Import.ImportName)
				|| TypedImportIdentityKeys.Contains(IdentityKey))
			{
				OutError.Reset();
				OutError.Category = TEXT("typed_host_identity_conflict");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details = TEXT("Typed host import identities must be unique and cannot replace static imports.");
				return false;
			}
			const FAvidScriptVmDynamicImport* const* PackageImport =
				PackageImports.Find(IdentityKey);
			const bool bAuthorityMatches = bSupplemental
				? Import.PreparedTarget.IsBoundForShape(Import.Shape)
				: PackageImport != nullptr
					&& (*PackageImport)->StableId == Import.StableId
					&& (*PackageImport)->Ordinal == Import.BindingOrdinal
					&& (*PackageImport)->Signature == Import.Signature;
			if (!bAuthorityMatches || ActualImportCounts.FindRef(IdentityKey) != 1)
			{
				OutError.Reset();
				OutError.Category = TEXT("typed_host_binding_mismatch");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details = TEXT("Typed host metadata does not match the binding package and actual WASM imports.");
				return false;
			}
			TypedImportIdentityKeys.Add(IdentityKey);
		}
		return true;
	}

	bool DefineTypedImports(
		TConstArrayView<FAvidScriptVmTypedHostImport> Imports,
		FAvidScriptVmError& OutError)
	{
		TypedHostContexts.Reserve(Imports.Num());
		for (const FAvidScriptVmTypedHostImport& Import : Imports)
		{
			TUniquePtr<FAvidScriptWasmtimeTypedHostContext> HostContext =
				MakeUnique<FAvidScriptWasmtimeTypedHostContext>();
			HostContext->Backend = this;
			HostContext->BindingOrdinal = Import.BindingOrdinal;
			HostContext->StableId = Import.StableId;
			HostContext->ModuleName = Import.ModuleName;
			HostContext->ImportName = Import.ImportName;
			HostContext->Shape = Import.Shape;
			HostContext->PreparedTarget = Import.PreparedTarget;
			FTCHARToUTF8 ModuleNameUtf8(*HostContext->ModuleName);
			FTCHARToUTF8 ImportNameUtf8(*HostContext->ImportName);
			FAvidScriptWasmtimeTypedHostContext* HostContextPointer = HostContext.Get();
			TypedHostContexts.Add(MoveTemp(HostContext));

			AvidScriptWasmtimeFailure* DefineFailure = nullptr;
			switch (Import.Shape)
			{
			case EAvidScriptVmTypedHostShape::EmptyI32:
				DefineFailure = avidscript_wasmtime_linker_define_empty_i32(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedEmptyI32Callback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::I32PairToI32:
				DefineFailure = avidscript_wasmtime_linker_define_i32_pair(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedI32PairCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::SelfI32PairToI32:
				DefineFailure = avidscript_wasmtime_linker_define_self_i32_pair(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedSelfI32PairCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::SelfI32PairToGuestI32:
				DefineFailure =
					avidscript_wasmtime_linker_define_self_i32_pair_guest_result(
						Linker,
						ModuleNameUtf8.Get(),
						static_cast<size_t>(ModuleNameUtf8.Length()),
						ImportNameUtf8.Get(),
						static_cast<size_t>(ImportNameUtf8.Length()),
						&TypedSelfI32PairGuestResultCallback,
						HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::SelfF32TripleToGuestVector:
				DefineFailure =
					avidscript_wasmtime_linker_define_self_f32_triple_guest_vector(
						Linker,
						ModuleNameUtf8.Get(),
						static_cast<size_t>(ModuleNameUtf8.Length()),
						ImportNameUtf8.Get(),
						static_cast<size_t>(ImportNameUtf8.Length()),
						&TypedSelfF32TripleGuestVectorCallback,
						HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet:
				DefineFailure = avidscript_wasmtime_linker_define_self_property_i32_get_set(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedSelfPropertyI32GetSetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::SelfPropertyI32Get:
				DefineFailure = avidscript_wasmtime_linker_define_self_property_i32_get(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedSelfPropertyI32GetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::SelfPropertyI32Set:
				DefineFailure = avidscript_wasmtime_linker_define_self_property_i32_set(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedSelfPropertyI32SetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Get:
				DefineFailure = avidscript_wasmtime_linker_define_packed_self_property_i32_get(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedPackedSelfPropertyI32GetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyI32Set:
				DefineFailure = avidscript_wasmtime_linker_define_packed_self_property_i32_set(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedPackedSelfPropertyI32SetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Get:
				DefineFailure = avidscript_wasmtime_linker_define_packed_self_property_i64_get(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedPackedSelfPropertyI64GetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyI64Set:
				DefineFailure = avidscript_wasmtime_linker_define_packed_self_property_i64_set(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedPackedSelfPropertyI64SetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Get:
				DefineFailure = avidscript_wasmtime_linker_define_self_property_f32_get(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedPackedSelfPropertyF32GetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Get:
				DefineFailure = avidscript_wasmtime_linker_define_packed_self_property_f64_get(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedPackedSelfPropertyF64GetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyF64Set:
				DefineFailure = avidscript_wasmtime_linker_define_packed_self_property_f64_set(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedPackedSelfPropertyF64SetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::PackedSelfPropertyF32Set:
				DefineFailure = avidscript_wasmtime_linker_define_self_property_f32_set(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedPackedSelfPropertyF32SetCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::SelfVectorValue:
				DefineFailure = avidscript_wasmtime_linker_define_self_vector_value(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedSelfVectorValueCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::StableObjectRoundtrip:
				DefineFailure = avidscript_wasmtime_linker_define_stable_object_roundtrip(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedStableObjectRoundtripCallback,
					HostContextPointer);
				break;
			case EAvidScriptVmTypedHostShape::CommandBufferSubmit:
				DefineFailure = avidscript_wasmtime_linker_define_command_buffer_submit(
					Linker,
					ModuleNameUtf8.Get(),
					static_cast<size_t>(ModuleNameUtf8.Length()),
					ImportNameUtf8.Get(),
					static_cast<size_t>(ImportNameUtf8.Length()),
					&TypedCommandBufferSubmitCallback,
					HostContextPointer);
				break;
			default:
				OutError.Reset();
				OutError.Category = TEXT("typed_host_shape_unavailable");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details = TEXT("The requested typed host shape is not implemented by this backend stage.");
				return false;
			}
			if (DefineFailure != nullptr)
			{
				TArray<FAvidScriptVmStackFrame> Frames;
				bool bWasTrap = false;
				OutError.Reset();
				OutError.Category = TEXT("typed_host_registration_failed");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details = ConsumeWasmtimeFailure(DefineFailure, Frames, bWasTrap);
				return false;
			}
		}
		return true;
	}

	bool DefineStaticImportForModule(
		const char* ModuleName,
		const FAvidScriptVmStaticHostImport& Import,
		FAvidScriptWasmtimeHostContext& HostContext,
		FAvidScriptVmError& OutError)
	{
		TArray<AvidScriptWasmtimeValueKind, TInlineAllocator<8>> ParameterKinds;
		ParameterKinds.Reserve(HostContext.Signature.Parameters.Num());
		for (EAvidScriptVmValueKind Kind : HostContext.Signature.Parameters)
		{
			ParameterKinds.Add(ToWasmtimeValueKind(Kind));
		}
		AvidScriptWasmtimeValueKind ResultKind = ToWasmtimeValueKind(HostContext.Signature.Result);
		AvidScriptWasmtimeFailure* DefineFailure = avidscript_wasmtime_linker_define_func(
			Linker,
			ModuleName,
			FCStringAnsi::Strlen(ModuleName),
			Import.ImportName,
			FCStringAnsi::Strlen(Import.ImportName),
			ParameterKinds.GetData(),
			static_cast<size_t>(ParameterKinds.Num()),
			HostContext.Signature.bHasResult ? &ResultKind : nullptr,
			HostContext.Signature.bHasResult ? 1 : 0,
			&StaticHostCallback,
			&HostContext);
		if (DefineFailure != nullptr)
		{
			TArray<FAvidScriptVmStackFrame> Frames;
			bool bWasTrap = false;
			OutError.Reset();
			OutError.Category = TEXT("host_import_registration_failed");
			OutError.ImportModuleName = UTF8_TO_TCHAR(ModuleName);
			OutError.ImportName = UTF8_TO_TCHAR(Import.ImportName);
			OutError.Details = ConsumeWasmtimeFailure(DefineFailure, Frames, bWasTrap);
			return false;
		}
		return true;
	}

	bool DefineStaticImports(FAvidScriptVmError& OutError)
	{
		const TConstArrayView<FAvidScriptVmStaticHostImport> Imports = GetAvidScriptVmStaticHostImports();
		HostContexts.Reserve(Imports.Num());
		for (const FAvidScriptVmStaticHostImport& Import : Imports)
		{
			TUniquePtr<FAvidScriptWasmtimeHostContext> HostContext = MakeUnique<FAvidScriptWasmtimeHostContext>();
			HostContext->Backend = this;
			HostContext->Import = &Import;
			FString ParseError;
			if (!ParseAvidScriptVmAbiSignature(
				UTF8_TO_TCHAR(Import.Signature),
				HostContext->Signature,
				ParseError))
			{
				SetWasmtimeError(OutError, TEXT("host_import_registration_failed"), ParseError);
				return false;
			}
			FAvidScriptWasmtimeHostContext* HostContextPointer = HostContext.Get();
			HostContexts.Add(MoveTemp(HostContext));
			if (!DefineStaticImportForModule("avidscript", Import, *HostContextPointer, OutError))
			{
				return false;
			}
			if (Import.bSupportsEnvCompatibility
				&& !DefineStaticImportForModule("env", Import, *HostContextPointer, OutError))
			{
				return false;
			}
		}
		return true;
	}

	bool DefineDynamicImports(
		const FAvidScriptVmBindingPackage& BindingPackage,
		FAvidScriptVmError& OutError)
	{
		DynamicHostContexts.Reserve(BindingPackage.Imports.Num());
		for (const FAvidScriptVmDynamicImport& Import : BindingPackage.Imports)
		{
			if (TypedImportIdentityKeys.Contains(
				MakeWasmtimeImportIdentityKey(Import.ModuleName, Import.ImportName)))
			{
				continue;
			}
			TUniquePtr<FAvidScriptWasmtimeDynamicHostContext> HostContext =
				MakeUnique<FAvidScriptWasmtimeDynamicHostContext>();
			HostContext->Backend = this;
			HostContext->Ordinal = Import.Ordinal;
			HostContext->StableId = Import.StableId;
			HostContext->ModuleName = Import.ModuleName;
			HostContext->ImportName = Import.ImportName;
			HostContext->CompactSignature = Import.Signature;
			if (!Import.PreparedTarget.IsEmpty()
				&& !Import.PreparedTarget.IsBound())
			{
				OutError.Reset();
				OutError.Category = TEXT("host_import_registration_failed");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details =
					TEXT("The prepared dynamic host target is partially bound.");
				return false;
			}
			HostContext->PreparedTarget = Import.PreparedTarget;
			FString ParseError;
			if (!ParseAvidScriptVmAbiSignature(
				Import.Signature,
				HostContext->Signature,
				ParseError))
			{
				OutError.Reset();
				OutError.Category = TEXT("host_import_registration_failed");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details = MoveTemp(ParseError);
				return false;
			}

			TArray<AvidScriptWasmtimeValueKind, TInlineAllocator<64>> ParameterKinds;
			ParameterKinds.Reserve(HostContext->Signature.Parameters.Num());
			for (EAvidScriptVmValueKind Kind : HostContext->Signature.Parameters)
			{
				ParameterKinds.Add(ToWasmtimeValueKind(Kind));
			}
			const AvidScriptWasmtimeValueKind ResultKind =
				ToWasmtimeValueKind(HostContext->Signature.Result);
			FTCHARToUTF8 ModuleNameUtf8(*HostContext->ModuleName);
			FTCHARToUTF8 ImportNameUtf8(*HostContext->ImportName);
			FAvidScriptWasmtimeDynamicHostContext* HostContextPointer = HostContext.Get();
			DynamicHostContexts.Add(MoveTemp(HostContext));
			AvidScriptWasmtimeFailure* DefineFailure = avidscript_wasmtime_linker_define_func(
				Linker,
				ModuleNameUtf8.Get(),
				static_cast<size_t>(ModuleNameUtf8.Length()),
				ImportNameUtf8.Get(),
				static_cast<size_t>(ImportNameUtf8.Length()),
				ParameterKinds.GetData(),
				static_cast<size_t>(ParameterKinds.Num()),
				HostContextPointer->Signature.bHasResult ? &ResultKind : nullptr,
				HostContextPointer->Signature.bHasResult ? 1 : 0,
				&DynamicHostCallback,
				HostContextPointer);
			if (DefineFailure != nullptr)
			{
				TArray<FAvidScriptVmStackFrame> Frames;
				bool bWasTrap = false;
				OutError.Reset();
				OutError.Category = TEXT("host_import_registration_failed");
				OutError.ImportModuleName = Import.ModuleName;
				OutError.ImportName = Import.ImportName;
				OutError.Details = ConsumeWasmtimeFailure(DefineFailure, Frames, bWasTrap);
				return false;
			}
		}
		return true;
	}

	bool TryGetGuestMemory(uint8*& OutData, size_t& OutSize)
	{
		return avidscript_wasmtime_memory_data(
			Store,
			Instance,
			ActiveCaller,
			&OutData,
			&OutSize);
	}
#endif

	void PerformUnload()
	{
#if AVIDSCRIPT_WITH_WASMTIME
		for (const TPair<FString, uint32>& ActiveExport :
			ExportNameToIndex)
		{
			if (ExportEntries.IsValidIndex(
					static_cast<int32>(ActiveExport.Value)))
			{
				FAvidScriptWasmtimeExportEntry* Entry =
					ExportEntries[ActiveExport.Value].Get();
				if (Entry != nullptr && Entry->Function != nullptr)
				{
					avidscript_wasmtime_function_delete(
						Entry->Function);
					Entry->Function = nullptr;
				}
			}
		}
		if (Instance != nullptr)
		{
			avidscript_wasmtime_instance_delete(Instance);
			Instance = nullptr;
		}
		if (Module != nullptr)
		{
			avidscript_wasmtime_module_delete(Module);
			Module = nullptr;
		}
		if (Linker != nullptr)
		{
			avidscript_wasmtime_linker_delete(Linker);
			Linker = nullptr;
		}
		HostContexts.Reset();
		DynamicHostContexts.Reset();
		TypedHostContexts.Reset();
		TypedImportIdentityKeys.Reset();
		ActiveCaller = nullptr;
		if (Store != nullptr)
		{
			avidscript_wasmtime_store_delete(Store);
			Store = nullptr;
		}
		if (Engine != nullptr)
		{
			avidscript_wasmtime_engine_delete(Engine);
			Engine = nullptr;
		}
#endif
		ExportNameToIndex.Reset();
		++ExportGeneration;
		if (ExportGeneration == 0)
		{
			++ExportGeneration;
		}
		ExportLookupCount = 0;
		ModuleId.Reset();
		HostDispatcher = nullptr;
		TypedHostDispatcher = nullptr;
		bUnloadDeferred = false;
		AdvanceBackendInstanceIdentity();
	}

	void AdvanceBackendInstanceIdentity()
	{
		BackendInstanceIdentity = AllocateWasmtimeInstanceIdentity();
		OwnedBackendInstanceIdentities.Add(BackendInstanceIdentity);
	}

	FAvidScriptVmBackendInfo BackendInfo;
	uint64 BackendInstanceIdentity = 0;
	TSet<uint64> OwnedBackendInstanceIdentities;
	FString ModuleId;
	IAvidScriptHostDispatcher* HostDispatcher = nullptr;
	IAvidScriptVmTypedHostDispatcher* TypedHostDispatcher = nullptr;
	FAvidScriptVmLoadMetrics LoadMetrics;
	TMap<FString, uint32> ExportNameToIndex;
	uint32 ExportGeneration = 1;
	uint32 ExportLookupCount = 0;
	int32 ActiveCallDepth = 0;
	bool bUnloadDeferred = false;
	bool bHasPendingHostFailure = false;
	FString PendingHostImportModuleName;
	FString PendingHostImportName;
	FString PendingHostFailureDetails;

#if AVIDSCRIPT_WITH_WASMTIME
	AvidScriptWasmtimeEngine* Engine = nullptr;
	AvidScriptWasmtimeStore* Store = nullptr;
	AvidScriptWasmtimeLinker* Linker = nullptr;
	AvidScriptWasmtimeModule* Module = nullptr;
	AvidScriptWasmtimeInstance* Instance = nullptr;
	AvidScriptWasmtimeCaller* ActiveCaller = nullptr;
	TArray<TUniquePtr<FAvidScriptWasmtimeHostContext>> HostContexts;
	TArray<TUniquePtr<FAvidScriptWasmtimeDynamicHostContext>> DynamicHostContexts;
	TArray<TUniquePtr<FAvidScriptWasmtimeTypedHostContext>> TypedHostContexts;
	TSet<FString> TypedImportIdentityKeys;
	TArray<TUniquePtr<FAvidScriptWasmtimeExportEntry>> ExportEntries;
#endif
};
} // namespace

TUniquePtr<IAvidScriptVmBackend> CreateAvidScriptWasmtimeBackend(
	EAvidScriptVmArtifactFormat ArtifactFormat)
{
	return MakeUnique<FAvidScriptWasmtimeBackend>(ArtifactFormat);
}
