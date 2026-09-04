#include "AvidScriptVmBackend.h"

#include "AvidScriptVmExportTable.h"
#include "AvidScriptWamrCallStack.h"
#include "AvidScriptWamrDynamicRegistry.h"
#include "AvidScriptWamrHostBindings.h"
#include "AvidScriptWasmModuleLayout.h"

#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

#ifndef AVIDSCRIPT_WITH_WAMR
#define AVIDSCRIPT_WITH_WAMR 0
#endif
#ifndef AVIDSCRIPT_WAMR_INTERPRETER_CONFIG
#define AVIDSCRIPT_WAMR_INTERPRETER_CONFIG "unavailable"
#endif
#ifndef AVIDSCRIPT_WAMR_STATIC_LIB_SHA256
#define AVIDSCRIPT_WAMR_STATIC_LIB_SHA256 "unavailable"
#endif

#if AVIDSCRIPT_WITH_WAMR
extern "C"
{
#include "wasm_export.h"
}
#endif

namespace
{
constexpr uint32 ErrorBufferSize = 512;
FCriticalSection GBackendIdentityCriticalSection;
uint64 GNextBackendInstanceIdentity = 0;

uint64 AllocateBackendInstanceIdentity()
{
	FScopeLock Lock(&GBackendIdentityCriticalSection);
	++GNextBackendInstanceIdentity;
	if (GNextBackendInstanceIdentity == 0)
	{
		++GNextBackendInstanceIdentity;
	}
	return GNextBackendInstanceIdentity;
}

FString GetWamrTargetTriple()
{
#if PLATFORM_WINDOWS
	return TEXT("x86_64-pc-windows-msvc");
#elif PLATFORM_LINUX
	return TEXT("x86_64-unknown-linux-gnu");
#else
	return TEXT("unknown-unknown-unknown");
#endif
}

FAvidScriptVmBackendInfo MakeWamrBackendInfo()
{
	FAvidScriptVmBackendInfo Info;
	Info.Kind = EAvidScriptVmBackendKind::Wamr;
	Info.ExecutionMode = EAvidScriptVmExecutionMode::Interpreter;
	Info.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	Info.Capabilities = EAvidScriptVmCapability::GuestMemory
		| EAvidScriptVmCapability::Interpreter
		| EAvidScriptVmCapability::StructuredStack
		| EAvidScriptVmCapability::DebugProbe
		| EAvidScriptVmCapability::HostCallBudget;
	Info.StableBackendId = TEXT("wamr.interpreter");
	Info.TargetTriple = GetWamrTargetTriple();
#if AVIDSCRIPT_WITH_WAMR
	uint32 Major = 0;
	uint32 Minor = 0;
	uint32 Patch = 0;
	wasm_runtime_get_version(&Major, &Minor, &Patch);
	Info.RuntimeVersion = FString::Printf(TEXT("%u.%u.%u"), Major, Minor, Patch);
#else
	Info.RuntimeVersion = TEXT("unavailable");
#endif
	const FString InterpreterConfig =
		UTF8_TO_TCHAR(AVIDSCRIPT_WAMR_INTERPRETER_CONFIG);
	Info.RuntimeArtifactSha256 =
		UTF8_TO_TCHAR(AVIDSCRIPT_WAMR_STATIC_LIB_SHA256);
	Info.RuntimeBuildIdentity = FString::Printf(
		TEXT("wamr-v%s;config=%s;static_lib_sha256=%s"),
		*Info.RuntimeVersion,
		*InterpreterConfig,
		*Info.RuntimeArtifactSha256);
	return Info;
}

double MeasureElapsedMs(double StartSeconds)
{
	return FMath::Max((FPlatformTime::Seconds() - StartSeconds) * 1000.0, 0.0001);
}
void SetVmError(FAvidScriptVmError& OutError, const TCHAR* Category, const FString& Details)
{
	OutError.Reset();
	OutError.Category = Category;
	OutError.Details = Details;
}

#if AVIDSCRIPT_WITH_WAMR
FCriticalSection GWamrLeaseCriticalSection;
int32 GWamrLeaseCount = 0;

bool AcquireWamrLease(FAvidScriptVmError& OutError)
{
	FScopeLock Lock(&GWamrLeaseCriticalSection);
	if (GWamrLeaseCount == 0 && !wasm_runtime_init())
	{
		SetVmError(OutError, TEXT("runtime_init_failed"), TEXT("wasm_runtime_init returned false"));
		return false;
	}
	if (GWamrLeaseCount == 0 && !RegisterAvidScriptWamrHostBindings())
	{
		wasm_runtime_destroy();
		SetVmError(OutError, TEXT("host_import_registration_failed"), TEXT("WAMR rejected AvidScript native symbols."));
		return false;
	}

	++GWamrLeaseCount;
	return true;
}

void ReleaseWamrLease()
{
	FScopeLock Lock(&GWamrLeaseCriticalSection);
	if (GWamrLeaseCount <= 0)
	{
		GWamrLeaseCount = 0;
		return;
	}

	--GWamrLeaseCount;
	if (GWamrLeaseCount == 0)
	{
		UnregisterAvidScriptWamrHostBindings();
		wasm_runtime_destroy();
	}
}

FString GetWamrException(wasm_module_inst_t ModuleInstance)
{
	if (ModuleInstance == nullptr)
	{
		return TEXT("No WAMR module instance is available.");
	}

	const char* Exception = wasm_runtime_get_exception(ModuleInstance);
	return Exception != nullptr ? UTF8_TO_TCHAR(Exception) : TEXT("WAMR did not report an exception.");
}
#endif

class FAvidScriptWamrBackend final : public IAvidScriptVmBackend, public IAvidScriptWamrHostBridge, public IAvidScriptVmGuestMemory
{
public:
	FAvidScriptWamrBackend()
		: BackendInfo(MakeWamrBackendInfo())
	{
		AdvanceBackendInstanceIdentity();
	}

