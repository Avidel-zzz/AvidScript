#include "AvidScriptWasmRuntime.h"

#include "AvidScriptSceneComponentBinding.h"
#include "Diagnostics/AvidScriptWasmDebugMap.h"


DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWasmRuntime, Log, All);

namespace
{
constexpr uint32 AvidScriptWasmStackSize = 64 * 1024;
constexpr uint32 AvidScriptWasmHeapSize = 64 * 1024;
constexpr uint32 AvidScriptWasmErrorBufferSize = 512;
constexpr double AvidScriptMinimumMeasuredMs = 0.0001;
constexpr int32 AvidScriptMaximumPendingTimers = 1024;
constexpr int32 AvidScriptTimerHeapCompactionThreshold = 64;

struct FAvidScriptTimerDeadlineLess
{
	bool operator()(const FAvidScriptWasmTimerEntry& Left, const FAvidScriptWasmTimerEntry& Right) const
	{
		return Left.DueTimeSeconds < Right.DueTimeSeconds
			|| (Left.DueTimeSeconds == Right.DueTimeSeconds && Left.Handle < Right.Handle);
	}
};

const uint8 GAvidScriptMinimalWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x07,
	0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

const uint8 GAvidScriptHostImportWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x0d, 0x03, 0x60, 0x01, 0x7f, 0x01, 0x7f,
	0x60, 0x00, 0x00, 0x60, 0x01, 0x7d, 0x00,
	0x02, 0x1b, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64,
	0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x0c, 0x68,
	0x6f, 0x73, 0x74, 0x5f, 0x61, 0x64, 0x64, 0x5f,
	0x69, 0x33, 0x32, 0x00, 0x00, 0x03, 0x03, 0x02,
	0x01, 0x02, 0x07, 0x25, 0x02, 0x12, 0x61, 0x76,
	0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65,
	0x67, 0x69, 0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79,
	0x00, 0x01, 0x0c, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x74, 0x69, 0x63, 0x6b, 0x00,
	0x02, 0x0a, 0x0c, 0x02, 0x07, 0x00, 0x41, 0x29,
	0x10, 0x00, 0x1a, 0x0b, 0x02, 0x00, 0x0b
};

double MeasureElapsedMs(double StartSeconds)
{
	return FMath::Max((FPlatformTime::Seconds() - StartSeconds) * 1000.0, AvidScriptMinimumMeasuredMs);
}

void PrepareResult(
	FAvidScriptWasmSmokeResult& OutResult,
	const FString& ModuleId,
	const FAvidScriptWasmRuntimeMetrics& Metrics)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = ModuleId;
	OutResult.Metrics = Metrics;
}

void SetFailure(
	FAvidScriptWasmSmokeResult& OutResult,
	const FString& ModuleId,
	const FString& ExportName,
	const FString& Category,
	const FString& Details,
	const FString& NextAction,
	const FString& ImportModuleName = FString(),
	const FString& ImportName = FString())
{
	OutResult.ModuleId = ModuleId;
	OutResult.ExportName = ExportName;
	OutResult.ImportModuleName = ImportModuleName;
	OutResult.ImportName = ImportName;
	OutResult.ErrorCategory = Category;
	OutResult.NextAction = NextAction;

	const FString ImportText = (!ImportModuleName.IsEmpty() || !ImportName.IsEmpty())
		? FString::Printf(TEXT(" | import=%s.%s"), ImportModuleName.IsEmpty() ? TEXT("<none>") : *ImportModuleName, ImportName.IsEmpty() ? TEXT("<none>") : *ImportName)
		: FString();

	OutResult.ErrorMessage = FString::Printf(
		TEXT("AvidScript VM error | backend=VM | module=%s | export=%s%s | category=%s | details=%s | next=%s"),
		ModuleId.IsEmpty() ? TEXT("<none>") : *ModuleId,
		ExportName.IsEmpty() ? TEXT("<none>") : *ExportName,
		*ImportText,
		*Category,
		*Details,
		*NextAction);

	UE_LOG(LogAvidScriptWasmRuntime, Warning, TEXT("%s"), *OutResult.ErrorMessage);
}

void SetFailureFromVmError(
	FAvidScriptWasmSmokeResult& OutResult,
	const FString& ModuleId,
	const FString& ExportName,
	const FAvidScriptVmError& Error,
	const FAvidScriptWasmDebugMap* DebugMap)
{
	const FString Category = Error.Category.IsEmpty() ? TEXT("vm_error") : Error.Category;
	FString NextAction = TEXT("reject this script instance and report the VM failure");
	if (Category == TEXT("missing_export"))
	{
		NextAction = TEXT("skip this script instance and report the guest ABI mismatch");
	}
	else if (Category == TEXT("host_import_failed"))
	{
		NextAction = TEXT("stop this script instance and surface the host import failure");
	}
	else if (Category == TEXT("trap"))
	{
		NextAction = TEXT("stop ticking this script instance and surface the trap to UE logs");
	}

	SetFailure(
		OutResult,
		ModuleId,
		ExportName,
		Category,
		Error.Details,
		NextAction,
		Error.ImportModuleName,
		Error.ImportName);

	if (DebugMap != nullptr)
	{
		DebugMap->MapFrames(Error.StackFrames, OutResult.DiagnosticFrames);
	}
	else
	{
		OutResult.DiagnosticFrames.Reset(Error.StackFrames.Num());
		for (const FAvidScriptVmStackFrame& VmFrame : Error.StackFrames)
		{
			FAvidScriptWasmDiagnosticFrame& Frame = OutResult.DiagnosticFrames.AddDefaulted_GetRef();
			Frame.FunctionIndex = VmFrame.FunctionIndex;
			Frame.FunctionOffset = VmFrame.FunctionOffset;
			Frame.RawFunctionToken = VmFrame.RawFunctionToken;
			Frame.FunctionName = VmFrame.RawFunctionToken;
		}
	}
}

bool CallVmExport(
	IAvidScriptVmBackend* Backend,
	FAvidScriptVmExportHandle& CachedHandle,
	const FString& ModuleId,
	const char* ExportName,
	uint32 ArgCount,
	const uint32* Args,
	const FAvidScriptWasmDebugMap* DebugMap,
	FAvidScriptWasmSmokeResult& OutResult)
{
	const FString ExportNameText(UTF8_TO_TCHAR(ExportName));
	if (Backend == nullptr)
	{
		FAvidScriptVmError Error;
		Error.Category = TEXT("backend_unavailable");
		Error.Details = TEXT("No VM backend is attached to the runtime instance.");
		SetFailureFromVmError(OutResult, ModuleId, ExportNameText, Error, DebugMap);
		return false;
	}

	if (ArgCount > FAvidScriptVmCallFrame::MaxCells)
	{
		FAvidScriptVmError Error;
		Error.Category = TEXT("invalid_arguments");
		Error.Details = TEXT("The runtime call exceeds the VM fixed cell capacity.");
		SetFailureFromVmError(OutResult, ModuleId, ExportNameText, Error, DebugMap);
		return false;
	}

	FAvidScriptVmError Error;
	if (!CachedHandle.IsValid() && !Backend->ResolveExport(ExportNameText, CachedHandle, Error))
	{
		SetFailureFromVmError(OutResult, ModuleId, ExportNameText, Error, DebugMap);
		return false;
	}

	FAvidScriptVmCallFrame Frame;
	Frame.CellCount = ArgCount;
	if (ArgCount > 0 && Args != nullptr)
	{
		FMemory::Memcpy(Frame.Cells, Args, ArgCount * sizeof(uint32));
	}
	if (!Backend->Call(CachedHandle, Frame, Error))
	{
		SetFailureFromVmError(OutResult, ModuleId, ExportNameText, Error, DebugMap);
		return false;
	}
	return true;
}
} // namespace

FAvidScriptWasmRuntimeInstance::~FAvidScriptWasmRuntimeInstance()
{
	Unload();
}

bool FAvidScriptWasmRuntimeInstance::ReadStateBytes(
	uint32 GuestAddress,
	TArrayView<uint8> OutBytes,
	FString& OutError) const
{
	OutError.Reset();
	IAvidScriptVmGuestMemory* GuestMemory = VmBackend ? VmBackend->GetGuestMemory() : nullptr;
	if (!IsLoaded() || GuestMemory == nullptr)
	{
		OutError = TEXT("Loaded VM guest memory is unavailable for state migration.");
		return false;
	}
	return GuestMemory->ReadBytes(GuestAddress, OutBytes, OutError);
}

