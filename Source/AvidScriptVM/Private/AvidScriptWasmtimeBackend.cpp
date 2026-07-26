#include "AvidScriptVmBackend.h"

#include "AvidScriptVmStaticHostImports.h"
#include "AvidScriptWasmtimeApi.h"
#include "AvidScriptWasmModuleLayout.h"

#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

#ifndef AVIDSCRIPT_WITH_WASMTIME
#define AVIDSCRIPT_WITH_WASMTIME 0
#endif

namespace
{
FCriticalSection GWasmtimeIdentityCriticalSection;
FCriticalSection GWasmtimeDllCriticalSection;
uint64 GNextWasmtimeInstanceIdentity = 1ull << 63;
void* GWasmtimeDllHandle = nullptr;

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

#if AVIDSCRIPT_WITH_WASMTIME && PLATFORM_WINDOWS
bool EnsureWasmtimeDllLoaded(FString& OutError)
{
	FScopeLock Lock(&GWasmtimeDllCriticalSection);
	if (GWasmtimeDllHandle != nullptr)
	{
		return true;
	}
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("AvidScript"));
	if (!Plugin.IsValid())
	{
		OutError = TEXT("AvidScript plugin base directory is unavailable.");
		return false;
	}
	const FString Candidates[] = {
		FPaths::Combine(Plugin->GetBaseDir(), TEXT("Binaries/Win64/wasmtime.dll")),
		FPaths::Combine(
			Plugin->GetBaseDir(),
			TEXT("Source/ThirdParty/Wasmtime/installed/Win64/v45.0.0/lib/wasmtime.dll"))
	};
	for (const FString& Candidate : Candidates)
	{
		if (FPaths::FileExists(Candidate))
		{
			GWasmtimeDllHandle = FPlatformProcess::GetDllHandle(*Candidate);
			if (GWasmtimeDllHandle != nullptr)
			{
				return true;
			}
		}
	}
	OutError = TEXT("The locked Wasmtime v45 DLL could not be loaded from the plugin deployment or managed dependency layout.");
	return false;
}
#endif

void SetWasmtimeError(FAvidScriptVmError& OutError, const TCHAR* Category, const FString& Details)
{
	OutError.Reset();
	OutError.Category = Category;
	OutError.Details = Details;
}