	~FAvidScriptWamrBackend() override
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
			SetVmError(
				OutError,
				TEXT("reentrant_operation"),
				TEXT("A VM module cannot be replaced while a guest call is active."));
			return false;
		}
		Unload();
		LoadMetrics = FAvidScriptVmLoadMetrics();

#if !AVIDSCRIPT_WITH_WAMR
		SetVmError(OutError, TEXT("backend_unavailable"), TEXT("WAMR artifacts are unavailable for this target."));
		return false;
#else
		if (Bytecode.IsEmpty())
		{
			SetVmError(OutError, TEXT("invalid_bytecode"), TEXT("No WASM bytecode was provided."));
			return false;
		}

		if (Config.StackSize == 0 || Config.HeapSize == 0)
		{
			SetVmError(OutError, TEXT("invalid_config"), TEXT("VM stack and heap sizes must be non-zero."));
			return false;
		}
		if (Config.ExecutionBudget.FuelPerEntry > 0
			|| Config.ExecutionBudget.EpochDeadlineTicks > 0
			|| Config.ExecutionBudget.EpochTimeoutMilliseconds > 0
			|| Config.ExecutionBudget.MaxLinearMemoryBytes > 0)
		{
			SetVmError(
				OutError,
				TEXT("execution_budget_unsupported"),
				TEXT("This WAMR build does not provide verified fuel, epoch, or linear-memory budget enforcement."));
			return false;
		}
		MaxHostCallsPerEntry = Config.ExecutionBudget.MaxHostCallsPerEntry;
		if (Config.BindingPackage != nullptr)
		{
			if (Config.HostDispatcher == nullptr)
			{
				SetVmError(
					OutError,
					TEXT("invalid_config"),
					TEXT("Dynamic binding packages require a host dispatcher."));
				return false;
			}
		}

		FAvidScriptWasmModuleLayout ModuleLayout;
		FString LayoutError;
		if (!InspectAvidScriptWasmModuleLayout(Bytecode, ModuleLayout, LayoutError))
		{
			SetVmError(OutError, TEXT("wasm_layout_invalid"), LayoutError);
			return false;
		}
		if (!ValidateAvidScriptVmImportContract(
			ModuleLayout,
			Config.BindingPackage,
			TConstArrayView<FAvidScriptVmExpectedImport>(),
			false,
			OutError))
		{
			return false;
		}
		if (Config.BindingPackage != nullptr)
		{
			AttachedBindingPackage = *Config.BindingPackage;
		}

		const double RuntimeInitStartSeconds = FPlatformTime::Seconds();
		if (!AcquireWamrLease(OutError))
		{
			return false;
		}
		LoadMetrics.RuntimeInitMs = MeasureElapsedMs(RuntimeInitStartSeconds);
		bOwnsRuntimeLease = true;
		if (Config.BindingPackage != nullptr)
		{
			if (!AcquireAvidScriptWamrDynamicImports(
				AttachedBindingPackage,
				DynamicRegistrations,
				OutError))
			{
				Unload();
				return false;
			}
			for (const FAvidScriptWamrDynamicRegistration& Registration : DynamicRegistrations)
			{
				DynamicOrdinals.Add(Registration.Attachment, Registration.Ordinal);
			}
		}