bool FAvidScriptWasmRuntimeInstance::WriteStateBytes(
	uint32 GuestAddress,
	TConstArrayView<uint8> Bytes,
	FString& OutError)
{
	OutError.Reset();
	IAvidScriptVmGuestMemory* GuestMemory = VmBackend ? VmBackend->GetGuestMemory() : nullptr;
	if (!IsLoaded() || GuestMemory == nullptr)
	{
		OutError = TEXT("Loaded VM guest memory is unavailable for state migration.");
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	++StateWriteAttemptCount;
	if (StateWriteFailureAttempts.Contains(StateWriteAttemptCount))
	{
		OutError = TEXT("State write failure injected for automation coverage.");
		return false;
	}
#endif
	return GuestMemory->WriteBytes(GuestAddress, Bytes, OutError);
}

#if WITH_DEV_AUTOMATION_TESTS
void FAvidScriptWasmRuntimeInstance::SetStateWriteFailuresForTesting(TConstArrayView<int32> InWriteAttempts)
{
	StateWriteAttemptCount = 0;
	StateWriteFailureAttempts.Reset(InWriteAttempts.Num());
	for (const int32 WriteAttempt : InWriteAttempts)
	{
		if (WriteAttempt > 0)
		{
			StateWriteFailureAttempts.AddUnique(WriteAttempt);
		}
	}
}

void FAvidScriptWasmRuntimeInstance::ClearStateWriteFailureForTesting()
{
	StateWriteAttemptCount = 0;
	StateWriteFailureAttempts.Reset();
}
#endif

bool FAvidScriptWasmRuntimeInstance::LoadEmbeddedSmokeModule(FAvidScriptWasmSmokeResult& OutResult)
{
	return LoadModule(
		GAvidScriptMinimalWasmModule,
		UE_ARRAY_COUNT(GAvidScriptMinimalWasmModule),
		TEXT("embedded_smoke"),
		OutResult);
}

bool FAvidScriptWasmRuntimeInstance::LoadEmbeddedHostImportModule(FAvidScriptWasmSmokeResult& OutResult)
{
	return LoadModule(
		GAvidScriptHostImportWasmModule,
		UE_ARRAY_COUNT(GAvidScriptHostImportWasmModule),
		TEXT("embedded_host_import"),
		OutResult);
}

bool FAvidScriptWasmRuntimeInstance::LoadModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FString& InModuleId,
	FAvidScriptWasmSmokeResult& OutResult)
{
	return LoadModule(
		Bytecode,
		BytecodeSize,
		InModuleId,
		TSharedPtr<const FAvidScriptBindingPackage>(),
		TSharedPtr<const FAvidScriptWasmDebugMap>(),
		OutResult);
}

bool FAvidScriptWasmRuntimeInstance::LoadModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FString& InModuleId,
	const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage,
	FAvidScriptWasmSmokeResult& OutResult)

{
	return LoadModule(
		Bytecode,
		BytecodeSize,
		InModuleId,
		InBindingPackage,
		TSharedPtr<const FAvidScriptWasmDebugMap>(),
		OutResult);
}