FAvidScriptVmBackendInfo MakeWasmtimeBackendInfo()
{
	FAvidScriptVmBackendInfo Info;
	Info.Kind = EAvidScriptVmBackendKind::Wasmtime;
	Info.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Info.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	Info.Capabilities = EAvidScriptVmCapability::GuestMemory
		| EAvidScriptVmCapability::Jit
		| EAvidScriptVmCapability::StructuredStack;
	Info.StableBackendId = TEXT("wasmtime.cranelift.jit");
	Info.RuntimeVersion = TEXT("45.0.0");
#if PLATFORM_WINDOWS
	Info.TargetTriple = TEXT("x86_64-pc-windows-msvc");
#else
	Info.TargetTriple = TEXT("unknown-unknown-unknown");
#endif
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

struct FAvidScriptWasmtimeExportEntry
{
	FString Name;
	AvidScriptWasmtimeFunction* Function = nullptr;
	uint32 Generation = 0;
	uint32 ParameterCount = 0;
};
#endif

class FAvidScriptWasmtimeBackend final : public IAvidScriptVmBackend, public IAvidScriptVmGuestMemory
{
public:
	FAvidScriptWasmtimeBackend()
		: BackendInfo(MakeWasmtimeBackendInfo())
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
		if (Bytecode.IsEmpty())
		{
			SetWasmtimeError(OutError, TEXT("invalid_bytecode"), TEXT("No WASM bytecode was provided."));
			return false;
		}
		if (Config.StackSize == 0 || Config.HeapSize == 0)
		{
			SetWasmtimeError(OutError, TEXT("invalid_config"), TEXT("VM stack and heap sizes must be non-zero."));
			return false;
		}
		if (Config.BindingPackage != nullptr)
		{
			SetWasmtimeError(
				OutError,
				TEXT("dynamic_import_unavailable"),
				TEXT("Wasmtime dynamic binding packages are implemented by Phase 54 Task 3B."));
			return false;
		}

		FAvidScriptWasmModuleLayout ModuleLayout;
		FString LayoutError;
		if (!InspectAvidScriptWasmModuleLayout(Bytecode, ModuleLayout, LayoutError))
		{
			SetWasmtimeError(OutError, TEXT("wasm_layout_invalid"), LayoutError);
			return false;
		}
		if (!ValidateAvidScriptVmImportContract(
			ModuleLayout,
			nullptr,
			TConstArrayView<FAvidScriptVmExpectedImport>(),
			false,
			OutError))
		{
			return false;
		}

		ModuleId = InModuleId;
		HostDispatcher = Config.HostDispatcher;

		const double RuntimeInitStart = FPlatformTime::Seconds();
#if PLATFORM_WINDOWS
		FString DllLoadError;
		if (!EnsureWasmtimeDllLoaded(DllLoadError))
		{
			LoadMetrics.RuntimeInitMs = MeasureWasmtimeElapsedMs(RuntimeInitStart);
			SetWasmtimeError(OutError, TEXT("runtime_init_failed"), DllLoadError);
			return false;
		}
#endif
		Engine = avidscript_wasmtime_engine_new();
		LoadMetrics.RuntimeInitMs = MeasureWasmtimeElapsedMs(RuntimeInitStart);
		if (Engine == nullptr)
		{
			SetWasmtimeError(OutError, TEXT("runtime_init_failed"), TEXT("Wasmtime could not create a Cranelift engine."));
			return false;
		}

		const double ModuleLoadStart = FPlatformTime::Seconds();
		AvidScriptWasmtimeFailure* ModuleFailure = avidscript_wasmtime_module_new(
			Engine,
			Bytecode.GetData(),
			static_cast<size_t>(Bytecode.Num()),
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
			const FAvidScriptWasmtimeExportEntry& Existing = ExportEntries[*ExistingIndex];
			OutHandle.Slot = *ExistingIndex + 1;
			OutHandle.Generation = Existing.Generation;
			OutHandle.BackendInstanceIdentity = BackendInstanceIdentity;
			return true;
		}

		++ExportLookupCount;
		FTCHARToUTF8 ExportNameUtf8(*ExportName);
		AvidScriptWasmtimeFunction* Function = nullptr;
		uint32 ParameterCount = 0;
		const int ResolveResult = avidscript_wasmtime_instance_resolve_event_export(
			Store,
			Instance,
			ExportNameUtf8.Get(),
			static_cast<size_t>(ExportNameUtf8.Length()),
			&Function,
			&ParameterCount);
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
				TEXT("Wasmtime event exports must be () -> void or (f32) -> void in this execution tier."));
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

		FAvidScriptWasmtimeExportEntry& Entry = ExportEntries.AddDefaulted_GetRef();
		Entry.Name = ExportName;
		Entry.Function = Function;
		Entry.Generation = ExportGeneration;
		Entry.ParameterCount = ParameterCount;
		const uint32 EntryIndex = static_cast<uint32>(ExportEntries.Num() - 1);
		ExportNameToIndex.Add(ExportName, EntryIndex);

		OutHandle.Slot = EntryIndex + 1;
		OutHandle.Generation = Entry.Generation;
		OutHandle.BackendInstanceIdentity = BackendInstanceIdentity;
		return true;
#endif
	}

	bool Call(
		const FAvidScriptVmExportHandle& Handle,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError) override
	{
		OutError.Reset();
		bHasPendingHostFailure = false;
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
		const FAvidScriptWasmtimeExportEntry& Entry = ExportEntries[Handle.Slot - 1];
		if (Frame.CellCount != Entry.ParameterCount)
		{
			SetWasmtimeError(OutError, TEXT("invalid_arguments"), TEXT("VM call frame does not match the cached export signature."));
			return false;
		}

		float DeltaSeconds = 0.0f;
		const float* OptionalDeltaSeconds = nullptr;
		if (Entry.ParameterCount == 1)
		{
			FMemory::Memcpy(&DeltaSeconds, Frame.Cells, sizeof(float));
			OptionalDeltaSeconds = &DeltaSeconds;
		}

		++ActiveCallDepth;
		AvidScriptWasmtimeFailure* CallFailure = avidscript_wasmtime_function_call_event(
			Store,
			Entry.Function,
			OptionalDeltaSeconds);
		const bool bCallFailed = CallFailure != nullptr;
		const bool bUnloadRequestedDuringCall = bUnloadDeferred;
		FString FailureDetails;
		TArray<FAvidScriptVmStackFrame> StackFrames;
		bool bWasTrap = false;
		if (bCallFailed)
		{
			FailureDetails = ConsumeWasmtimeFailure(CallFailure, StackFrames, bWasTrap);
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
				OutError.ImportModuleName = TEXT("avidscript");
				OutError.ImportName = PendingHostImportName;
				OutError.Details = PendingHostFailureDetails;
			}
			else
			{
				SetWasmtimeError(
					OutError,
					bWasTrap ? TEXT("trap") : TEXT("invalid_arguments"),
					FailureDetails.IsEmpty() ? TEXT("Wasmtime call failed without a diagnostic message.") : FailureDetails);
			}
			OutError.StackFrames = MoveTemp(StackFrames);
			return false;
		}
		return true;
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
			bHasPendingHostFailure = true;
			PendingHostImportName = UTF8_TO_TCHAR(HostContext.Import->ImportName);
			PendingHostFailureDetails = TEXT("Wasmtime host callback shape does not match the static import catalog.");
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
				bHasPendingHostFailure = true;
				PendingHostImportName = UTF8_TO_TCHAR(HostContext.Import->ImportName);
				PendingHostFailureDetails = TEXT("Wasmtime host callback value kind does not match the static import catalog.");
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
			bHasPendingHostFailure = true;
			PendingHostImportName = UTF8_TO_TCHAR(HostContext.Import->ImportName);
			PendingHostFailureDetails = FailureDetails.IsEmpty()
				? TEXT("Wasmtime static host import adapter rejected the call.")
				: MoveTemp(FailureDetails);
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
#endif

private:
#if AVIDSCRIPT_WITH_WASMTIME
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
		for (FAvidScriptWasmtimeExportEntry& Entry : ExportEntries)
		{
			avidscript_wasmtime_function_delete(Entry.Function);
			Entry.Function = nullptr;
		}
		ExportEntries.Reset();
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
	FAvidScriptVmLoadMetrics LoadMetrics;
	TMap<FString, uint32> ExportNameToIndex;
	uint32 ExportGeneration = 1;
	uint32 ExportLookupCount = 0;
	int32 ActiveCallDepth = 0;
	bool bUnloadDeferred = false;
	bool bHasPendingHostFailure = false;
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
	TArray<FAvidScriptWasmtimeExportEntry> ExportEntries;
#endif
};
} // namespace

TUniquePtr<IAvidScriptVmBackend> CreateAvidScriptWasmtimeBackend()
{
	return MakeUnique<FAvidScriptWasmtimeBackend>();
}