		ModuleId = InModuleId;
		HostDispatcher = Config.HostDispatcher;
		ModuleBuffer.Append(Bytecode.GetData(), Bytecode.Num());

		char ErrorBuffer[ErrorBufferSize] = {};
		const double ModuleLoadStartSeconds = FPlatformTime::Seconds();
		{
			FAvidScriptWamrNativeRegistryScope RegistryScope;
			Module = wasm_runtime_load(
				ModuleBuffer.GetData(),
				static_cast<uint32>(ModuleBuffer.Num()),
				ErrorBuffer,
				sizeof(ErrorBuffer));
		}
		LoadMetrics.ModuleLoadMs = MeasureElapsedMs(ModuleLoadStartSeconds);
		if (Module == nullptr)
		{
			SetVmError(OutError, TEXT("load_failed"), UTF8_TO_TCHAR(ErrorBuffer));
			Unload();
			return false;
		}

		const int32 ImportCount = static_cast<int32>(wasm_runtime_get_import_count(Module));
		if (ImportCount < 0)
		{
			SetVmError(OutError, TEXT("import_inspection_failed"), TEXT("wasm_runtime_get_import_count returned a negative value."));
			Unload();
			return false;
		}

		for (int32 ImportIndex = 0; ImportIndex < ImportCount; ++ImportIndex)
		{
			wasm_import_t ImportInfo = {};
			wasm_runtime_get_import_type(Module, ImportIndex, &ImportInfo);
			if (ImportInfo.kind == WASM_IMPORT_EXPORT_KIND_FUNC && !ImportInfo.linked)
			{
				OutError.Reset();
				OutError.Category = TEXT("missing_import");
				OutError.ImportModuleName = ImportInfo.module_name != nullptr ? UTF8_TO_TCHAR(ImportInfo.module_name) : TEXT("");
				OutError.ImportName = ImportInfo.name != nullptr ? UTF8_TO_TCHAR(ImportInfo.name) : TEXT("");
				OutError.Details = FString::Printf(
					TEXT("Required host import '%s.%s' was not registered."),
					*OutError.ImportModuleName,
					*OutError.ImportName);
				Unload();
				return false;
			}
		}

		const double ModuleInstantiateStartSeconds = FPlatformTime::Seconds();
		ModuleInstance = wasm_runtime_instantiate(
			Module,
			Config.StackSize,
			Config.HeapSize,
			ErrorBuffer,
			sizeof(ErrorBuffer));
		LoadMetrics.ModuleInstantiateMs = MeasureElapsedMs(ModuleInstantiateStartSeconds);
		if (ModuleInstance == nullptr)
		{
			SetVmError(OutError, TEXT("instantiate_failed"), UTF8_TO_TCHAR(ErrorBuffer));
			Unload();
			return false;
		}

		const double ExecEnvCreateStartSeconds = FPlatformTime::Seconds();
		ExecEnv = wasm_runtime_create_exec_env(ModuleInstance, Config.StackSize);
		LoadMetrics.ExecEnvCreateMs = MeasureElapsedMs(ExecEnvCreateStartSeconds);
		if (ExecEnv == nullptr)
		{
			SetVmError(OutError, TEXT("exec_env_failed"), TEXT("wasm_runtime_create_exec_env returned null."));
			Unload();
			return false;
		}