bool FAvidScriptWasmRuntimeInstance::LoadModule(
	const uint8* Bytecode,
	int32 BytecodeSize,
	const FString& InModuleId,
	const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage,
	const TSharedPtr<const FAvidScriptWasmDebugMap>& InDebugMap,
	FAvidScriptWasmSmokeResult& OutResult)
{
	Unload();
	Metrics = FAvidScriptWasmRuntimeMetrics();
	ResetHostImportState();
	ModuleId = InModuleId;
	PrepareResult(OutResult, ModuleId, Metrics);
	CopyHostImportStateToResult(OutResult);

	if (Bytecode == nullptr || BytecodeSize <= 0)
	{
		SetFailure(OutResult, ModuleId, TEXT("<module>"), TEXT("invalid_bytecode"), TEXT("No WASM bytecode was provided"), TEXT("provide a non-empty WASM module buffer"));
		return false;
	}

	VmBackend = CreateAvidScriptWamrBackend();
	if (!VmBackend)
	{
		SetFailure(OutResult, ModuleId, TEXT("<runtime>"), TEXT("backend_unavailable"), TEXT("The VM backend factory returned null"), TEXT("verify the AvidScriptVM module is available for this target"));
		return false;
	}

	BindingPackage = InBindingPackage;
	DebugMap = InDebugMap;
	if (BindingPackage.IsValid())
	{
		BindingInvocationScratch.SetNumUninitialized(BindingPackage->GetRequiredScratchSize());
	}
	else
	{
		BindingInvocationScratch.Reset();
	}

	FAvidScriptVmLoadConfig Config;
	Config.HostDispatcher = this;
	Config.BindingPackage = BindingPackage.IsValid() ? &BindingPackage->GetVmPackage() : nullptr;
	FAvidScriptVmError Error;
	const bool bLoaded = VmBackend->Load(MakeArrayView(Bytecode, BytecodeSize), ModuleId, Config, Error);
	const FAvidScriptVmLoadMetrics& LoadMetrics = VmBackend->GetLoadMetrics();
	Metrics.RuntimeInitMs = LoadMetrics.RuntimeInitMs;
	Metrics.ModuleLoadMs = LoadMetrics.ModuleLoadMs;
	Metrics.ModuleInstantiateMs = LoadMetrics.ModuleInstantiateMs;
	Metrics.ExecEnvCreateMs = LoadMetrics.ExecEnvCreateMs;
	OutResult.Metrics = Metrics;
	if (!bLoaded)
	{
		SetFailureFromVmError(OutResult, ModuleId, TEXT("<module>"), Error, DebugMap.Get());
		VmBackend.Reset();
		BindingPackage.Reset();
		DebugMap.Reset();
		BindingInvocationScratch.Reset();
		return false;
	}

	OutResult.bRuntimeInitialized = true;
	OutResult.bModuleLoaded = true;
	OutResult.bModuleInstantiated = true;
	FAvidScriptLifecycleTransitionResult LifecycleResult;
	if (!LifecycleState.TryTransition(EAvidScriptLifecycleState::Loaded, LifecycleResult))
	{
		SetFailure(OutResult, ModuleId, TEXT("<lifecycle>"), TEXT("invalid_state"), TEXT("The runtime lifecycle rejected the Loaded transition"), TEXT("unload the session and create a fresh runtime instance"));
		Unload();
		return false;
	}
	return true;
}
bool FAvidScriptWasmRuntimeInstance::ValidateRequiredExports(
	const TArray<FString>& RequiredExports,
	FAvidScriptWasmSmokeResult& OutResult) const
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = IsLoaded();
	OutResult.bModuleLoaded = IsLoaded();
	OutResult.bModuleInstantiated = IsLoaded();
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	OutResult.TickCallCount = TickCallCount;
	CopyHostImportStateToResult(OutResult);

	if (!IsLoaded())
	{
		SetFailure(OutResult, ModuleId, TEXT("<module>"), TEXT("invalid_state"), TEXT("No WASM module is loaded"), TEXT("load a module before validating required exports"));
		return false;
	}

	for (const FString& RequiredExport : RequiredExports)
	{
		if (RequiredExport.IsEmpty())
		{
			SetFailure(OutResult, ModuleId, TEXT("<manifest>"), TEXT("missing_export"), TEXT("Required export name is empty"), TEXT("fix the reload manifest before activating this script"));
			return false;
		}

		FAvidScriptVmError Error;
		FAvidScriptVmExportHandle Handle;
		if (!VmBackend->ResolveExport(RequiredExport, Handle, Error))
		{
			SetFailureFromVmError(OutResult, ModuleId, RequiredExport, Error, DebugMap.Get());
			return false;
		}
	}
	return true;
}
bool FAvidScriptWasmRuntimeInstance::BeginPlay(FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = IsLoaded();
	OutResult.bModuleLoaded = IsLoaded();
	OutResult.bModuleInstantiated = IsLoaded();
	OutResult.bEndPlayCalled = bHasEndedPlay;
	CopyObservableStateToResult(OutResult);

	if (!IsLoaded())
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_begin_play"),
			TEXT("invalid_state"),
			TEXT("No WASM module is loaded"),
			TEXT("load a module before calling BeginPlay"));
		return false;
	}

	if (LifecycleState.GetState() != EAvidScriptLifecycleState::Loaded)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_begin_play"),
			TEXT("invalid_state"),
			TEXT("BeginPlay requires the Loaded lifecycle state"),
			TEXT("start each loaded runtime exactly once"));
		return false;
	}

	FAvidScriptLifecycleTransitionResult LifecycleResult;
	if (!LifecycleState.TryTransition(EAvidScriptLifecycleState::Starting, LifecycleResult))
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_begin_play"),
			TEXT("invalid_state"),
			TEXT("BeginPlay lifecycle transition was rejected"),
			TEXT("unload the session and create a fresh runtime instance"));
		return false;
	}

	const double BeginPlayStartSeconds = FPlatformTime::Seconds();
	if (!CallVmExport(
		VmBackend.Get(),
		BeginPlayExport,
		ModuleId,
		"avid_on_begin_play",
		0,
		nullptr,
		DebugMap.Get(),
		OutResult))
	{
		Metrics.BeginPlayCallMs = MeasureElapsedMs(BeginPlayStartSeconds);
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	Metrics.BeginPlayCallMs = MeasureElapsedMs(BeginPlayStartSeconds);
	bHasBegunPlay = true;
	bHasEndedPlay = false;
	bEndPlayAttempted = false;
	bEndPlaySucceeded = false;
	CachedEndPlayResult = FAvidScriptWasmSmokeResult();
	LifecycleState.TryTransition(EAvidScriptLifecycleState::Running, LifecycleResult);
	OutResult.Metrics = Metrics;
	OutResult.bBeginPlayCalled = true;
	OutResult.TickCallCount = TickCallCount;
	CopyObservableStateToResult(OutResult);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = IsLoaded();
	OutResult.bModuleLoaded = IsLoaded();
	OutResult.bModuleInstantiated = IsLoaded();
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;

	if (!IsLoaded())
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_tick"),
			TEXT("invalid_state"),
			TEXT("No WASM module is loaded"),
			TEXT("load a module before ticking"));
		CopyObservableStateToResult(OutResult);
		return false;
	}

	if (LifecycleState.GetState() != EAvidScriptLifecycleState::Running)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_tick"),
			TEXT("invalid_state"),
			TEXT("Tick requires the Running lifecycle state"),
			TEXT("call BeginPlay successfully before ticking"));
		CopyObservableStateToResult(OutResult);
		return false;
	}

	CollectDueTimers(DeltaSeconds);
	Metrics.TimerCallbackCallMs = 0.0;

	uint32 TickArgs[1] = {};
	static_assert(sizeof(TickArgs[0]) == sizeof(DeltaSeconds), "VM f32 argument must fit in one cell.");
	FMemory::Memcpy(&TickArgs[0], &DeltaSeconds, sizeof(DeltaSeconds));

	const double TickStartSeconds = FPlatformTime::Seconds();
	if (!CallVmExport(
		VmBackend.Get(),
		TickExport,
		ModuleId,
		"avid_on_tick",
		UE_ARRAY_COUNT(TickArgs),
		TickArgs,
		DebugMap.Get(),
		OutResult))
	{
		Metrics.TickCallMs = MeasureElapsedMs(TickStartSeconds);
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	Metrics.TickCallMs = MeasureElapsedMs(TickStartSeconds);
	++TickCallCount;
	OutResult.Metrics = Metrics;
	OutResult.bTickCalled = true;
	OutResult.TickCallCount = TickCallCount;

	if (!ExecuteDueTimerCallbacks(OutResult))
	{
		OutResult.Metrics = Metrics;
		OutResult.bTickCalled = true;
		OutResult.TickCallCount = TickCallCount;
		CopyObservableStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	OutResult.Metrics = Metrics;
	CopyObservableStateToResult(OutResult);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchEvent(
	int32 EventId,
	float Value,
	FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = IsLoaded();
	OutResult.bModuleLoaded = IsLoaded();
	OutResult.bModuleInstantiated = IsLoaded();
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	OutResult.TickCallCount = TickCallCount;
	CopyObservableStateToResult(OutResult);

	if (!IsLoaded() || !bHasBegunPlay || bEndPlayAttempted)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_event"),
			TEXT("invalid_state"),
			TEXT("Gameplay events require an active runtime between BeginPlay and EndPlay"),
			TEXT("dispatch events only while the AvidScript component is actively playing"));
		return false;
	}

	if (EventId < 0 || !FMath::IsFinite(Value))
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_event"),
			TEXT("invalid_argument"),
			FString::Printf(TEXT("Invalid gameplay event payload | id=%d | value=%g"), EventId, Value),
			TEXT("use a non-negative event id and a finite float value"));
		return false;
	}

	uint32 EventArgs[2] = { static_cast<uint32>(EventId), 0 };
	static_assert(sizeof(EventArgs[1]) == sizeof(Value), "VM f32 argument must fit in one cell.");
	FMemory::Memcpy(&EventArgs[1], &Value, sizeof(Value));

	const double EventStartSeconds = FPlatformTime::Seconds();
	if (!CallVmExport(
		VmBackend.Get(),
		EventExport,
		ModuleId,
		"avid_on_event",
		UE_ARRAY_COUNT(EventArgs),
		EventArgs,
		DebugMap.Get(),
		OutResult))
	{
		Metrics.EventCallbackCallMs = MeasureElapsedMs(EventStartSeconds);
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	Metrics.EventCallbackCallMs = MeasureElapsedMs(EventStartSeconds);
	++EventCallbackCount;
	LastEventId = EventId;
	LastEventValue = Value;
	OutResult.Metrics = Metrics;
	CopyObservableStateToResult(OutResult);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchGameplayEvent(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutResult)
{
	constexpr const TCHAR* ExportName = TEXT("avid_on_gameplay_event");
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = IsLoaded();
	OutResult.bModuleLoaded = IsLoaded();
	OutResult.bModuleInstantiated = IsLoaded();
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	OutResult.TickCallCount = TickCallCount;
	CopyObservableStateToResult(OutResult);

	if (!IsLoaded() || !bHasBegunPlay || bEndPlayAttempted)
	{
		SetFailure(
			OutResult,
			ModuleId,
			ExportName,
			TEXT("invalid_state"),
			TEXT("Typed gameplay events require an active runtime between BeginPlay and EndPlay"),
			TEXT("dispatch typed events only while the AvidScript session is Running"));
		return false;
	}

	const uint8 EventTypeValue = static_cast<uint8>(Event.Type);
	const bool bKnownType = EventTypeValue <= static_cast<uint8>(EAvidScriptGameplayEventType::Input);
	const bool bRequiresObjectHandle =
		Event.Type == EAvidScriptGameplayEventType::BeginOverlap ||
		Event.Type == EAvidScriptGameplayEventType::EndOverlap ||
		Event.Type == EAvidScriptGameplayEventType::Hit;
	if (!bKnownType || Event.PrimaryId < 0 || Event.SecondaryId < 0 ||
		(bRequiresObjectHandle && !Event.ObjectHandle.IsValid()) ||
		!FMath::IsFinite(Event.VectorValue.X) ||
		!FMath::IsFinite(Event.VectorValue.Y) ||
		!FMath::IsFinite(Event.VectorValue.Z))
	{
		SetFailure(
			OutResult,
			ModuleId,
			ExportName,
			TEXT("invalid_argument"),
			FString::Printf(
				TEXT("Invalid typed gameplay event | type=%u | primary=%d | secondary=%d | slot=%u | generation=%u"),
				EventTypeValue,
				Event.PrimaryId,
				Event.SecondaryId,
				Event.ObjectHandle.Slot,
				Event.ObjectHandle.Generation),
			TEXT("provide a known event type, non-negative ids, finite values, and a valid object handle when required"));
		return false;
	}

	if (!bGameplayEventExportLookupAttempted)
	{
		bGameplayEventExportLookupAttempted = true;
		FAvidScriptVmError ResolveError;
		if (!VmBackend->ResolveExport(ExportName, GameplayEventExport, ResolveError) &&
			ResolveError.Category != TEXT("missing_export"))
		{
			SetFailureFromVmError(OutResult, ModuleId, ExportName, ResolveError, DebugMap.Get());
			FAvidScriptLifecycleTransitionResult LifecycleResult;
			LifecycleState.MarkFaulted(LifecycleResult);
			return false;
		}
	}

	if (!GameplayEventExport.IsValid())
	{
		return true;
	}

	uint32 EventArgs[FAvidScriptVmCallFrame::MaxCells] = {
		static_cast<uint32>(EventTypeValue),
		static_cast<uint32>(Event.PrimaryId),
		static_cast<uint32>(Event.SecondaryId),
		Event.ObjectHandle.Slot,
		Event.ObjectHandle.Generation,
		0,
		0,
		0
	};
	FMemory::Memcpy(&EventArgs[5], &Event.VectorValue.X, sizeof(float));
	FMemory::Memcpy(&EventArgs[6], &Event.VectorValue.Y, sizeof(float));
	FMemory::Memcpy(&EventArgs[7], &Event.VectorValue.Z, sizeof(float));

	const double EventStartSeconds = FPlatformTime::Seconds();
	if (!CallVmExport(
			VmBackend.Get(),
			GameplayEventExport,
			ModuleId,
			"avid_on_gameplay_event",
			UE_ARRAY_COUNT(EventArgs),
			EventArgs,
			DebugMap.Get(),
			OutResult))
	{
		Metrics.EventCallbackCallMs = MeasureElapsedMs(EventStartSeconds);
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	Metrics.EventCallbackCallMs = MeasureElapsedMs(EventStartSeconds);
	++EventCallbackCount;
	LastEventId = EventTypeValue;
	LastEventValue = Event.VectorValue.X;
	OutResult.Metrics = Metrics;
	CopyObservableStateToResult(OutResult);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::EndPlay(FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = IsLoaded();
	OutResult.bModuleLoaded = IsLoaded();
	OutResult.bModuleInstantiated = IsLoaded();
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	OutResult.TickCallCount = TickCallCount;
	CopyObservableStateToResult(OutResult);

	if (!IsLoaded())
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_end_play"),
			TEXT("invalid_state"),
			TEXT("No WASM module is loaded"),
			TEXT("load a module before calling EndPlay"));
		return false;
	}

	if (bEndPlayAttempted)
	{
		OutResult = CachedEndPlayResult;
		return bEndPlaySucceeded;
	}

	if (LifecycleState.GetState() != EAvidScriptLifecycleState::Running)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_end_play"),
			TEXT("invalid_state"),
			TEXT("EndPlay requires the Running lifecycle state"),
			TEXT("call EndPlay only after a successful BeginPlay"));
		return false;
	}

	FAvidScriptLifecycleTransitionResult LifecycleResult;
	if (!LifecycleState.TryTransition(EAvidScriptLifecycleState::Stopping, LifecycleResult))
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_end_play"),
			TEXT("invalid_state"),
			TEXT("EndPlay lifecycle transition was rejected"),
			TEXT("unload the session and create a fresh runtime instance"));
		return false;
	}

	bEndPlayAttempted = true;
	FAvidScriptVmError EndPlayResolveError;
	if (!EndPlayExport.IsValid() && !VmBackend->ResolveExport(TEXT("avid_on_end_play"), EndPlayExport, EndPlayResolveError))
	{
		Metrics.EndPlayCallMs = 0.0;
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
		bEndPlaySucceeded = true;
		LifecycleState.TryTransition(EAvidScriptLifecycleState::Stopped, LifecycleResult);
		CachedEndPlayResult = OutResult;
		return true;
	}

	const double EndPlayStartSeconds = FPlatformTime::Seconds();
	if (!CallVmExport(
		VmBackend.Get(),
		EndPlayExport,
		ModuleId,
		"avid_on_end_play",
		0,
		nullptr,
		DebugMap.Get(),
		OutResult))
	{
		Metrics.EndPlayCallMs = MeasureElapsedMs(EndPlayStartSeconds);
		OutResult.Metrics = Metrics;
		OutResult.bBeginPlayCalled = bHasBegunPlay;
		OutResult.bEndPlayCalled = false;
		OutResult.TickCallCount = TickCallCount;
		CopyObservableStateToResult(OutResult);
		bEndPlaySucceeded = false;
		LifecycleState.MarkFaulted(LifecycleResult);
		CachedEndPlayResult = OutResult;
		return false;
	}

	Metrics.EndPlayCallMs = MeasureElapsedMs(EndPlayStartSeconds);
	bHasEndedPlay = true;
	bEndPlaySucceeded = true;
	LifecycleState.TryTransition(EAvidScriptLifecycleState::Stopped, LifecycleResult);
	OutResult.Metrics = Metrics;
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = true;
	OutResult.TickCallCount = TickCallCount;
	CopyObservableStateToResult(OutResult);
	CachedEndPlayResult = OutResult;
	return true;
}