		wasm_runtime_set_user_data(ExecEnv, static_cast<IAvidScriptWamrHostBridge*>(this));
		return true;
#endif
	}

	bool ResolveExport(
		const FString& ExportName,
		FAvidScriptVmExportHandle& OutHandle,
		FAvidScriptVmError& OutError) override
	{
		OutError.Reset();
#if !AVIDSCRIPT_WITH_WAMR
		OutHandle = {};
		SetVmError(OutError, TEXT("backend_unavailable"), TEXT("WAMR artifacts are unavailable for this target."));
		return false;
#else
		if (!IsLoaded())
		{
			OutHandle = {};
			SetVmError(OutError, TEXT("invalid_state"), TEXT("ResolveExport requires a loaded VM instance."));
			return false;
		}

		const bool bResolved = ExportTable.ResolveOrCache(ExportName, [this, &ExportName]() -> void*
		{
			FTCHARToUTF8 ExportNameUtf8(*ExportName);
			return wasm_runtime_lookup_function(ModuleInstance, ExportNameUtf8.Get());
		}, OutHandle);
		if (!bResolved)
		{
			SetVmError(
				OutError,
				TEXT("missing_export"),
				FString::Printf(TEXT("Required export '%s' was not found."), *ExportName));
		}
		else
		{
			OutHandle.BackendInstanceIdentity = BackendInstanceIdentity;
			void* FunctionValue = nullptr;
			if (!ExportTable.TryGet(OutHandle, FunctionValue, OutError))
			{
				OutHandle = {};
				return false;
			}
			const wasm_function_inst_t Function =
				static_cast<wasm_function_inst_t>(FunctionValue);
			const uint32 ResultCount = wasm_func_get_result_count(
				Function,
				ModuleInstance);
			if (ResultCount > FAvidScriptVmCallResult::MaxCells)
			{
				OutHandle = {};
				SetVmError(
					OutError,
					TEXT("invalid_export"),
					TEXT("WAMR export result count exceeds the fixed VM result capacity."));
				return false;
			}
			TArray<wasm_valkind_t, TInlineAllocator<FAvidScriptVmCallResult::MaxCells>>
				ResultTypes;
			ResultTypes.SetNumUninitialized(ResultCount);
			if (ResultCount > 0)
			{
				wasm_func_get_result_types(
					Function,
					ModuleInstance,
					ResultTypes.GetData());
			}
			uint32 ResultCellCount = 0;
			for (const wasm_valkind_t ResultType : ResultTypes)
			{
				switch (ResultType)
				{
				case WASM_I32:
				case WASM_F32:
					++ResultCellCount;
					break;
				case WASM_I64:
				case WASM_F64:
					ResultCellCount += 2;
					break;
				default:
					OutHandle = {};
					SetVmError(
						OutError,
						TEXT("invalid_export"),
						TEXT("WAMR export results must use only core numeric ABI types."));
					return false;
				}
				if (ResultCellCount > FAvidScriptVmCallResult::MaxCells)
				{
					OutHandle = {};
					SetVmError(
						OutError,
						TEXT("invalid_export"),
						TEXT("WAMR export result cells exceed the fixed VM result capacity."));
					return false;
				}
			}
			if (ExportResultCellCounts.Num() <
				static_cast<int32>(OutHandle.Slot))
			{
				ExportResultCellCounts.SetNumZeroed(
					static_cast<int32>(OutHandle.Slot));
			}
			ExportResultCellCounts[OutHandle.Slot - 1] = ResultCellCount;
		}
		return bResolved;
#endif
	}

	bool ValidateExportSignature(
		const FAvidScriptVmExportHandle& Handle,
		const FAvidScriptVmAbiSignature& ExpectedSignature,
		FAvidScriptVmError& OutError) override
	{
		OutError.Reset();
#if !AVIDSCRIPT_WITH_WAMR
		static_cast<void>(Handle);
		static_cast<void>(ExpectedSignature);
		SetVmError(OutError, TEXT("backend_unavailable"), TEXT("WAMR artifacts are unavailable for this target."));
		return false;
#else
		if (!IsLoaded() || Handle.BackendInstanceIdentity != BackendInstanceIdentity)
		{
			SetVmError(OutError, TEXT("invalid_export"), TEXT("Export signature validation requires a handle from this loaded VM instance."));
			return false;
		}
		void* FunctionValue = nullptr;
		if (!ExportTable.TryGet(Handle, FunctionValue, OutError))
		{
			return false;
		}
		if (FunctionValue == nullptr)
		{
			SetVmError(OutError, TEXT("invalid_export"), TEXT("The cached WAMR export is no longer available."));
			return false;
		}
		const wasm_function_inst_t Function = static_cast<wasm_function_inst_t>(FunctionValue);
		const uint32 ParameterCount = wasm_func_get_param_count(Function, ModuleInstance);
		const uint32 ResultCount = wasm_func_get_result_count(Function, ModuleInstance);
		if (ParameterCount != static_cast<uint32>(ExpectedSignature.Parameters.Num())
			|| ResultCount != (ExpectedSignature.bHasResult ? 1u : 0u))
		{
			SetVmError(OutError, TEXT("invalid_export"), TEXT("WAMR export parameter or result count does not match the expected signature."));
			return false;
		}
		const auto MatchesKind = [](const wasm_valkind_t Actual, const EAvidScriptVmValueKind Expected)
		{
			switch (Expected)
			{
			case EAvidScriptVmValueKind::I32: return Actual == WASM_I32;
			case EAvidScriptVmValueKind::I64: return Actual == WASM_I64;
			case EAvidScriptVmValueKind::F32: return Actual == WASM_F32;
			case EAvidScriptVmValueKind::F64: return Actual == WASM_F64;
			default: return false;
			}
		};
		TArray<wasm_valkind_t, TInlineAllocator<8>> ParameterTypes;
		ParameterTypes.SetNumUninitialized(ExpectedSignature.Parameters.Num());
		if (ParameterCount > 0)
		{
			wasm_func_get_param_types(Function, ModuleInstance, ParameterTypes.GetData());
		}
		for (int32 Index = 0; Index < ParameterTypes.Num(); ++Index)
		{
			if (!MatchesKind(ParameterTypes[Index], ExpectedSignature.Parameters[Index]))
			{
				SetVmError(OutError, TEXT("invalid_export"), TEXT("WAMR export parameter kind does not match the expected signature."));
				return false;
			}
		}
		if (ExpectedSignature.bHasResult)
		{
			wasm_valkind_t ResultType;
			wasm_func_get_result_types(Function, ModuleInstance, &ResultType);
			if (!MatchesKind(ResultType, ExpectedSignature.Result))
			{
				SetVmError(OutError, TEXT("invalid_export"), TEXT("WAMR export result kind does not match the expected signature."));
				return false;
			}
		}
		return true;
#endif
	}

	bool PrepareExportCall(
		const FAvidScriptVmExportHandle& Handle,
		FAvidScriptVmPreparedExportCall& OutCall,
		FAvidScriptVmError& OutError) override
	{
		static_cast<void>(Handle);
		OutCall = FAvidScriptVmPreparedExportCall();
		OutError.Reset();
		OutError.Category = TEXT("prepared_export_unsupported");
		OutError.Details =
			TEXT("The WAMR backend uses the validated generic Call path.");
		return false;
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
		bHasPendingHostImportFailure = false;
		bHostCallBudgetExceeded = false;
		CurrentHostCallCount = 0;
		PendingHostImportName.Reset();
		PendingHostFailureCategory.Reset();
		PendingHostImportDetails.Reset();
#if !AVIDSCRIPT_WITH_WAMR
		SetVmError(OutError, TEXT("backend_unavailable"), TEXT("WAMR artifacts are unavailable for this target."));
		return false;
#else

		if (Frame.CellCount > FAvidScriptVmCallFrame::MaxCells)
		{
			SetVmError(OutError, TEXT("invalid_arguments"), TEXT("VM call frame exceeds its fixed cell capacity."));
			return false;
		}

		if (Handle.BackendInstanceIdentity != BackendInstanceIdentity)
		{
			const bool bBelongsToPreviousInstance = OwnedBackendInstanceIdentities.Contains(
				Handle.BackendInstanceIdentity);
			SetVmError(
				OutError,
				bBelongsToPreviousInstance ? TEXT("stale_export") : TEXT("foreign_export"),
				bBelongsToPreviousInstance
					? TEXT("The export handle belongs to an unloaded VM instance.")
					: TEXT("The export handle belongs to a different VM backend instance."));
			return false;
		}

		void* FunctionValue = nullptr;
		if (!ExportTable.TryGet(Handle, FunctionValue, OutError))
		{
			return false;
		}
		if (!IsLoaded())
		{
			SetVmError(OutError, TEXT("invalid_state"), TEXT("Call requires a loaded VM instance."));
			return false;
		}
		if (Handle.Slot == 0 ||
			Handle.Slot > static_cast<uint32>(ExportResultCellCounts.Num()))
		{
			SetVmError(
				OutError,
				TEXT("stale_export"),
				TEXT("The export result ABI is no longer available."));
			return false;
		}
		const uint32 ResultCellCount =
			ExportResultCellCounts[Handle.Slot - 1];

		uint32 Cells[FAvidScriptVmCallFrame::MaxCells] = {};
		FMemory::Memcpy(Cells, Frame.Cells, Frame.CellCount * sizeof(uint32));
		++ActiveCallDepth;
		const bool bCallSucceeded = wasm_runtime_call_wasm(
			ExecEnv,
			static_cast<wasm_function_inst_t>(FunctionValue),
			Frame.CellCount,
			Cells);
		const bool bUnloadRequestedDuringCall = bUnloadDeferred;
		FString Exception;
		TArray<FAvidScriptVmStackFrame> StackFrames;
		if (!bCallSucceeded)
		{
			Exception = GetWamrException(ModuleInstance);
			CaptureAvidScriptWamrCallStack(ExecEnv, StackFrames);
			wasm_runtime_clear_exception(ModuleInstance);
		}

		--ActiveCallDepth;
		if (ActiveCallDepth == 0 && bUnloadDeferred)
		{
			PerformUnload();
		}

		if (bUnloadRequestedDuringCall)
		{
			SetVmError(
				OutError,
				TEXT("reentrant_unload"),
				TEXT("VM unload was deferred until the active guest call unwound."));
			return false;
		}

		if (!bCallSucceeded)
		{
			if (bHasPendingHostImportFailure)
			{
				OutError.Reset();
				OutError.Category = bHostCallBudgetExceeded
					? TEXT("host_call_budget_exhausted")
					: PendingHostFailureCategory.IsEmpty()
						? TEXT("host_import_failed")
						: PendingHostFailureCategory;
				OutError.ImportModuleName = TEXT("avidscript");
				OutError.ImportName = PendingHostImportName;
				OutError.Details = PendingHostImportDetails;
				OutError.StackFrames = MoveTemp(StackFrames);
				return false;
			}
			SetVmError(OutError, TEXT("trap"), Exception);
			OutError.StackFrames = MoveTemp(StackFrames);
			return false;
		}

		if (OutResult != nullptr)
		{
			FMemory::Memcpy(
				OutResult->Cells,
				Cells,
				ResultCellCount * sizeof(uint32));
			OutResult->CellCount = ResultCellCount;
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
#if AVIDSCRIPT_WITH_WAMR
		return Module != nullptr && ModuleInstance != nullptr && ExecEnv != nullptr;
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
		return ExportTable.GetLookupCount();
	}
	const FAvidScriptVmLoadMetrics& GetLoadMetrics() const override
	{
		return LoadMetrics;
	}

	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) override
	{
		OutResult = FAvidScriptHostCallResult();
		if (!TryConsumeHostCallBudget())
		{
			OutResult.ErrorCategory = TEXT("host_call_budget_exhausted");
			OutResult.Details = TEXT("The guest exceeded the per-entry host-call budget.");
			return false;
		}
		if (HostDispatcher == nullptr)
		{
			OutResult.ErrorCategory = TEXT("host_import_failed");
			OutResult.Details = TEXT("No host dispatcher is attached to the VM instance.");
			return false;
		}
		return HostDispatcher->DispatchHostCall(Call, OutResult);
	}

	bool DispatchDynamicHostCall(
		const FAvidScriptWamrRawImportAttachment& Attachment,
		TConstArrayView<uint64> Arguments,
		int64& OutReturnValue,
		FString& OutFailureCategory,
		FString& OutFailureDetails) override
	{
		OutReturnValue = 0;
		OutFailureCategory.Reset();
		OutFailureDetails.Reset();
		if (!TryConsumeHostCallBudget())
		{
			OutFailureCategory = TEXT("host_call_budget_exhausted");
			OutFailureDetails = TEXT("The guest exceeded the per-entry host-call budget.");
			return false;
		}
		const uint32* Ordinal = DynamicOrdinals.Find(&Attachment);
		if (Ordinal == nullptr)
		{
			OutFailureCategory = TEXT("dynamic_host_attachment_invalid");
			OutFailureDetails = TEXT("The raw import is not attached to this VM binding package.");
			return false;
		}
		if (HostDispatcher == nullptr)
		{
			OutFailureCategory = TEXT("host_import_failed");
			OutFailureDetails = TEXT("No host dispatcher is attached to the VM instance.");
			return false;
		}
		FAvidScriptDynamicHostCall Call;
		Call.BindingOrdinal = *Ordinal;
		Call.Arguments = Arguments;
		Call.GuestMemory = this;
		FAvidScriptDynamicHostCallResult Result;
		if (!HostDispatcher->DispatchDynamicHostCall(Call, Result) || !Result.bSucceeded)
		{
			OutFailureCategory = Result.ErrorCategory.IsEmpty()
				? TEXT("host_import_failed")
				: Result.ErrorCategory;
			OutFailureDetails = Result.Details.IsEmpty()
				? FString::Printf(TEXT("Dynamic host dispatcher rejected ordinal %u."), *Ordinal)
				: Result.Details;
			return false;
		}
		OutReturnValue = Attachment.ResultKind == EAvidScriptWamrRawResultKind::I64
			? Result.ReturnValueI64
			: Result.ReturnValue;
		return true;
	}

	bool ReadBytes(
		uint32 GuestAddress,
		TArrayView<uint8> OutBytes,
		FString& OutError) override
	{
		OutError.Reset();
		if (OutBytes.IsEmpty())
		{
			return true;
		}
		#if !AVIDSCRIPT_WITH_WAMR
		OutError = TEXT("WAMR artifacts are unavailable for this target.");
		return false;
		#else
		if (ModuleInstance == nullptr)
		{
			OutError = FString::Printf(TEXT("Guest read range is invalid at %u for %d bytes."), GuestAddress, OutBytes.Num());
			return false;
		}
		if (wasm_runtime_get_exception(ModuleInstance) != nullptr)
		{
			OutError = TEXT("Guest memory cannot be read while the WAMR instance has a pending exception.");
			return false;
		}
		if (!wasm_runtime_validate_app_addr(ModuleInstance, GuestAddress, OutBytes.Num()))
		{
			OutError = FString::Printf(TEXT("Guest read range is invalid at %u for %d bytes."), GuestAddress, OutBytes.Num());
			wasm_runtime_clear_exception(ModuleInstance);
			return false;
		}
		const void* Source = wasm_runtime_addr_app_to_native(ModuleInstance, GuestAddress);
		if (Source == nullptr)
		{
			OutError = TEXT("WAMR could not translate the guest read address.");
			return false;
		}
		FMemory::Memcpy(OutBytes.GetData(), Source, OutBytes.Num());
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
#if !AVIDSCRIPT_WITH_WAMR
		OutError = TEXT("guest_memory_borrow_unavailable: WAMR artifacts are unavailable for this target.");
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
		if (ModuleInstance == nullptr)
		{
			OutError = TEXT("guest_memory_invalid: WAMR instance is unavailable for read-only borrowing.");
			return false;
		}
		if (wasm_runtime_get_exception(ModuleInstance) != nullptr)
		{
			OutError = TEXT("guest_memory_invalid: guest memory cannot be borrowed while WAMR has a pending exception.");
			return false;
		}
		if (!wasm_runtime_validate_app_addr(ModuleInstance, GuestAddress, ByteCount))
		{
			OutError = FString::Printf(
				TEXT("guest_memory_invalid: read-only borrow range is invalid at %u for %u bytes."),
				GuestAddress,
				ByteCount);
			wasm_runtime_clear_exception(ModuleInstance);
			return false;
		}
		const uint8* Data = static_cast<const uint8*>(
			wasm_runtime_addr_app_to_native(ModuleInstance, GuestAddress));
		if (Data == nullptr)
		{
			OutError = TEXT("guest_memory_invalid: WAMR could not translate the read-only borrow address.");
			return false;
		}
		OutBytes = MakeArrayView(Data, static_cast<int32>(ByteCount));
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
#if !AVIDSCRIPT_WITH_WAMR
		OutError = TEXT("guest_memory_borrow_unavailable: WAMR artifacts are unavailable for this target.");
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
		if (ModuleInstance == nullptr)
		{
			OutError = TEXT("guest_memory_invalid: WAMR instance is unavailable for mutable borrowing.");
			return false;
		}
		if (wasm_runtime_get_exception(ModuleInstance) != nullptr)
		{
			OutError = TEXT("guest_memory_invalid: guest memory cannot be borrowed while WAMR has a pending exception.");
			return false;
		}
		if (!wasm_runtime_validate_app_addr(ModuleInstance, GuestAddress, ByteCount))
		{
			OutError = FString::Printf(
				TEXT("guest_memory_invalid: mutable borrow range is invalid at %u for %u bytes."),
				GuestAddress,
				ByteCount);
			wasm_runtime_clear_exception(ModuleInstance);
			return false;
		}
		uint8* Data = static_cast<uint8*>(
			wasm_runtime_addr_app_to_native(ModuleInstance, GuestAddress));
		if (Data == nullptr)
		{
			OutError = TEXT("guest_memory_invalid: WAMR could not translate the mutable borrow address.");
			return false;
		}
		OutBytes = MakeArrayView(Data, static_cast<int32>(ByteCount));
		return true;
#endif
	}

	bool WriteBytes(
		uint32 GuestAddress,
		TConstArrayView<uint8> Bytes,
		FString& OutError) override
	{
		OutError.Reset();
		if (Bytes.IsEmpty())
		{
			return true;
		}
		#if !AVIDSCRIPT_WITH_WAMR
		OutError = TEXT("WAMR artifacts are unavailable for this target.");
		return false;
		#else
		if (ModuleInstance == nullptr)
		{
			OutError = FString::Printf(TEXT("Guest write range is invalid at %u for %d bytes."), GuestAddress, Bytes.Num());
			return false;
		}
		if (wasm_runtime_get_exception(ModuleInstance) != nullptr)
		{
			OutError = TEXT("Guest memory cannot be written while the WAMR instance has a pending exception.");
			return false;
		}
		if (!wasm_runtime_validate_app_addr(ModuleInstance, GuestAddress, Bytes.Num()))
		{
			OutError = FString::Printf(TEXT("Guest write range is invalid at %u for %d bytes."), GuestAddress, Bytes.Num());
			wasm_runtime_clear_exception(ModuleInstance);
			return false;
		}
		void* Destination = wasm_runtime_addr_app_to_native(ModuleInstance, GuestAddress);
		if (Destination == nullptr)
		{
			OutError = TEXT("WAMR could not translate the guest write address.");
			return false;
		}
		FMemory::Memcpy(Destination, Bytes.GetData(), Bytes.Num());
		return true;
		#endif
	}

	void RecordHostImportFailure(
		const char* ImportName,
		const FString& Category,
		const FString& Details) override
	{
		bHasPendingHostImportFailure = true;
		PendingHostImportName = UTF8_TO_TCHAR(ImportName);
		PendingHostFailureCategory = Category;
		PendingHostImportDetails = Details;
	}