void FAvidScriptWasmRuntimeInstance::Unload()
{
	FAvidScriptWasmSmokeResult IgnoredResult;
	Unload(IgnoredResult);
}

void FAvidScriptWasmRuntimeInstance::Unload(FAvidScriptWasmSmokeResult& OutResult)
{
	const FString PreviousModuleId = ModuleId;
	const bool bWasRuntimeInitialized = IsLoaded();
	const bool bWasModuleLoaded = IsLoaded();
	const bool bWasModuleInstantiated = IsLoaded();
	const bool bHadBegunPlay = bHasBegunPlay;
	const bool bHadEndedPlay = bHasEndedPlay;
	const int32 PreviousTickCallCount = TickCallCount;
	const int32 PreviousTimerCallbackCount = TimerCallbackCount;
	const int32 PreviousLastTimerCallbackId = LastTimerCallbackId;
	const int32 PreviousLastTimerHandle = LastTimerHandle;
	const int32 PreviousEventCallbackCount = EventCallbackCount;
	const int32 PreviousLastEventId = LastEventId;
	const float PreviousLastEventValue = LastEventValue;
	const int32 PreviousHostImportCallCount = HostImportCallCount;
	const int32 PreviousHostImportInput = LastHostImportInput;
	const int32 PreviousHostImportResult = LastHostImportResult;
	const bool bHadResources = bWasRuntimeInitialized || bWasModuleLoaded || bWasModuleInstantiated;
	const double UnloadStartSeconds = FPlatformTime::Seconds();

	if (VmBackend)
	{
		VmBackend->Unload();
		VmBackend.Reset();
	}
	BindingPackage.Reset();
	DebugMap.Reset();
	BindingInvocationScratch.Reset();
	BeginPlayExport = {};
	TickExport = {};
	EndPlayExport = {};
	TimerExport = {};
	EventExport = {};
	GameplayEventExport = {};
	bGameplayEventExportLookupAttempted = false;
	ModuleId.Empty();
	bHasBegunPlay = false;
	bHasEndedPlay = false;
	bEndPlayAttempted = false;
	bEndPlaySucceeded = false;
	CachedEndPlayResult = FAvidScriptWasmSmokeResult();
	TickCallCount = 0;
	ResetTimerState();
	ResetEventState();
	ResetHostImportState();
	LifecycleState.Reset();

	Metrics.UnloadMs = bHadResources ? MeasureElapsedMs(UnloadStartSeconds) : 0.0;
	PrepareResult(OutResult, PreviousModuleId, Metrics);
	OutResult.bRuntimeInitialized = bWasRuntimeInitialized;
	OutResult.bModuleLoaded = bWasModuleLoaded;
	OutResult.bModuleInstantiated = bWasModuleInstantiated;
	OutResult.bBeginPlayCalled = bHadBegunPlay;
	OutResult.bEndPlayCalled = bHadEndedPlay;
	OutResult.bTickCalled = PreviousTickCallCount > 0;
	OutResult.TickCallCount = PreviousTickCallCount;
	OutResult.bTimerCallbackCalled = PreviousTimerCallbackCount > 0;
	OutResult.TimerCallbackCount = PreviousTimerCallbackCount;
	OutResult.LastTimerCallbackId = PreviousLastTimerCallbackId;
	OutResult.LastTimerHandle = PreviousLastTimerHandle;
	OutResult.bEventCallbackCalled = PreviousEventCallbackCount > 0;
	OutResult.EventCallbackCount = PreviousEventCallbackCount;
	OutResult.LastEventId = PreviousLastEventId;
	OutResult.LastEventValue = PreviousLastEventValue;
	OutResult.HostImportCallCount = PreviousHostImportCallCount;
	OutResult.LastHostImportInput = PreviousHostImportInput;
	OutResult.LastHostImportResult = PreviousHostImportResult;
	OutResult.bUnloaded = true;
}

bool FAvidScriptWasmRuntimeInstance::IsLoaded() const
{
	return VmBackend && VmBackend->IsLoaded();
}

void FAvidScriptWasmRuntimeInstance::SetHostContext(const FAvidScriptWasmHostContext& InHostContext)
{
	HostContext = InHostContext;
}

void FAvidScriptWasmRuntimeInstance::ClearHostContext()
{
	HostContext = FAvidScriptWasmHostContext();
}

int32 FAvidScriptWasmRuntimeInstance::HandleOwnerGetSlotImport()
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = 0;
	LastHostImportResult = 0;
	++HostImportCallCount;

	if (HostContext.ObjectRegistry == nullptr || !HostContext.OwnerHandle.IsValid())
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("owner_get_slot"),
			TEXT("Missing valid owner handle context for avidscript.owner_get_slot"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FAvidScriptObjectHandleResult ResolveResult;
	if (HostContext.ObjectRegistry->ResolveObject(HostContext.OwnerHandle, ResolveResult) == nullptr ||
		HostContext.OwnerHandle.Slot > static_cast<uint32>(MAX_int32))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("owner_get_slot"),
			ResolveResult.ErrorMessage.IsEmpty()
				? TEXT("Owner handle slot cannot be represented by the i32 host ABI")
				: ResolveResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = static_cast<int32>(HostContext.OwnerHandle.Slot);
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleOwnerGetGenerationImport()
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = 0;
	LastHostImportResult = 0;
	++HostImportCallCount;

	if (HostContext.ObjectRegistry == nullptr || !HostContext.OwnerHandle.IsValid())
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("owner_get_generation"),
			TEXT("Missing valid owner handle context for avidscript.owner_get_generation"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FAvidScriptObjectHandleResult ResolveResult;
	if (HostContext.ObjectRegistry->ResolveObject(HostContext.OwnerHandle, ResolveResult) == nullptr ||
		HostContext.OwnerHandle.Generation > static_cast<uint32>(MAX_int32))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("owner_get_generation"),
			ResolveResult.ErrorMessage.IsEmpty()
				? TEXT("Owner handle generation cannot be represented by the i32 host ABI")
				: ResolveResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = static_cast<int32>(HostContext.OwnerHandle.Generation);
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleActorGetLocationImport(int32 Slot, int32 Generation, FVector& OutLocation)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;
	OutLocation = FVector::ZeroVector;

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_location"),
			TEXT("Missing host object registry for avidscript.actor_get_location"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_location"),
			FString::Printf(TEXT("Invalid actor handle for avidscript.actor_get_location | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FAvidScriptObjectHandle ActorHandle;
	ActorHandle.Slot = static_cast<uint32>(Slot);
	ActorHandle.Generation = static_cast<uint32>(Generation);

	FAvidScriptActorBindingResult BindingResult;
	if (!FAvidScriptActorBinding::GetActorLocation(*HostContext.ObjectRegistry, ActorHandle, OutLocation, BindingResult))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_location"),
			BindingResult.ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("Actor location read failed for avidscript.actor_get_location | slot=%d | generation=%d"), Slot, Generation)
				: BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleActorSetLocationImport(int32 Slot, int32 Generation, const FVector& Location)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_set_location"),
			TEXT("Missing host object registry for avidscript.actor_set_location"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_set_location"),
			FString::Printf(TEXT("Invalid actor handle for avidscript.actor_set_location | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FAvidScriptObjectHandle ActorHandle;
	ActorHandle.Slot = static_cast<uint32>(Slot);
	ActorHandle.Generation = static_cast<uint32>(Generation);

	FAvidScriptActorBindingResult BindingResult;
	if (!FAvidScriptActorBinding::SetActorLocation(
		*HostContext.ObjectRegistry,
		ActorHandle,
		Location,
		HostContext.ActorWritePolicy,
		BindingResult,
		HostContext.HostEffectJournal))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_set_location"),
			BindingResult.ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("Actor location write failed for avidscript.actor_set_location | slot=%d | generation=%d"), Slot, Generation)
				: BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleActorAddLocationOffsetImport(int32 Slot, int32 Generation, const FVector& Offset)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_add_location_offset"),
			TEXT("Missing host object registry for avidscript.actor_add_location_offset"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_add_location_offset"),
			FString::Printf(TEXT("Invalid actor handle for avidscript.actor_add_location_offset | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FAvidScriptObjectHandle ActorHandle;
	ActorHandle.Slot = static_cast<uint32>(Slot);
	ActorHandle.Generation = static_cast<uint32>(Generation);

	FAvidScriptActorBindingResult BindingResult;
	if (!FAvidScriptActorBinding::AddActorLocationOffset(
		*HostContext.ObjectRegistry,
		ActorHandle,
		Offset,
		HostContext.ActorWritePolicy,
		BindingResult,
		HostContext.HostEffectJournal))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_add_location_offset"),
			BindingResult.ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("Actor location offset failed for avidscript.actor_add_location_offset | slot=%d | generation=%d"), Slot, Generation)
				: BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleActorGetRotationImport(int32 Slot, int32 Generation, FRotator& OutRotation)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;
	OutRotation = FRotator::ZeroRotator;

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_rotation"),
			TEXT("Missing host object registry for avidscript.actor_get_rotation"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_rotation"),
			FString::Printf(TEXT("Invalid actor handle for avidscript.actor_get_rotation | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FAvidScriptObjectHandle ActorHandle;
	ActorHandle.Slot = static_cast<uint32>(Slot);
	ActorHandle.Generation = static_cast<uint32>(Generation);

	FAvidScriptActorBindingResult BindingResult;
	if (!FAvidScriptActorBinding::GetActorRotation(*HostContext.ObjectRegistry, ActorHandle, OutRotation, BindingResult))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_rotation"),
			BindingResult.ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("Actor rotation read failed for avidscript.actor_get_rotation | slot=%d | generation=%d"), Slot, Generation)
				: BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleActorSetRotationImport(int32 Slot, int32 Generation, const FRotator& Rotation)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_set_rotation"),
			TEXT("Missing host object registry for avidscript.actor_set_rotation"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_set_rotation"),
			FString::Printf(TEXT("Invalid actor handle for avidscript.actor_set_rotation | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FAvidScriptObjectHandle ActorHandle;
	ActorHandle.Slot = static_cast<uint32>(Slot);
	ActorHandle.Generation = static_cast<uint32>(Generation);

	FAvidScriptActorBindingResult BindingResult;
	if (!FAvidScriptActorBinding::SetActorRotation(
		*HostContext.ObjectRegistry,
		ActorHandle,
		Rotation,
		HostContext.ActorWritePolicy,
		BindingResult,
		HostContext.HostEffectJournal))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_set_rotation"),
			BindingResult.ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("Actor rotation write failed for avidscript.actor_set_rotation | slot=%d | generation=%d"), Slot, Generation)
				: BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleActorGetScaleImport(int32 Slot, int32 Generation, FVector& OutScale3D)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;
	OutScale3D = FVector::ZeroVector;

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_scale"), TEXT("Missing host object registry for avidscript.actor_get_scale"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_scale"), FString::Printf(TEXT("Invalid actor handle for avidscript.actor_get_scale | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FAvidScriptObjectHandle ActorHandle;
	ActorHandle.Slot = static_cast<uint32>(Slot);
	ActorHandle.Generation = static_cast<uint32>(Generation);

	FAvidScriptActorBindingResult BindingResult;
	if (!FAvidScriptActorBinding::GetActorScale3D(*HostContext.ObjectRegistry, ActorHandle, OutScale3D, BindingResult))
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_scale"), BindingResult.ErrorMessage.IsEmpty() ? FString::Printf(TEXT("Actor scale read failed | slot=%d | generation=%d"), Slot, Generation) : BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleActorSetScaleImport(int32 Slot, int32 Generation, const FVector& Scale3D)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_set_scale"), TEXT("Missing host object registry for avidscript.actor_set_scale"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_set_scale"), FString::Printf(TEXT("Invalid actor handle for avidscript.actor_set_scale | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FAvidScriptObjectHandle ActorHandle;
	ActorHandle.Slot = static_cast<uint32>(Slot);
	ActorHandle.Generation = static_cast<uint32>(Generation);

	FAvidScriptActorBindingResult BindingResult;
	if (!FAvidScriptActorBinding::SetActorScale3D(
		*HostContext.ObjectRegistry,
		ActorHandle,
		Scale3D,
		HostContext.ActorWritePolicy,
		BindingResult,
		HostContext.HostEffectJournal))
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_set_scale"), BindingResult.ErrorMessage.IsEmpty() ? FString::Printf(TEXT("Actor scale write failed | slot=%d | generation=%d"), Slot, Generation) : BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

bool FAvidScriptWasmRuntimeInstance::HandleActorGetTransformBatchImport(
	int32 RequestedCount,
	TConstArrayView<uint32> InputCells,
	TArrayView<float> OutputFloats,
	int32& OutProcessedCount)
{
	constexpr int32 InputCellsPerTransform = 2;
	constexpr int32 OutputFloatsPerTransform = 9;
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = RequestedCount;
	LastHostImportResult = 0;
	++HostImportCallCount;
	OutProcessedCount = 0;

	const int64 ExpectedInputCellCount = static_cast<int64>(RequestedCount) * InputCellsPerTransform;
	const int64 ExpectedOutputFloatCount = static_cast<int64>(RequestedCount) * OutputFloatsPerTransform;
	if (RequestedCount < 0 || RequestedCount > AvidScriptMaximumActorTransformBatchSize ||
		ExpectedInputCellCount != InputCells.Num() || ExpectedOutputFloatCount != OutputFloats.Num())
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_transform_batch"),
			FString::Printf(
				TEXT("Invalid transform batch wire shape | count=%d | input_cells=%d | output_floats=%d"),
				RequestedCount,
				InputCells.Num(),
				OutputFloats.Num()));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return false;
	}

	if (RequestedCount == 0)
	{
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return true;
	}

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_transform_batch"),
			TEXT("Missing host object registry for avidscript.actor_get_transform_batch"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return false;
	}

	TransformBatchHandleScratch.Reset(RequestedCount);
	for (int32 Index = 0; Index < RequestedCount; ++Index)
	{
		const int32 CellIndex = Index * InputCellsPerTransform;
		TransformBatchHandleScratch.Add(FAvidScriptObjectHandle{
			InputCells[CellIndex],
			InputCells[CellIndex + 1]
		});
	}

	FAvidScriptActorTransformBatchResult BatchResult;
	if (!FAvidScriptActorBinding::GetActorTransforms(
			*HostContext.ObjectRegistry,
			TransformBatchHandleScratch,
			TransformBatchSnapshotScratch,
			BatchResult))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_transform_batch"),
			BatchResult.ErrorMessage.IsEmpty()
				? FString::Printf(TEXT("Actor transform batch failed | processed=%d | failed_index=%d"), BatchResult.ProcessedCount, BatchResult.FailedIndex)
				: BatchResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return false;
	}

	TransformBatchOutputScratch.Reset(static_cast<int32>(ExpectedOutputFloatCount));
	auto AppendWireFloat = [this](double Value) -> bool
	{
		if (!FMath::IsFinite(Value) || FMath::Abs(Value) > static_cast<double>(MAX_flt))
		{
			return false;
		}
		TransformBatchOutputScratch.Add(static_cast<float>(Value));
		return true;
	};

	for (int32 Index = 0; Index < TransformBatchSnapshotScratch.Num(); ++Index)
	{
		const FAvidScriptActorTransformSnapshot& Snapshot = TransformBatchSnapshotScratch[Index];
		if (!AppendWireFloat(Snapshot.Location.X) ||
			!AppendWireFloat(Snapshot.Location.Y) ||
			!AppendWireFloat(Snapshot.Location.Z) ||
			!AppendWireFloat(Snapshot.Rotation.Pitch) ||
			!AppendWireFloat(Snapshot.Rotation.Yaw) ||
			!AppendWireFloat(Snapshot.Rotation.Roll) ||
			!AppendWireFloat(Snapshot.Scale3D.X) ||
			!AppendWireFloat(Snapshot.Scale3D.Y) ||
			!AppendWireFloat(Snapshot.Scale3D.Z))
		{
			TransformBatchOutputScratch.Reset();
			SetPendingHostImportFailure(
				TEXT("avidscript"),
				TEXT("actor_get_transform_batch"),
				FString::Printf(TEXT("Transform batch contains a non-finite or non-representable value | index=%d"), Index));
			Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
			return false;
		}
	}

	check(TransformBatchOutputScratch.Num() == OutputFloats.Num());
	FMemory::Memcpy(
		OutputFloats.GetData(),
		TransformBatchOutputScratch.GetData(),
		TransformBatchOutputScratch.Num() * sizeof(float));
	OutProcessedCount = RequestedCount;
	LastHostImportResult = OutProcessedCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return true;
}

int32 FAvidScriptWasmRuntimeInstance::HandleActorGetRootComponentImport(
	int32 Slot,
	int32 Generation,
	FAvidScriptObjectHandle& OutComponentHandle)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;
	OutComponentHandle = FAvidScriptObjectHandle();

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_root_component"), TEXT("Missing host object registry for avidscript.actor_get_root_component"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_root_component"), FString::Printf(TEXT("Invalid actor handle for avidscript.actor_get_root_component | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	const FAvidScriptObjectHandle ActorHandle{ static_cast<uint32>(Slot), static_cast<uint32>(Generation) };
	FAvidScriptActorBindingResult BindingResult;
	if (!FAvidScriptActorBinding::GetRootComponentHandle(*HostContext.ObjectRegistry, ActorHandle, OutComponentHandle, BindingResult))
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_root_component"), BindingResult.ErrorMessage.IsEmpty() ? FString::Printf(TEXT("Root component lookup failed | slot=%d | generation=%d"), Slot, Generation) : BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleSceneComponentGetWorldLocationImport(
	int32 Slot,
	int32 Generation,
	FVector& OutWorldLocation)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;
	OutWorldLocation = FVector::ZeroVector;

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("scene_component_get_world_location"), TEXT("Missing host object registry for avidscript.scene_component_get_world_location"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("scene_component_get_world_location"), FString::Printf(TEXT("Invalid component handle for avidscript.scene_component_get_world_location | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	const FAvidScriptObjectHandle ComponentHandle{ static_cast<uint32>(Slot), static_cast<uint32>(Generation) };
	FAvidScriptSceneComponentBindingResult BindingResult;
	if (!FAvidScriptSceneComponentBinding::GetWorldLocation(*HostContext.ObjectRegistry, ComponentHandle, OutWorldLocation, BindingResult))
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("scene_component_get_world_location"), BindingResult.ErrorMessage.IsEmpty() ? FString::Printf(TEXT("SceneComponent location read failed | slot=%d | generation=%d"), Slot, Generation) : BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleSceneComponentSetWorldLocationImport(
	int32 Slot,
	int32 Generation,
	const FVector& WorldLocation)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Slot;
	LastHostImportResult = 0;
	++HostImportCallCount;

	if (HostContext.ObjectRegistry == nullptr)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("scene_component_set_world_location"), TEXT("Missing host object registry for avidscript.scene_component_set_world_location"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (Slot <= 0 || Generation <= 0)
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("scene_component_set_world_location"), FString::Printf(TEXT("Invalid component handle for avidscript.scene_component_set_world_location | slot=%d | generation=%d"), Slot, Generation));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	const FAvidScriptObjectHandle ComponentHandle{ static_cast<uint32>(Slot), static_cast<uint32>(Generation) };
	FAvidScriptSceneComponentBindingResult BindingResult;
	if (!FAvidScriptSceneComponentBinding::SetWorldLocation(
		*HostContext.ObjectRegistry,
		ComponentHandle,
		WorldLocation,
		HostContext.ActorWritePolicy,
		BindingResult,
		HostContext.HostEffectJournal))
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("scene_component_set_world_location"), BindingResult.ErrorMessage.IsEmpty() ? FString::Printf(TEXT("SceneComponent location write failed | slot=%d | generation=%d"), Slot, Generation) : BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleTimerSetOnceImport(float DelaySeconds, int32 CallbackId)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = CallbackId;
	LastHostImportResult = 0;
	++HostImportCallCount;

	if (!IsLoaded()
		|| !FMath::IsFinite(DelaySeconds)
		|| DelaySeconds < 0.0f
		|| CallbackId < 0
		|| ActiveTimers.Num() >= AvidScriptMaximumPendingTimers)
	{
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	const int32 TimerHandle = AllocateTimerHandle();
	if (TimerHandle <= 0)
	{
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	const FAvidScriptWasmTimerEntry Timer{
		TimerHandle,
		CallbackId,
		TimerClockSeconds + static_cast<double>(DelaySeconds)
	};
	ActiveTimers.Add(TimerHandle, Timer);
	TimerHeap.HeapPush(Timer, FAvidScriptTimerDeadlineLess());
	LastHostImportResult = TimerHandle;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return TimerHandle;
}

int32 FAvidScriptWasmRuntimeInstance::HandleTimerCancelImport(int32 TimerHandle)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = TimerHandle;
	LastHostImportResult = 0;
	++HostImportCallCount;

	if (ActiveTimers.Remove(TimerHandle) > 0)
	{
		++StaleTimerHeapEntryCount;
		LastHostImportResult = 1;
		CompactTimerHeapIfNeeded();
	}

	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

void FAvidScriptWasmRuntimeInstance::CollectDueTimers(float DeltaSeconds)
{
	DueTimerScratch.Reset();
	const double SafeDeltaSeconds = FMath::IsFinite(DeltaSeconds) && DeltaSeconds > 0.0f
		? static_cast<double>(DeltaSeconds)
		: 0.0;
	TimerClockSeconds += SafeDeltaSeconds;

	const FAvidScriptTimerDeadlineLess DeadlineLess;
	while (!TimerHeap.IsEmpty() && TimerHeap[0].DueTimeSeconds <= TimerClockSeconds)
	{
		FAvidScriptWasmTimerEntry HeapTimer;
		TimerHeap.HeapPop(HeapTimer, DeadlineLess, EAllowShrinking::No);

		const FAvidScriptWasmTimerEntry* ActiveTimer = ActiveTimers.Find(HeapTimer.Handle);
		const bool bIsActiveEntry = ActiveTimer != nullptr
			&& ActiveTimer->DueTimeSeconds == HeapTimer.DueTimeSeconds
			&& ActiveTimer->CallbackId == HeapTimer.CallbackId;
		if (!bIsActiveEntry)
		{
			StaleTimerHeapEntryCount = FMath::Max(0, StaleTimerHeapEntryCount - 1);
			continue;
		}

		DueTimerScratch.Add(*ActiveTimer);
		ActiveTimers.Remove(HeapTimer.Handle);
	}

	CompactTimerHeapIfNeeded();
}

bool FAvidScriptWasmRuntimeInstance::ExecuteDueTimerCallbacks(FAvidScriptWasmSmokeResult& OutResult)
{
	for (const FAvidScriptWasmTimerEntry& Timer : DueTimerScratch)
	{
		uint32 TimerArgs[2] = {
			static_cast<uint32>(Timer.CallbackId),
			static_cast<uint32>(Timer.Handle)
		};
		const double CallbackStartSeconds = FPlatformTime::Seconds();
		if (!CallVmExport(
			VmBackend.Get(),
			TimerExport,
			ModuleId,
			"avid_on_timer",
			UE_ARRAY_COUNT(TimerArgs),
			TimerArgs,
			DebugMap.Get(),
			OutResult))
		{
			Metrics.TimerCallbackCallMs += MeasureElapsedMs(CallbackStartSeconds);
			return false;
		}

		Metrics.TimerCallbackCallMs += MeasureElapsedMs(CallbackStartSeconds);
		++TimerCallbackCount;
		LastTimerCallbackId = Timer.CallbackId;
		LastTimerHandle = Timer.Handle;
	}
	return true;
}

int32 FAvidScriptWasmRuntimeInstance::AllocateTimerHandle()
{
	for (int32 Attempt = 0; Attempt <= AvidScriptMaximumPendingTimers; ++Attempt)
	{
		const int32 Candidate = NextTimerHandle;
		NextTimerHandle = NextTimerHandle == MAX_int32 ? 1 : NextTimerHandle + 1;
		if (Candidate > 0 && !ActiveTimers.Contains(Candidate))
		{
			return Candidate;
		}
	}
	return 0;
}

void FAvidScriptWasmRuntimeInstance::CompactTimerHeapIfNeeded()
{
	const bool bHasEnoughStaleEntries = StaleTimerHeapEntryCount >= AvidScriptTimerHeapCompactionThreshold;
	const bool bStaleEntriesDominate = StaleTimerHeapEntryCount > ActiveTimers.Num();
	const bool bHeapExceedsBound = TimerHeap.Num() > AvidScriptMaximumPendingTimers * 2;
	if (!bHasEnoughStaleEntries || (!bStaleEntriesDominate && !bHeapExceedsBound))
	{
		return;
	}

	TimerHeap.Reset(ActiveTimers.Num());
	for (const TPair<int32, FAvidScriptWasmTimerEntry>& TimerPair : ActiveTimers)
	{
		TimerHeap.Add(TimerPair.Value);
	}
	TimerHeap.Heapify(FAvidScriptTimerDeadlineLess());
	StaleTimerHeapEntryCount = 0;
}

void FAvidScriptWasmRuntimeInstance::ResetTimerState()
{
	ActiveTimers.Reset();
	TimerHeap.Reset();
	DueTimerScratch.Reset();
	TimerClockSeconds = 0.0;
	StaleTimerHeapEntryCount = 0;
	NextTimerHandle = 1;
	TimerCallbackCount = 0;
	LastTimerCallbackId = 0;
	LastTimerHandle = 0;
}

void FAvidScriptWasmRuntimeInstance::CopyTimerStateToResult(FAvidScriptWasmSmokeResult& OutResult) const
{
	OutResult.bTimerCallbackCalled = TimerCallbackCount > 0;
	OutResult.TimerCallbackCount = TimerCallbackCount;
	OutResult.LastTimerCallbackId = LastTimerCallbackId;
	OutResult.LastTimerHandle = LastTimerHandle;
}

void FAvidScriptWasmRuntimeInstance::ResetEventState()
{
	EventCallbackCount = 0;
	LastEventId = 0;
	LastEventValue = 0.0f;
}

void FAvidScriptWasmRuntimeInstance::CopyEventStateToResult(FAvidScriptWasmSmokeResult& OutResult) const
{
	OutResult.bEventCallbackCalled = EventCallbackCount > 0;
	OutResult.EventCallbackCount = EventCallbackCount;
	OutResult.LastEventId = LastEventId;
	OutResult.LastEventValue = LastEventValue;
}

int32 FAvidScriptWasmRuntimeInstance::HandleHostAddI32Import(int32 Input)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Input;
	LastHostImportResult = Input + 1;
	++HostImportCallCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleHostFailI32Import(int32 Input)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Input;
	LastHostImportResult = 0;
	++HostImportCallCount;
	SetPendingHostImportFailure(
		TEXT("avidscript"),
		TEXT("host_fail_i32"),
		FString::Printf(TEXT("Host import avidscript.host_fail_i32 rejected input %d"), Input));
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 0;
}

void FAvidScriptWasmRuntimeInstance::SetPendingHostImportFailure(
	const FString& ImportModuleName,
	const FString& ImportName,
	const FString& Details)
{
	bHasPendingHostImportFailure = true;
	PendingHostImportModuleName = ImportModuleName;
	PendingHostImportName = ImportName;
	PendingHostImportDetails = Details;
}

bool FAvidScriptWasmRuntimeInstance::ConsumePendingHostImportFailure(
	FString& OutImportModuleName,
	FString& OutImportName,
	FString& OutDetails)
{
	if (!bHasPendingHostImportFailure)
	{
		return false;
	}

	OutImportModuleName = PendingHostImportModuleName;
	OutImportName = PendingHostImportName;
	OutDetails = PendingHostImportDetails;
	bHasPendingHostImportFailure = false;
	PendingHostImportModuleName.Empty();
	PendingHostImportName.Empty();
	PendingHostImportDetails.Empty();
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchDynamicHostCall(
	const FAvidScriptDynamicHostCall& Call,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	++HostImportCallCount;
	LastHostImportInput = static_cast<int32>(Call.BindingOrdinal);
	LastHostImportResult = 0;
	if (!BindingPackage.IsValid())
	{
		OutResult = FAvidScriptDynamicHostCallResult();
		OutResult.Details = TEXT("No reflected binding package is attached to this Runtime instance.");
		Metrics.HostImportCallMs += MeasureElapsedMs(HostImportStartSeconds);
		return false;
	}

	FAvidScriptBindingInvocationContext InvocationContext;
	InvocationContext.ObjectRegistry = HostContext.ObjectRegistry;
	InvocationContext.OwnerHandle = HostContext.OwnerHandle;
	InvocationContext.WritePolicy = HostContext.ActorWritePolicy;
	InvocationContext.HostEffectJournal = HostContext.HostEffectJournal;
	const bool bSucceeded = BindingPackage->Dispatch(
		Call,
		InvocationContext,
		BindingInvocationScratch,
		OutResult);
	LastHostImportResult = OutResult.ReturnValue;
	Metrics.HostImportCallMs += MeasureElapsedMs(HostImportStartSeconds);
	return bSucceeded;
}

bool FAvidScriptWasmRuntimeInstance::DispatchHostCall(
	const FAvidScriptHostCall& Call,
	FAvidScriptHostCallResult& OutResult)
{
	OutResult = FAvidScriptHostCallResult();
	auto Finish = [this, &OutResult](int32 ReturnValue, bool bSucceeded)
	{
		OutResult.ReturnValue = ReturnValue;
		OutResult.bSucceeded = bSucceeded;
		if (!bSucceeded)
		{
			FString ImportModuleName;
			FString ImportName;
			if (!ConsumePendingHostImportFailure(ImportModuleName, ImportName, OutResult.Details))
			{
				OutResult.Details = TEXT("The Runtime host dispatcher rejected the binding call.");
			}
		}
		return bSucceeded;
	};

	switch (Call.BindingId)
	{
	case EAvidScriptHostBindingId::HostAddI32:
		return Finish(HandleHostAddI32Import(Call.IntArgs[0]), true);
	case EAvidScriptHostBindingId::HostFailI32:
		HandleHostFailI32Import(Call.IntArgs[0]);
		return Finish(0, false);
	case EAvidScriptHostBindingId::OwnerGetSlot:
	{
		const int32 Value = HandleOwnerGetSlotImport();
		return Finish(Value, Value > 0);
	}
	case EAvidScriptHostBindingId::OwnerGetGeneration:
	{
		const int32 Value = HandleOwnerGetGenerationImport();
		return Finish(Value, Value > 0);
	}
	case EAvidScriptHostBindingId::TimerSetOnce:
	{
		const int32 Value = HandleTimerSetOnceImport(Call.FloatArgs[0], Call.IntArgs[0]);
		return Finish(Value, Value > 0);
	}
	case EAvidScriptHostBindingId::TimerCancel:
	{
		const int32 Value = HandleTimerCancelImport(Call.IntArgs[0]);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ActorGetLocation:
	{
		FVector Value = FVector::ZeroVector;
		const int32 ReturnValue = HandleActorGetLocationImport(Call.IntArgs[0], Call.IntArgs[1], Value);
		OutResult.FloatValues[0] = static_cast<float>(Value.X);
		OutResult.FloatValues[1] = static_cast<float>(Value.Y);
		OutResult.FloatValues[2] = static_cast<float>(Value.Z);
		return Finish(ReturnValue, ReturnValue != 0);
	}
	case EAvidScriptHostBindingId::ActorSetLocation:
	{
		const int32 ReturnValue = HandleActorSetLocationImport(Call.IntArgs[0], Call.IntArgs[1], FVector(Call.FloatArgs[0], Call.FloatArgs[1], Call.FloatArgs[2]));
		return Finish(ReturnValue, ReturnValue != 0);
	}
	case EAvidScriptHostBindingId::ActorAddLocationOffset:
	{
		const int32 ReturnValue = HandleActorAddLocationOffsetImport(Call.IntArgs[0], Call.IntArgs[1], FVector(Call.FloatArgs[0], Call.FloatArgs[1], Call.FloatArgs[2]));
		return Finish(ReturnValue, ReturnValue != 0);
	}
	case EAvidScriptHostBindingId::ActorGetRotation:
	{
		FRotator Value = FRotator::ZeroRotator;
		const int32 ReturnValue = HandleActorGetRotationImport(Call.IntArgs[0], Call.IntArgs[1], Value);
		OutResult.FloatValues[0] = static_cast<float>(Value.Pitch);
		OutResult.FloatValues[1] = static_cast<float>(Value.Yaw);
		OutResult.FloatValues[2] = static_cast<float>(Value.Roll);
		return Finish(ReturnValue, ReturnValue != 0);
	}
	case EAvidScriptHostBindingId::ActorSetRotation:
	{
		const int32 ReturnValue = HandleActorSetRotationImport(Call.IntArgs[0], Call.IntArgs[1], FRotator(Call.FloatArgs[0], Call.FloatArgs[1], Call.FloatArgs[2]));
		return Finish(ReturnValue, ReturnValue != 0);
	}
	case EAvidScriptHostBindingId::ActorGetScale:
	{
		FVector Value = FVector::ZeroVector;
		const int32 ReturnValue = HandleActorGetScaleImport(Call.IntArgs[0], Call.IntArgs[1], Value);
		OutResult.FloatValues[0] = static_cast<float>(Value.X);
		OutResult.FloatValues[1] = static_cast<float>(Value.Y);
		OutResult.FloatValues[2] = static_cast<float>(Value.Z);
		return Finish(ReturnValue, ReturnValue != 0);
	}
	case EAvidScriptHostBindingId::ActorSetScale:
	{
		const int32 ReturnValue = HandleActorSetScaleImport(Call.IntArgs[0], Call.IntArgs[1], FVector(Call.FloatArgs[0], Call.FloatArgs[1], Call.FloatArgs[2]));
		return Finish(ReturnValue, ReturnValue != 0);
	}
	case EAvidScriptHostBindingId::ActorGetTransformBatch:
	{
		int32 ProcessedCount = 0;
		const bool bSucceeded = HandleActorGetTransformBatchImport(
			Call.IntArgs[0],
			Call.InputCells,
			Call.OutputFloats,
			ProcessedCount);
		return Finish(ProcessedCount, bSucceeded);
	}
	case EAvidScriptHostBindingId::ActorGetRootComponent:
	{
		FAvidScriptObjectHandle Value;
		const int32 ReturnValue = HandleActorGetRootComponentImport(Call.IntArgs[0], Call.IntArgs[1], Value);
		OutResult.IntValues[0] = Value.Slot;
		OutResult.IntValues[1] = Value.Generation;
		return Finish(ReturnValue, ReturnValue != 0);
	}
	case EAvidScriptHostBindingId::SceneComponentGetWorldLocation:
	{
		FVector Value = FVector::ZeroVector;
		const int32 ReturnValue = HandleSceneComponentGetWorldLocationImport(Call.IntArgs[0], Call.IntArgs[1], Value);
		OutResult.FloatValues[0] = static_cast<float>(Value.X);
		OutResult.FloatValues[1] = static_cast<float>(Value.Y);
		OutResult.FloatValues[2] = static_cast<float>(Value.Z);
		return Finish(ReturnValue, ReturnValue != 0);
	}
	case EAvidScriptHostBindingId::SceneComponentSetWorldLocation:
	{
		const int32 ReturnValue = HandleSceneComponentSetWorldLocationImport(Call.IntArgs[0], Call.IntArgs[1], FVector(Call.FloatArgs[0], Call.FloatArgs[1], Call.FloatArgs[2]));
		return Finish(ReturnValue, ReturnValue != 0);
	}
	default:
		OutResult.Details = TEXT("The VM requested an unknown AvidScript host binding id.");
		return false;
	}
}
void FAvidScriptWasmRuntimeInstance::ResetHostImportState()
{
	HostImportCallCount = 0;
	LastHostImportInput = 0;
	LastHostImportResult = 0;
	bHasPendingHostImportFailure = false;
	PendingHostImportModuleName.Empty();
	PendingHostImportName.Empty();
	PendingHostImportDetails.Empty();
}

void FAvidScriptWasmRuntimeInstance::CopyHostImportStateToResult(FAvidScriptWasmSmokeResult& OutResult) const
{
	OutResult.HostImportCallCount = HostImportCallCount;
	OutResult.LastHostImportInput = LastHostImportInput;
	OutResult.LastHostImportResult = LastHostImportResult;
	OutResult.Metrics = Metrics;
}

void FAvidScriptWasmRuntimeInstance::CopyObservableStateToResult(FAvidScriptWasmSmokeResult& OutResult) const
{
	CopyHostImportStateToResult(OutResult);
	CopyEventStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
}

bool FAvidScriptWasmRuntime::RunEmbeddedSmokeTest(FAvidScriptWasmSmokeResult& OutResult)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	if (!Runtime.LoadEmbeddedSmokeModule(OutResult))
	{
		return false;
	}

	if (!Runtime.BeginPlay(OutResult))
	{
		return false;
	}

	if (!Runtime.Tick(1.0f / 60.0f, OutResult))
	{
		return false;
	}

	Runtime.Unload(OutResult);
	return true;
}

bool FAvidScriptWasmRuntime::RunEmbeddedHostImportSmokeTest(FAvidScriptWasmSmokeResult& OutResult)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	if (!Runtime.LoadEmbeddedHostImportModule(OutResult))
	{
		return false;
	}

	if (!Runtime.BeginPlay(OutResult))
	{
		return false;
	}

	return true;
}