private:
	bool TryConsumeHostCallBudget()
	{
		if (MaxHostCallsPerEntry == 0)
		{
			return true;
		}
		if (CurrentHostCallCount >= MaxHostCallsPerEntry)
		{
			bHostCallBudgetExceeded = true;
			return false;
		}
		++CurrentHostCallCount;
		return true;
	}

	void PerformUnload()
	{
#if AVIDSCRIPT_WITH_WAMR
		if (ExecEnv != nullptr)
		{
			wasm_runtime_set_user_data(ExecEnv, nullptr);
			wasm_runtime_destroy_exec_env(ExecEnv);
			ExecEnv = nullptr;
		}

		if (ModuleInstance != nullptr)
		{
			wasm_runtime_deinstantiate(ModuleInstance);
			ModuleInstance = nullptr;
		}

		if (Module != nullptr)
		{
			wasm_runtime_unload(Module);
			Module = nullptr;
		}

		ReleaseAvidScriptWamrDynamicImports(DynamicRegistrations);
		DynamicOrdinals.Reset();
		AttachedBindingPackage = FAvidScriptVmBindingPackage();

		if (bOwnsRuntimeLease)
		{
			ReleaseWamrLease();
			bOwnsRuntimeLease = false;
		}
#endif

		ExportTable.Reset();
		ExportResultCellCounts.Reset();
		ModuleBuffer.Reset();
		ModuleId.Reset();
		HostDispatcher = nullptr;
		MaxHostCallsPerEntry = 0;
		CurrentHostCallCount = 0;
		bHostCallBudgetExceeded = false;
		bUnloadDeferred = false;
		AdvanceBackendInstanceIdentity();
	}

	void AdvanceBackendInstanceIdentity()
	{
		BackendInstanceIdentity = AllocateBackendInstanceIdentity();
		OwnedBackendInstanceIdentities.Add(BackendInstanceIdentity);
	}

	FAvidScriptVmBackendInfo BackendInfo;
	uint64 BackendInstanceIdentity = 0;
	TSet<uint64> OwnedBackendInstanceIdentities;
	TArray<uint8> ModuleBuffer;
	FString ModuleId;
	IAvidScriptHostDispatcher* HostDispatcher = nullptr;
	FAvidScriptVmBindingPackage AttachedBindingPackage;
	TArray<FAvidScriptWamrDynamicRegistration> DynamicRegistrations;
	TMap<const FAvidScriptWamrRawImportAttachment*, uint32> DynamicOrdinals;
	FAvidScriptVmExportTable ExportTable;
	TArray<uint32> ExportResultCellCounts;
	FAvidScriptVmLoadMetrics LoadMetrics;
	bool bOwnsRuntimeLease = false;
	int32 ActiveCallDepth = 0;
	bool bUnloadDeferred = false;
	bool bHasPendingHostImportFailure = false;
	bool bHostCallBudgetExceeded = false;
	uint32 MaxHostCallsPerEntry = 0;
	uint32 CurrentHostCallCount = 0;
	FString PendingHostImportName;
	FString PendingHostFailureCategory;
	FString PendingHostImportDetails;

#if AVIDSCRIPT_WITH_WAMR
	wasm_module_t Module = nullptr;
	wasm_module_inst_t ModuleInstance = nullptr;
	wasm_exec_env_t ExecEnv = nullptr;
#endif
};
}

TUniquePtr<IAvidScriptVmBackend> CreateAvidScriptWamrBackend()
{
	return MakeUnique<FAvidScriptWamrBackend>();
}
