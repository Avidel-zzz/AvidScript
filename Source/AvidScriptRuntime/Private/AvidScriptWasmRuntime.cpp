#include "AvidScriptWasmRuntime.h"
#include "AvidScriptWasmRuntimePrivate.h"

#include "AvidScriptBindingDescriptor.h"
#include "AvidScriptSceneComponentBinding.h"
#include "AvidScriptValueCapability.h"
#include "Containers/StringConv.h"
#include "Continuation/AvidScriptContinuationResultCodec.h"
#include "DataBridge/AvidScriptCommandBuffer.h"
#include "Diagnostics/AvidScriptWasmDebugMap.h"
#include "Misc/PackageName.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"


DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWasmRuntime, Log, All);

struct FAvidScriptPreparedGeneratedHostCall
{
	FAvidScriptWasmRuntimeInstance* Runtime = nullptr;
	FAvidScriptPreparedGeneratedBinding Binding;
	FAvidScriptGeneratedI32PairCall I32PairCall = nullptr;
	FAvidScriptGeneratedPropertyI32GetCall PropertyI32GetCall = nullptr;
	FAvidScriptGeneratedPropertyI32SetCall PropertyI32SetCall = nullptr;
	uint64 PreparedCallbackEpoch = 0;
	uint64 PreparedReloadEpoch = 0;
	EAvidScriptPreparedHostEffectMode EffectMode =
		EAvidScriptPreparedHostEffectMode::Rejected;
};

struct FAvidScriptPreparedReflectionHostCall
{
	FAvidScriptWasmRuntimeInstance* Runtime = nullptr;
	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptPreparedReflectionBinding Binding;
};

struct FAvidScriptPreparedDynamicHostCall
{
	FAvidScriptWasmRuntimeInstance* Runtime = nullptr;
	TSharedPtr<const FAvidScriptBindingPackage> Package;
	FAvidScriptPreparedDynamicBinding Binding;
};

bool AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
	IAvidScriptVmBackend& Backend,
	const FAvidScriptVmExportHandle& Handle,
	const uint32 ExpectedParameterCellCount,
	FAvidScriptCachedVmExport& OutExport,
	FAvidScriptVmError& OutError)
{
	OutExport = FAvidScriptCachedVmExport();
	OutError.Reset();
	FAvidScriptVmPreparedExportCall PreparedCall;
	if (!Backend.PrepareExportCall(
			Handle,
			PreparedCall,
			OutError))
	{
		if (OutError.Category != TEXT("prepared_export_unsupported"))
		{
			return false;
		}
		OutExport.Handle = Handle;
		OutError.Reset();
		return true;
	}
	if (!PreparedCall.IsValid())
	{
		OutError.Reset();
		OutError.Category = TEXT("prepared_export_invalid");
		OutError.Details =
			TEXT("The VM backend returned an invalid prepared export call.");
		return false;
	}
	if (PreparedCall.ParameterCellCount != ExpectedParameterCellCount)
	{
		OutError.Reset();
		OutError.Category = TEXT("invalid_export");
		OutError.Details =
			TEXT("The prepared VM export signature does not match the Runtime lifecycle ABI.");
		return false;
	}

	OutExport.Handle = Handle;
	OutExport.PreparedCall = PreparedCall;
	return true;
}

namespace
{
constexpr uint32 AvidScriptWasmStackSize = 64 * 1024;
const FString AvidScriptBeginPlayExportName(TEXT("avid_on_begin_play"));
const FString AvidScriptTickExportName(TEXT("avid_on_tick"));
const FString AvidScriptEventExportName(TEXT("avid_on_event"));
const FString AvidScriptGameplayEventExportName(
	TEXT("avid_on_gameplay_event"));
const FString AvidScriptEndPlayExportName(TEXT("avid_on_end_play"));
const FString AvidScriptTimerExportName(TEXT("avid_on_timer"));
const FString AvidScriptContinuationExportName(TEXT("avid_on_continuation"));
const FString AvidScriptContinuationV2ExportName(
	TEXT("avid_on_continuation_v2"));
constexpr uint32 AvidScriptWasmHeapSize = 64 * 1024;
constexpr uint32 AvidScriptWasmErrorBufferSize = 512;
constexpr double AvidScriptMinimumMeasuredMs = 0.0001;
constexpr int32 AvidScriptMaximumPendingTimers = 1024;
constexpr int32 AvidScriptTimerHeapCompactionThreshold = 64;
constexpr int32 AvidScriptDataBridgeBudgetCheckStride = 32;
constexpr uint32 AvidScriptMaximumAsyncObjectPathUtf8Bytes = 1024;

bool DecodeAvidScriptUtf8ValueReference(
	const uint32 ValueReference,
	IAvidScriptVmGuestMemory* GuestMemory,
	const FAvidScriptUtf8ValueHeap& Utf8ValueHeap,
	FString& OutValue,
	FString& OutError)
{
	OutValue.Reset();
	OutError.Reset();
	TConstArrayView<uint8> Utf8Bytes;
	TArray<uint8, TInlineAllocator<256>> LinearStorage;
	if (FAvidScriptUtf8ValueHeap::IsHeapToken(ValueReference))
	{
		if (!Utf8ValueHeap.Resolve(ValueReference, Utf8Bytes, OutError))
		{
			return false;
		}
	}
	else
	{
		uint8 LengthBytes[sizeof(uint32)] = {};
		if (GuestMemory == nullptr
			|| ValueReference > MAX_uint32 - sizeof(LengthBytes)
			|| !GuestMemory->ReadBytes(
				ValueReference,
				MakeArrayView(LengthBytes),
				OutError))
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("continuation_object_path_memory_invalid");
			}
			return false;
		}

		const uint32 ByteCount = static_cast<uint32>(LengthBytes[0])
			| (static_cast<uint32>(LengthBytes[1]) << 8)
			| (static_cast<uint32>(LengthBytes[2]) << 16)
			| (static_cast<uint32>(LengthBytes[3]) << 24);
		if (ByteCount == 0
			|| ByteCount > AvidScriptMaximumAsyncObjectPathUtf8Bytes)
		{
			OutError = TEXT("continuation_object_path_size_invalid");
			return false;
		}

		const uint64 PayloadAddress = static_cast<uint64>(ValueReference)
			+ sizeof(LengthBytes);
		const uint64 StoredSize = static_cast<uint64>(ByteCount) + 1;
		if (PayloadAddress + StoredSize
			> static_cast<uint64>(MAX_uint32) + 1)
		{
			OutError = TEXT("continuation_object_path_memory_overflow");
			return false;
		}

		LinearStorage.SetNumUninitialized(static_cast<int32>(StoredSize));
		if (!GuestMemory->ReadBytes(
				static_cast<uint32>(PayloadAddress),
				MakeArrayView(LinearStorage),
				OutError)
			|| LinearStorage[ByteCount] != 0)
		{
			if (OutError.IsEmpty())
			{
				OutError = TEXT("continuation_object_path_terminator_invalid");
			}
			return false;
		}
		Utf8Bytes = MakeArrayView(LinearStorage).Left(
			static_cast<int32>(ByteCount));
	}

	bool bContainsNull = false;
	for (const uint8 Byte : Utf8Bytes)
	{
		bContainsNull |= Byte == 0;
	}
	if (Utf8Bytes.IsEmpty()
		|| static_cast<uint32>(Utf8Bytes.Num())
			> AvidScriptMaximumAsyncObjectPathUtf8Bytes
		|| bContainsNull)
	{
		OutError = TEXT("continuation_object_path_bytes_invalid");
		return false;
	}

	const FUTF8ToTCHAR Converted(
		reinterpret_cast<const ANSICHAR*>(Utf8Bytes.GetData()),
		Utf8Bytes.Num());
	const FTCHARToUTF8 RoundTrip(Converted.Get(), Converted.Length());
	if (Converted.Length() <= 0
		|| RoundTrip.Length() != Utf8Bytes.Num()
		|| FMemory::Memcmp(
			RoundTrip.Get(),
			Utf8Bytes.GetData(),
			Utf8Bytes.Num()) != 0)
	{
		OutError = TEXT("continuation_object_path_utf8_invalid");
		return false;
	}

	OutValue = FString(Converted.Length(), Converted.Get());
	return true;
}

struct FAvidScriptPreparedDataLaneWrite
{
	const FAvidScriptGeneratedBindingEntry* Entry = nullptr;
	FAvidScriptGeneratedPropertyI32SetCall SetCall = nullptr;
	FIntProperty* Property = nullptr;
	UObject* Receiver = nullptr;
	FAvidScriptObjectHandle ReceiverHandle;
	uint32 BindingOrdinal = MAX_uint32;
	int32 PreviousValue = 0;
	int32 Value = 0;
};

uint32 LoadAvidScriptLittleEndianU32(
	const TConstArrayView<uint8> Bytes,
	const int32 Offset)
{
	return static_cast<uint32>(Bytes[Offset])
		| (static_cast<uint32>(Bytes[Offset + 1]) << 8)
		| (static_cast<uint32>(Bytes[Offset + 2]) << 16)
		| (static_cast<uint32>(Bytes[Offset + 3]) << 24);
}

void StoreAvidScriptLittleEndianU32(
	const TArrayView<uint8> Bytes,
	const int32 Offset,
	const uint32 Value)
{
	Bytes[Offset] = static_cast<uint8>(Value);
	Bytes[Offset + 1] = static_cast<uint8>(Value >> 8);
	Bytes[Offset + 2] = static_cast<uint8>(Value >> 16);
	Bytes[Offset + 3] = static_cast<uint8>(Value >> 24);
}

float LoadAvidScriptLittleEndianF32(
	const TConstArrayView<uint8> Bytes,
	const int32 Offset)
{
	const uint32 Bits = LoadAvidScriptLittleEndianU32(Bytes, Offset);
	float Value = 0.0f;
	static_assert(sizeof(Value) == sizeof(Bits));
	FMemory::Memcpy(&Value, &Bits, sizeof(Value));
	return Value;
}

void StoreAvidScriptLittleEndianF32(
	const TArrayView<uint8> Bytes,
	const int32 Offset,
	const float Value)
{
	uint32 Bits = 0;
	static_assert(sizeof(Value) == sizeof(Bits));
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	StoreAvidScriptLittleEndianU32(Bytes, Offset, Bits);
}

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
	const FAvidScriptVmBackendInfo& BackendInfo,
	const FAvidScriptWasmRuntimeMetrics& Metrics)
{
	OutResult = FAvidScriptWasmSmokeResult();
	OutResult.ModuleId = ModuleId;
	OutResult.BackendInfo = BackendInfo;
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

bool InvokeVmExport(
	IAvidScriptVmBackend* Backend,
	FAvidScriptCachedVmExport& CachedExport,
	const FString& ExportName,
	uint32 ArgCount,
	const uint32* Args,
	FAvidScriptVmError& OutError)
{
	OutError = FAvidScriptVmError();
	if (Backend == nullptr)
	{
		OutError.Category = TEXT("backend_unavailable");
		OutError.Details = TEXT("No VM backend is attached to the runtime instance.");
		return false;
	}

	if (ArgCount > FAvidScriptVmCallFrame::MaxCells)
	{
		OutError.Category = TEXT("invalid_arguments");
		OutError.Details = TEXT("The runtime call exceeds the VM fixed cell capacity.");
		return false;
	}

	if (!CachedExport.Handle.IsValid())
	{
		FAvidScriptVmExportHandle Handle;
		if (!Backend->ResolveExport(ExportName, Handle, OutError))
		{
			return false;
		}
		if (!AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
				*Backend,
				Handle,
				ArgCount,
				CachedExport,
				OutError))
		{
			return false;
		}
	}

	FAvidScriptVmCallFrame Frame;
	Frame.CellCount = ArgCount;
	if (ArgCount > 0 && Args != nullptr)
	{
		FMemory::Memcpy(Frame.Cells, Args, ArgCount * sizeof(uint32));
	}
	const bool bCalled =
		CachedExport.PreparedCall.InvokeFunction != nullptr
		? CachedExport.PreparedCall.InvokeFunction(
			CachedExport.PreparedCall.Owner,
			CachedExport.PreparedCall.Target,
			Frame,
			OutError,
			nullptr)
		: Backend->Call(CachedExport.Handle, Frame, OutError);
	return bCalled;
}

bool CallVmExport(
	IAvidScriptVmBackend* Backend,
	FAvidScriptCachedVmExport& CachedExport,
	const FString& ModuleId,
	const FString& ExportName,
	uint32 ArgCount,
	const uint32* Args,
	const FAvidScriptWasmDebugMap* DebugMap,
	FAvidScriptWasmSmokeResult& OutResult)
{
	FAvidScriptVmError Error;
	if (InvokeVmExport(
			Backend,
			CachedExport,
			ExportName,
			ArgCount,
			Args,
			Error))
	{
		return true;
	}
	SetFailureFromVmError(OutResult, ModuleId, ExportName, Error, DebugMap);
	return false;
}
} // namespace

FAvidScriptWasmRuntimeInstance::FAvidScriptWasmRuntimeInstance()
{
	BackendSelection.BackendKind = EAvidScriptVmBackendKind::Wamr;
	BackendSelection.ExecutionMode = EAvidScriptVmExecutionMode::Auto;
	BackendSelection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	BackendSelection.bAllowFallback = true;
	BindingInvocationContext.ArrayValueHeap = &ArrayValueHeap;
	BindingInvocationContext.Utf8ValueHeap = &Utf8ValueHeap;
	BindingInvocationContext.CompositeValueHeap = &CompositeValueHeap;
}

FAvidScriptWasmRuntimeInstance::FAvidScriptWasmRuntimeInstance(
	const FAvidScriptVmBackendSelection& InBackendSelection)
	: BackendSelection(InBackendSelection)
{
	BindingInvocationContext.ArrayValueHeap = &ArrayValueHeap;
	BindingInvocationContext.Utf8ValueHeap = &Utf8ValueHeap;
	BindingInvocationContext.CompositeValueHeap = &CompositeValueHeap;
}

FAvidScriptWasmRuntimeInstance::~FAvidScriptWasmRuntimeInstance()
{
	Unload();
}

bool FAvidScriptWasmRuntimeInstance::BuildPreparedTypedHostImports(
	FString& OutError)
{
	OutError.Reset();
	TypedHostImports.Reset();
	PreparedGeneratedHostCalls.Reset();
	PreparedReflectionHostCalls.Reset();
	PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
	PreparedDynamicHostCalls.Reset();
	if (!BindingPackage.IsValid())
	{
		TypedHostImports = SupplementalTypedHostImports;
		return true;
	}
	if (!BindingPackage->BuildTypedHostImports(TypedHostImports, OutError))
	{
		return false;
	}

	TArray<FAvidScriptPreparedGeneratedBinding> PreparedBindings;
	if (!BindingPackage->BuildPreparedGeneratedBindings(
			PreparedBindings,
			OutError))
	{
		TypedHostImports.Reset();
		PreparedReflectionHostCalls.Reset();
		return false;
	}
	if (PreparedBindings.Num() != TypedHostImports.Num())
	{
		TypedHostImports.Reset();
		PreparedReflectionHostCalls.Reset();
		OutError = TEXT("generated_binding_prepared_count_mismatch");
		return false;
	}

	PreparedGeneratedHostCalls.Reserve(PreparedBindings.Num());
	for (int32 Index = 0; Index < TypedHostImports.Num(); ++Index)
	{
		FAvidScriptVmTypedHostImport& Import = TypedHostImports[Index];
		const FAvidScriptPreparedGeneratedBinding& Binding =
			PreparedBindings[Index];
		if (Import.BindingOrdinal != Binding.BindingOrdinal
			|| Binding.Entry == nullptr)
		{
			TypedHostImports.Reset();
			PreparedGeneratedHostCalls.Reset();
			PreparedReflectionHostCalls.Reset();
			OutError = TEXT("generated_binding_prepared_identity_mismatch");
			return false;
		}
		if (Import.Shape
				!= EAvidScriptVmTypedHostShape::SelfI32PairToI32
			&& Import.Shape
				!= EAvidScriptVmTypedHostShape::SelfPropertyI32Get
			&& Import.Shape
				!= EAvidScriptVmTypedHostShape::SelfPropertyI32Set)
		{
			continue;
		}
		const bool bScalarShapeMatches =
			Import.Shape == EAvidScriptVmTypedHostShape::SelfI32PairToI32
			&& Binding.Entry->Shape
				== EAvidScriptGeneratedBindingShape::I32PairToI32
			&& Binding.Entry->I32PairCall != nullptr;
		const bool bPropertyGetShapeMatches =
			Import.Shape == EAvidScriptVmTypedHostShape::SelfPropertyI32Get
			&& Binding.Entry->Shape
				== EAvidScriptGeneratedBindingShape::PropertyI32Get
			&& Binding.Entry->PropertyI32GetCall != nullptr;
		const bool bPropertySetShapeMatches =
			Import.Shape == EAvidScriptVmTypedHostShape::SelfPropertyI32Set
			&& Binding.Entry->Shape
				== EAvidScriptGeneratedBindingShape::PropertyI32Set
			&& Binding.Entry->PropertyI32SetCall != nullptr;
		if (Binding.Entry->ReceiverMode
				!= EAvidScriptGeneratedReceiverMode::SelfBound
			|| (!bScalarShapeMatches
				&& !bPropertyGetShapeMatches
				&& !bPropertySetShapeMatches))
		{
			TypedHostImports.Reset();
			PreparedGeneratedHostCalls.Reset();
			PreparedReflectionHostCalls.Reset();
			OutError = TEXT("generated_binding_prepared_shape_mismatch");
			return false;
		}

		TUniquePtr<FAvidScriptPreparedGeneratedHostCall> Call =
			MakeUnique<FAvidScriptPreparedGeneratedHostCall>();
		Call->Runtime = this;
		Call->Binding = Binding;
		Import.PreparedTarget.Context = Call.Get();
		switch (Import.Shape)
		{
		case EAvidScriptVmTypedHostShape::SelfI32PairToI32:
			Call->I32PairCall =
				Binding.Entry->PreparedI32PairCall != nullptr
				? Binding.Entry->PreparedI32PairCall
				: Binding.Entry->I32PairCall;
			Import.PreparedTarget.SelfI32Pair =
				&FAvidScriptWasmRuntimeInstance::InvokePreparedSelfI32Pair;
			break;
		case EAvidScriptVmTypedHostShape::SelfPropertyI32Get:
			Call->PropertyI32GetCall =
				Binding.Entry->PreparedPropertyI32GetCall != nullptr
				? Binding.Entry->PreparedPropertyI32GetCall
				: Binding.Entry->PropertyI32GetCall;
			Import.PreparedTarget.SelfPropertyI32Get =
				&FAvidScriptWasmRuntimeInstance::
					InvokePreparedSelfPropertyI32Get;
			break;
		case EAvidScriptVmTypedHostShape::SelfPropertyI32Set:
			Call->PropertyI32SetCall =
				Binding.Entry->PreparedPropertyI32SetCall != nullptr
				? Binding.Entry->PreparedPropertyI32SetCall
				: Binding.Entry->PropertyI32SetCall;
			Import.PreparedTarget.SelfPropertyI32Set =
				&FAvidScriptWasmRuntimeInstance::
					InvokePreparedSelfPropertyI32Set;
			break;
		default:
			checkNoEntry();
			break;
		}
		PreparedGeneratedHostCalls.Add(MoveTemp(Call));
	}

	TArray<FAvidScriptPreparedReflectionBinding> ReflectionBindings;
	if (!BindingPackage->BuildPreparedReflectionBindings(
			ReflectionBindings,
			OutError))
	{
		TypedHostImports.Reset();
		PreparedGeneratedHostCalls.Reset();
		PreparedReflectionHostCalls.Reset();
		return false;
	}
	TypedHostImports.Reserve(
		TypedHostImports.Num() + ReflectionBindings.Num());
	PreparedReflectionHostCalls.Reserve(
		ReflectionBindings.Num());
	for (const FAvidScriptPreparedReflectionBinding& Binding
		: ReflectionBindings)
	{
		const bool bScalar = Binding.TypedHostImport.Shape
			== EAvidScriptVmTypedHostShape::SelfI32PairToGuestI32;
		const bool bProperty = Binding.TypedHostImport.Shape
			== EAvidScriptVmTypedHostShape::SelfPropertyI32GetSet;
		const bool bVector = Binding.TypedHostImport.Shape
			== EAvidScriptVmTypedHostShape::SelfF32TripleToGuestVector;
		const bool bObject = Binding.TypedHostImport.Shape
			== EAvidScriptVmTypedHostShape::StableObjectRoundtrip;
		const bool bShapeTargetMatches =
			(bScalar
				&& Binding.NativeGuard != nullptr
				&& Binding.I32PairCall != nullptr)
			|| (bProperty
				&& ((Binding.bPropertyWrite
						&& Binding.PropertyI32Set != nullptr)
					|| (!Binding.bPropertyWrite
						&& Binding.PropertyI32Get != nullptr)))
			|| (bVector
				&& Binding.NativeGuard != nullptr
				&& Binding.VectorCall != nullptr)
			|| (bObject
				&& Binding.NativeGuard != nullptr
				&& Binding.ObjectCall != nullptr);
		if (Binding.BindingOrdinal == MAX_uint32
			|| Binding.ExpectedClass == nullptr
			|| Binding.ImmutablePlanIdentity == nullptr
			|| !bShapeTargetMatches)
		{
			TypedHostImports.Reset();
			PreparedGeneratedHostCalls.Reset();
			PreparedReflectionHostCalls.Reset();
			OutError =
				TEXT("prepared_reflection_shape_mismatch");
			return false;
		}

		TUniquePtr<FAvidScriptPreparedReflectionHostCall> Call =
			MakeUnique<FAvidScriptPreparedReflectionHostCall>();
		Call->Runtime = this;
		Call->Package = BindingPackage;
		Call->Binding = Binding;
		FAvidScriptVmTypedHostImport& Import =
			TypedHostImports.Add_GetRef(Binding.TypedHostImport);
		Import.PreparedTarget.Context = Call.Get();
		if (bScalar)
		{
			Import.PreparedTarget.SelfI32PairGuestResult =
				&FAvidScriptWasmRuntimeInstance::
					InvokePreparedReflectionSelfI32PairGuestResult;
		}
		else if (bProperty)
		{
			Import.PreparedTarget.SelfGuestAddress =
				&FAvidScriptWasmRuntimeInstance::
					InvokePreparedReflectionSelfGuestAddress;
		}
		else if (bVector)
		{
			Import.PreparedTarget.SelfF32TripleGuestVector =
				&FAvidScriptWasmRuntimeInstance::
					InvokePreparedReflectionSelfF32TripleGuestVector;
		}
		else
		{
			Import.PreparedTarget.StableObjectRoundtrip =
				&FAvidScriptWasmRuntimeInstance::
					InvokePreparedReflectionStableObjectRoundtrip;
		}
		PreparedReflectionHostCalls.Add(MoveTemp(Call));
	}
	if (!BuildPreparedDynamicHostImports(OutError))
	{
		TypedHostImports.Reset();
		PreparedGeneratedHostCalls.Reset();
		PreparedReflectionHostCalls.Reset();
		PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
		PreparedDynamicHostCalls.Reset();
		return false;
	}
	TypedHostImports.Append(SupplementalTypedHostImports);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::SetSupplementalTypedHostImports(
	const TConstArrayView<FAvidScriptVmTypedHostImport> Imports,
	FString& OutError)
{
	OutError.Reset();
	if (IsLoaded())
	{
		OutError = TEXT("supplemental typed imports must be configured before VM load");
		return false;
	}
	TSet<FString> Identities;
	for (const FAvidScriptVmTypedHostImport& Import : Imports)
	{
		const FString Identity = Import.ModuleName + TEXT("\n") + Import.ImportName;
		if (!Import.bSupplementalRuntimeAuthority
			|| Import.BindingOrdinal != MAX_uint32
			|| Import.StableId.IsEmpty()
			|| Import.ModuleName.IsEmpty()
			|| Import.ImportName.IsEmpty()
			|| !Import.PreparedTarget.IsBoundForShape(Import.Shape)
			|| Identities.Contains(Identity))
		{
			OutError = TEXT("supplemental typed import authority is invalid or duplicated");
			return false;
		}
		Identities.Add(Identity);
	}
	SupplementalTypedHostImports = Imports;
	return true;
}

bool FAvidScriptWasmRuntimeInstance::BuildPreparedDynamicHostImports(
	FString& OutError)
{
	PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
	PreparedDynamicHostCalls.Reset();
	if (!BindingPackage.IsValid())
	{
		return true;
	}

	PreparedVmBindingPackage = BindingPackage->GetVmPackage();
	TArray<FAvidScriptPreparedDynamicBinding> PreparedBindings;
	if (!BindingPackage->BuildPreparedDynamicBindings(
			PreparedBindings,
			OutError))
	{
		PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
		return false;
	}

	PreparedDynamicHostCalls.Reserve(PreparedBindings.Num());
	for (const FAvidScriptPreparedDynamicBinding& Binding : PreparedBindings)
	{
		const int32 ImportIndex = static_cast<int32>(Binding.BindingOrdinal);
		if (!PreparedVmBindingPackage.Imports.IsValidIndex(ImportIndex))
		{
			OutError = TEXT("prepared_dynamic_import_ordinal_invalid");
			PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
			PreparedDynamicHostCalls.Reset();
			return false;
		}
		FAvidScriptVmDynamicImport& Import =
			PreparedVmBindingPackage.Imports[ImportIndex];
		if (Import.Ordinal != Binding.BindingOrdinal
			|| Import.StableId != Binding.StableId
			|| Import.ModuleName != Binding.ModuleName
			|| Import.ImportName != Binding.ImportName
			|| Import.Signature != Binding.Signature
			|| !Import.PreparedTarget.IsEmpty()
			|| Binding.ImmutableInvocationCell == nullptr
			|| Binding.ExpectedClass == nullptr
			|| Binding.Invoke == nullptr)
		{
			OutError = TEXT("prepared_dynamic_import_identity_mismatch");
			PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
			PreparedDynamicHostCalls.Reset();
			return false;
		}

		TUniquePtr<FAvidScriptPreparedDynamicHostCall> Call =
			MakeUnique<FAvidScriptPreparedDynamicHostCall>();
		Call->Runtime = this;
		Call->Package = BindingPackage;
		Call->Binding = Binding;
		Import.PreparedTarget.Context = Call.Get();
		Import.PreparedTarget.Invoke =
			&FAvidScriptWasmRuntimeInstance::InvokePreparedDynamicHost;
		PreparedDynamicHostCalls.Add(MoveTemp(Call));
	}
	return true;
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
	TArrayView<const uint8> BytecodeView;
	if (Bytecode != nullptr && BytecodeSize > 0)
	{
		BytecodeView = MakeArrayView(Bytecode, BytecodeSize);
	}
	return LoadArtifactView(
		FAvidScriptVmArtifactView::FromWasmBytecode(BytecodeView),
		InModuleId,
		InBindingPackage,
		InDebugMap,
		OutResult);
}

bool FAvidScriptWasmRuntimeInstance::LoadArtifact(
	const FAvidScriptVmOwnedArtifact& Artifact,
	const FString& InModuleId,
	const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage,
	const TSharedPtr<const FAvidScriptWasmDebugMap>& InDebugMap,
	FAvidScriptWasmSmokeResult& OutResult)
{
	const bool bAuthorizedSerializedArtifact =
		Artifact.ArtifactFormat ==
			EAvidScriptVmArtifactFormat::WasmtimeSerialized
		&& AuthorizeAvidScriptVmArtifact(
			Artifact.AttestationId,
			Artifact);
	return LoadArtifactView(
		Artifact.MakeView(
			bAuthorizedSerializedArtifact
				? EAvidScriptVmArtifactTrust::VerifiedPackage
				: EAvidScriptVmArtifactTrust::Untrusted),
		InModuleId,
		InBindingPackage,
		InDebugMap,
		OutResult);
}

bool FAvidScriptWasmRuntimeInstance::LoadArtifactView(
	const FAvidScriptVmArtifactView& Artifact,
	const FString& InModuleId,
	const TSharedPtr<const FAvidScriptBindingPackage>& InBindingPackage,
	const TSharedPtr<const FAvidScriptWasmDebugMap>& InDebugMap,
	FAvidScriptWasmSmokeResult& OutResult)
{
	Unload();
	Metrics = FAvidScriptWasmRuntimeMetrics();
	DataBridgeMetrics = FAvidScriptDataBridgeMetrics();
	ActiveBackendInfo = FAvidScriptVmBackendInfo();
	ResetHostImportState();
	ModuleId = InModuleId;
	PrepareResult(OutResult, ModuleId, ActiveBackendInfo, Metrics);
	CopyHostImportStateToResult(OutResult);

	if (Artifact.ExecutionBytes.IsEmpty()
		|| Artifact.CanonicalWasmBytes.IsEmpty())
	{
		SetFailure(OutResult, ModuleId, TEXT("<module>"), TEXT("invalid_bytecode"), TEXT("No VM artifact bytes were provided"), TEXT("provide a non-empty canonical WASM or verified precompiled artifact"));
		return false;
	}

	FAvidScriptVmError Error;
	VmBackend = CreateAvidScriptVmBackend(BackendSelection, Error);
	if (!VmBackend)
	{
		SetFailureFromVmError(OutResult, ModuleId, TEXT("<runtime>"), Error, InDebugMap.Get());
		return false;
	}
	ActiveBackendInfo = VmBackend->GetBackendInfo();
	OutResult.BackendInfo = ActiveBackendInfo;

	BindingPackage = InBindingPackage;
	DebugMap = InDebugMap;
	if (BindingPackage.IsValid() || !SupplementalTypedHostImports.IsEmpty())
	{
		if (BindingPackage.IsValid())
		{
			BindingInvocationScratch.SetNumUninitialized(
				BindingPackage->GetRequiredScratchSize());
		}
		else
		{
			BindingInvocationScratch.Reset();
		}
		FString TypedImportError;
		if (!BuildPreparedTypedHostImports(TypedImportError))
		{
			SetFailure(
				OutResult,
				ModuleId,
				TEXT("<generated-bindings>"),
				TEXT("generated_binding_unavailable"),
				TypedImportError,
				TEXT("load the matching generated module before loading the VM"));
			VmBackend.Reset();
			BindingPackage.Reset();
			DebugMap.Reset();
			BindingInvocationScratch.Reset();
			TypedHostImports.Reset();
			PreparedGeneratedHostCalls.Reset();
			PreparedReflectionHostCalls.Reset();
			PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
			PreparedDynamicHostCalls.Reset();
			return false;
		}
	}
	else
	{
		BindingInvocationScratch.Reset();
		TypedHostImports.Reset();
		PreparedGeneratedHostCalls.Reset();
		PreparedReflectionHostCalls.Reset();
		PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
		PreparedDynamicHostCalls.Reset();
	}

	FAvidScriptVmLoadConfig Config;
	Config.HostDispatcher = this;
	Config.TypedHostDispatcher = this;
	Config.TypedHostImports = TypedHostImports;
	Config.BindingPackage = BindingPackage.IsValid()
		? &PreparedVmBindingPackage
		: nullptr;
	const bool bLoaded = VmBackend->LoadArtifact(
		Artifact,
		ModuleId,
		Config,
		Error);
	ActiveBackendInfo = VmBackend->GetBackendInfo();
	OutResult.BackendInfo = ActiveBackendInfo;
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
		TypedHostImports.Reset();
		PreparedGeneratedHostCalls.Reset();
		PreparedReflectionHostCalls.Reset();
		PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
		PreparedDynamicHostCalls.Reset();
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
	FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, ActiveBackendInfo, Metrics);
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
		FAvidScriptCachedVmExport* CachedExport = nullptr;
		uint32 ExpectedParameterCellCount = 0;
		if (RequiredExport == AvidScriptBeginPlayExportName)
		{
			CachedExport = &BeginPlayExport;
		}
		else if (RequiredExport == AvidScriptTickExportName)
		{
			CachedExport = &TickExport;
			ExpectedParameterCellCount = 1;
		}
		else if (RequiredExport == AvidScriptEndPlayExportName)
		{
			CachedExport = &EndPlayExport;
		}
		else if (RequiredExport == AvidScriptTimerExportName)
		{
			CachedExport = &TimerExport;
			ExpectedParameterCellCount = 2;
		}
		else if (RequiredExport == AvidScriptContinuationExportName)
		{
			CachedExport = &ContinuationExport;
			ExpectedParameterCellCount = 4;
		}
		else if (RequiredExport == AvidScriptContinuationV2ExportName)
		{
			CachedExport = &ContinuationV2Export;
			ExpectedParameterCellCount = 6;
		}
		else if (RequiredExport == AvidScriptEventExportName)
		{
			CachedExport = &EventExport;
			ExpectedParameterCellCount = 2;
		}
		else if (RequiredExport == AvidScriptGameplayEventExportName)
		{
			CachedExport = &GameplayEventExport;
			ExpectedParameterCellCount = 8;
			bGameplayEventExportLookupAttempted = true;
		}
		if (CachedExport != nullptr
			&& !AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
				*VmBackend,
				Handle,
				ExpectedParameterCellCount,
				*CachedExport,
				Error))
		{
			SetFailureFromVmError(
				OutResult,
				ModuleId,
				RequiredExport,
				Error,
				DebugMap.Get());
			return false;
		}
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::PrepareNamedExportCall(
	const FString& ExportName,
	FAvidScriptVmPreparedExportCall& OutCall,
	FString& OutError)
{
	OutCall = FAvidScriptVmPreparedExportCall();
	OutError.Reset();
	if (!IsLoaded() || !VmBackend)
	{
		OutError = TEXT("invalid_state: no loaded VM backend is available");
		return false;
	}
	if (ExportName.IsEmpty())
	{
		OutError = TEXT("invalid_arguments: generated export name is empty");
		return false;
	}

	FAvidScriptVmError Error;
	FAvidScriptVmExportHandle Handle;
	if (!VmBackend->ResolveExport(ExportName, Handle, Error)
		|| !VmBackend->PrepareExportCall(Handle, OutCall, Error)
		|| !OutCall.IsValid())
	{
		if (Error.Category.IsEmpty())
		{
			Error.Category = TEXT("prepared_export_invalid");
			Error.Details = TEXT("the VM backend returned an invalid prepared export call");
		}
		OutError = FString::Printf(
			TEXT("%s: %s"),
			*Error.Category,
			Error.Details.IsEmpty() ? TEXT("no details") : *Error.Details);
		OutCall = FAvidScriptVmPreparedExportCall();
		return false;
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::BeginPlay(FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, ActiveBackendInfo, Metrics);
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
	BeginTypedCallbackEpoch();
	const bool bBeginPlayCalled = CallVmExport(
		VmBackend.Get(),
		BeginPlayExport,
		ModuleId,
		AvidScriptBeginPlayExportName,
		0,
		nullptr,
		DebugMap.Get(),
		OutResult);
	EndTypedCallbackEpoch();
	if (!bBeginPlayCalled)
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
	return Tick(
		DeltaSeconds,
		OutResult,
		EAvidScriptWasmResultDetail::FullSnapshot);
}

bool FAvidScriptWasmRuntimeInstance::TickHot(
	const float DeltaSeconds,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	return Tick(
		DeltaSeconds,
		OutFailure,
		EAvidScriptWasmResultDetail::HotFailureOnly);
}

bool FAvidScriptWasmRuntimeInstance::Tick(
	const float DeltaSeconds,
	FAvidScriptWasmSmokeResult& OutResult,
	const EAvidScriptWasmResultDetail ResultDetail)
{
	const bool bFullSnapshot =
		ResultDetail == EAvidScriptWasmResultDetail::FullSnapshot;
	const bool bHotFailureOnly =
		ResultDetail == EAvidScriptWasmResultDetail::HotFailureOnly;
	if (bFullSnapshot)
	{
		PrepareResult(OutResult, ModuleId, ActiveBackendInfo, Metrics);
	}
	else if (!bHotFailureOnly)
	{
		OutResult.BackendInfo.Kind = ActiveBackendInfo.Kind;
		OutResult.BackendInfo.ExecutionMode = ActiveBackendInfo.ExecutionMode;
		OutResult.BackendInfo.ArtifactFormat = ActiveBackendInfo.ArtifactFormat;
		OutResult.BackendInfo.Capabilities = ActiveBackendInfo.Capabilities;
		OutResult.bTickCalled = false;
		OutResult.ExportName.Reset();
		OutResult.ImportModuleName.Reset();
		OutResult.ImportName.Reset();
		OutResult.ErrorCategory.Reset();
		OutResult.NextAction.Reset();
		OutResult.ErrorMessage.Reset();
		OutResult.DiagnosticFrames.Reset();
	}
	if (!bHotFailureOnly)
	{
		OutResult.bRuntimeInitialized = IsLoaded();
		OutResult.bModuleLoaded = IsLoaded();
		OutResult.bModuleInstantiated = IsLoaded();
		OutResult.bBeginPlayCalled = bHasBegunPlay;
		OutResult.bEndPlayCalled = bHasEndedPlay;
	}

	if (!IsLoaded())
	{
		if (!bFullSnapshot)
		{
			PrepareResult(OutResult, ModuleId, ActiveBackendInfo, Metrics);
			OutResult.bRuntimeInitialized = false;
			OutResult.bModuleLoaded = false;
			OutResult.bModuleInstantiated = false;
		}
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
		if (!bFullSnapshot)
		{
			PrepareResult(OutResult, ModuleId, ActiveBackendInfo, Metrics);
			OutResult.bRuntimeInitialized = IsLoaded();
			OutResult.bModuleLoaded = IsLoaded();
			OutResult.bModuleInstantiated = IsLoaded();
			OutResult.bBeginPlayCalled = bHasBegunPlay;
			OutResult.bEndPlayCalled = bHasEndedPlay;
		}
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
	BeginTypedCallbackEpoch();
	FAvidScriptVmError TickError;
	const bool bTickCalled = bHotFailureOnly
		? InvokeVmExport(
			VmBackend.Get(),
			TickExport,
			AvidScriptTickExportName,
			UE_ARRAY_COUNT(TickArgs),
			TickArgs,
			TickError)
		: CallVmExport(
			VmBackend.Get(),
			TickExport,
			ModuleId,
			AvidScriptTickExportName,
			UE_ARRAY_COUNT(TickArgs),
			TickArgs,
			DebugMap.Get(),
			OutResult);
	EndTypedCallbackEpoch();
	if (!bTickCalled)
	{
		Metrics.TickCallMs = MeasureElapsedMs(TickStartSeconds);
		if (bHotFailureOnly)
		{
			CaptureSnapshot(OutResult);
			SetFailureFromVmError(
				OutResult,
				ModuleId,
				AvidScriptTickExportName,
				TickError,
				DebugMap.Get());
		}
		OutResult.ModuleId = ModuleId;
		OutResult.BackendInfo = ActiveBackendInfo;
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	Metrics.TickCallMs = MeasureElapsedMs(TickStartSeconds);
	++TickCallCount;
	if (!bHotFailureOnly)
	{
		OutResult.Metrics = Metrics;
		OutResult.bTickCalled = true;
		OutResult.TickCallCount = TickCallCount;
	}

	FAvidScriptVmError TimerError;
	if (!ExecuteDueTimerCallbacks(TimerError))
	{
		if (bHotFailureOnly)
		{
			CaptureSnapshot(OutResult);
		}
		SetFailureFromVmError(
			OutResult,
			ModuleId,
			AvidScriptTimerExportName,
			TimerError,
			DebugMap.Get());
		OutResult.ModuleId = ModuleId;
		OutResult.BackendInfo = ActiveBackendInfo;
		OutResult.Metrics = Metrics;
		OutResult.bTickCalled = true;
		OutResult.TickCallCount = TickCallCount;
		CopyObservableStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	if (!bHotFailureOnly)
	{
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchEvent(
	int32 EventId,
	float Value,
	FAvidScriptWasmSmokeResult& OutResult)
{
	return DispatchEvent(
		EventId,
		Value,
		OutResult,
		EAvidScriptWasmResultDetail::FullSnapshot);
}

bool FAvidScriptWasmRuntimeInstance::DispatchEventHot(
	const int32 EventId,
	const float Value,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	return DispatchEvent(
		EventId,
		Value,
		OutFailure,
		EAvidScriptWasmResultDetail::HotFailureOnly);
}

#if WITH_DEV_AUTOMATION_TESTS
bool FAvidScriptWasmRuntimeInstance::InvokeI32PairExportHotForTesting(
	const FString& ExportName,
	const int32 FirstArgument,
	const int32 SecondArgument,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	if (!IsLoaded() || ExportName.IsEmpty())
	{
		CaptureSnapshot(OutFailure);
		SetFailure(
			OutFailure,
			ModuleId,
			ExportName,
			TEXT("invalid_state"),
			TEXT("Testing export invocation requires a loaded runtime and a non-empty export name."),
			TEXT("load a module and provide one explicit export name before invoking it"));
		return false;
	}

	if (TestingI32PairExportName != ExportName)
	{
		TestingI32PairExport = {};
		TestingI32PairExportName = ExportName;
	}
	const uint32 Arguments[] = {
		static_cast<uint32>(FirstArgument),
		static_cast<uint32>(SecondArgument)
	};
	BeginTypedCallbackEpoch();
	FAvidScriptVmError Error;
	const bool bCalled = InvokeVmExport(
		VmBackend.Get(),
		TestingI32PairExport,
		ExportName,
		UE_ARRAY_COUNT(Arguments),
		Arguments,
		Error);
	EndTypedCallbackEpoch();
	if (!bCalled)
	{
		CaptureSnapshot(OutFailure);
		SetFailureFromVmError(
			OutFailure,
			ModuleId,
			ExportName,
			Error,
			DebugMap.Get());
	}
	return bCalled;
}
#endif

bool FAvidScriptWasmRuntimeInstance::DispatchEvent(
	const int32 EventId,
	const float Value,
	FAvidScriptWasmSmokeResult& OutResult,
	const EAvidScriptWasmResultDetail ResultDetail)
{
	const bool bHotFailureOnly =
		ResultDetail == EAvidScriptWasmResultDetail::HotFailureOnly;
	if (!bHotFailureOnly)
	{
		CaptureSnapshot(OutResult);
	}

	if (!IsLoaded() || !bHasBegunPlay || bEndPlayAttempted)
	{
		if (bHotFailureOnly)
		{
			CaptureSnapshot(OutResult);
		}
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
		if (bHotFailureOnly)
		{
			CaptureSnapshot(OutResult);
		}
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
	BeginTypedCallbackEpoch();
	FAvidScriptVmError EventError;
	const bool bEventCalled = bHotFailureOnly
		? InvokeVmExport(
			VmBackend.Get(),
			EventExport,
			AvidScriptEventExportName,
			UE_ARRAY_COUNT(EventArgs),
			EventArgs,
			EventError)
		: CallVmExport(
			VmBackend.Get(),
			EventExport,
			ModuleId,
			AvidScriptEventExportName,
			UE_ARRAY_COUNT(EventArgs),
			EventArgs,
			DebugMap.Get(),
			OutResult);
	EndTypedCallbackEpoch();
	if (!bEventCalled)
	{
		Metrics.EventCallbackCallMs = MeasureElapsedMs(EventStartSeconds);
		if (bHotFailureOnly)
		{
			CaptureSnapshot(OutResult);
			SetFailureFromVmError(
				OutResult,
				ModuleId,
				AvidScriptEventExportName,
				EventError,
				DebugMap.Get());
		}
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
	if (!bHotFailureOnly)
	{
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchGameplayEvent(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutResult)
{
	return DispatchGameplayEvent(
		Event,
		OutResult,
		EAvidScriptWasmResultDetail::FullSnapshot);
}

bool FAvidScriptWasmRuntimeInstance::DispatchGameplayEventHot(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutFailure)
{
	return DispatchGameplayEvent(
		Event,
		OutFailure,
		EAvidScriptWasmResultDetail::HotFailureOnly);
}

bool FAvidScriptWasmRuntimeInstance::BuildPreparedDelegateEvents(
	TArray<FAvidScriptPreparedDelegateEvent>& OutEvents,
	FString& OutError)
{
	OutEvents.Reset();
	OutError.Reset();
	if (!BindingPackage.IsValid())
	{
		return true;
	}
	if (!BindingPackage->BuildPreparedDelegateEvents(OutEvents, OutError))
	{
		return false;
	}
	return PrepareDelegateEventExports(OutEvents, OutError);
}

bool FAvidScriptWasmRuntimeInstance::BuildPreparedCallbacks(
	TArray<FAvidScriptPreparedDelegateEvent>& OutDelegateEvents,
	TArray<FAvidScriptPreparedDelegateEvent>& OutInboundHandlers,
	FString& OutError)
{
	OutDelegateEvents.Reset();
	OutInboundHandlers.Reset();
	OutError.Reset();
	if (!BindingPackage.IsValid())
	{
		return true;
	}
	if (!BindingPackage->BuildPreparedDelegateEvents(
			OutDelegateEvents,
			OutError)
		|| !BindingPackage->BuildPreparedInboundHandlers(
			OutInboundHandlers,
			OutError))
	{
		OutDelegateEvents.Reset();
		OutInboundHandlers.Reset();
		return false;
	}

	TArray<FAvidScriptPreparedDelegateEvent> Callbacks = OutDelegateEvents;
	Callbacks.Append(OutInboundHandlers);
	if (!PrepareDelegateEventExports(Callbacks, OutError))
	{
		OutDelegateEvents.Reset();
		OutInboundHandlers.Reset();
		return false;
	}
	OutDelegateEvents.Reset();
	OutInboundHandlers.Reset();
	for (FAvidScriptPreparedDelegateEvent& Callback : Callbacks)
	{
		if (Callback.CallbackKind == TEXT("multicast"))
		{
			OutDelegateEvents.Add(MoveTemp(Callback));
		}
		else
		{
			OutInboundHandlers.Add(MoveTemp(Callback));
		}
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::PrepareDelegateEventExports(
	TArray<FAvidScriptPreparedDelegateEvent>& InOutEvents,
	FString& OutError)
{
	OutError.Reset();
	DelegateEventExports.Reset();
	if (!IsLoaded() || VmBackend == nullptr)
	{
		OutError = TEXT("delegate_runtime_unavailable");
		return false;
	}

	TArray<FAvidScriptPreparedDelegateEvent> ImplementedEvents;
	ImplementedEvents.Reserve(InOutEvents.Num());
	for (const FAvidScriptPreparedDelegateEvent& Event : InOutEvents)
	{
		FAvidScriptVmExportHandle Handle;
		FAvidScriptVmError ResolveError;
		if (!VmBackend->ResolveExport(Event.ExportName, Handle, ResolveError))
		{
			if (ResolveError.Category == TEXT("missing_export"))
			{
				continue;
			}
			OutError = FString::Printf(
				TEXT("delegate_export_resolve_failed | export=%s | category=%s | details=%s"),
				*Event.ExportName,
				ResolveError.Category.IsEmpty() ? TEXT("vm_error") : *ResolveError.Category,
				ResolveError.Details.IsEmpty() ? TEXT("<none>") : *ResolveError.Details);
			DelegateEventExports.Reset();
			return false;
		}

		FAvidScriptCachedVmExport CachedExport;
		if (!AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
				*VmBackend,
				Handle,
				Event.ParameterCellCount,
				CachedExport,
				ResolveError))
		{
			OutError = FString::Printf(
				TEXT("delegate_export_prepare_failed | export=%s | category=%s | details=%s"),
				*Event.ExportName,
				ResolveError.Category.IsEmpty() ? TEXT("vm_error") : *ResolveError.Category,
				ResolveError.Details.IsEmpty() ? TEXT("<none>") : *ResolveError.Details);
			DelegateEventExports.Reset();
			return false;
		}
		DelegateEventExports.Add(Event.StableId, MoveTemp(CachedExport));
		ImplementedEvents.Add(Event);
	}

	InOutEvents = MoveTemp(ImplementedEvents);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchPreparedDelegateEvent(
	const FAvidScriptPreparedDelegateEvent& Event,
	void* NativeParameters,
	FAvidScriptWasmSmokeResult& OutResult)
{
	CaptureSnapshot(OutResult);
	if (!IsLoaded() || !bHasBegunPlay || bEndPlayAttempted)
	{
		SetFailure(
			OutResult,
			ModuleId,
			Event.ExportName,
			TEXT("invalid_state"),
			TEXT("Delegate events require an active runtime between BeginPlay and EndPlay"),
			TEXT("broadcast generated delegate events only while the AvidScript session is Running"));
		return false;
	}
	if (Event.StableId.IsEmpty()
		|| Event.ExportName.IsEmpty()
		|| Event.ImmutableCodecIdentity == nullptr
		|| Event.Encode == nullptr
		|| Event.ParameterCellCount > FAvidScriptVmCallFrame::MaxCells)
	{
		SetFailure(
			OutResult,
			ModuleId,
			Event.ExportName,
			TEXT("invalid_argument"),
			TEXT("The prepared delegate event plan is invalid"),
			TEXT("rebuild the schema 11 binding package before dispatch"));
		return false;
	}

	FAvidScriptVmCallFrame Frame;
	TArray<FAvidScriptObjectHandle> BorrowedHandles;
	TUniquePtr<FAvidScriptPreparedDelegateOutputTransaction> OutputTransaction;
	uint32 OutputTransactionToken = 0;
	FString EncodeCategory;
	FString EncodeDetails;
	const auto ReleaseBorrows = [this, &BorrowedHandles]()
	{
		if (HostContext.ObjectRegistry == nullptr)
		{
			return;
		}
		for (int32 Index = BorrowedHandles.Num() - 1; Index >= 0; --Index)
		{
			FAvidScriptObjectHandleResult ReleaseResult;
			HostContext.ObjectRegistry->ReleaseBorrowedHandle(
				BorrowedHandles[Index],
				ReleaseResult,
				false);
		}
		BorrowedHandles.Reset();
	};
	if (Event.OutputParameterCount > 0)
	{
		if (ActiveDelegateOutputTransaction != nullptr
			|| ActiveDelegateOutputToken != 0)
		{
			SetFailure(
				OutResult,
				ModuleId,
				Event.ExportName,
				TEXT("reentrant_operation"),
				TEXT("A prepared delegate output transaction is already active"),
				TEXT("defer nested delegate dispatch until the active callback returns"));
			return false;
		}
		if (!BindingPackage.IsValid()
			|| !BindingPackage->BeginPreparedDelegateOutputTransaction(
				Event,
				NativeParameters,
				OutputTransaction,
				EncodeDetails))
		{
			SetFailure(
				OutResult,
				ModuleId,
				Event.ExportName,
				TEXT("delegate_output_prepare_failed"),
				EncodeDetails.IsEmpty()
					? TEXT("The prepared delegate output transaction could not be created")
					: *EncodeDetails,
				TEXT("regenerate the binding descriptor and reload the matching script artifact"));
			return false;
		}
		OutputTransactionToken = FAvidScriptValueCapability::AllocateToken();
		if (OutputTransactionToken == 0)
		{
			SetFailure(
				OutResult,
				ModuleId,
				Event.ExportName,
				TEXT("delegate_output_token_exhausted"),
				TEXT("The process-wide capability token space is exhausted"),
				TEXT("restart the process before dispatching additional script callbacks"));
			return false;
		}
	}
	if (!Event.Encode(
			Event.ImmutableCodecIdentity,
			NativeParameters,
			BindingInvocationContext,
			OutputTransactionToken,
			Frame,
			BorrowedHandles,
			EncodeCategory,
			EncodeDetails)
		|| Frame.CellCount != Event.ParameterCellCount)
	{
		ReleaseBorrows();
		SetFailure(
			OutResult,
			ModuleId,
			Event.ExportName,
			EncodeCategory.IsEmpty()
				? TEXT("delegate_parameter_encode_failed")
				: *EncodeCategory,
			EncodeDetails.IsEmpty()
				? TEXT("The prepared delegate codec rejected the native parameter frame")
				: *EncodeDetails,
			TEXT("verify that the broadcast values still satisfy the generated event contract"));
		return false;
	}

	FAvidScriptCachedVmExport& CachedExport =
		DelegateEventExports.FindOrAdd(Event.StableId);
	const double EventStartSeconds = FPlatformTime::Seconds();
	ActiveDelegateOutputTransaction = OutputTransaction.Get();
	ActiveDelegateOutputToken = OutputTransactionToken;
	BeginTypedCallbackEpoch();
	FAvidScriptVmError EventError;
	bool bCalled = InvokeVmExport(
		VmBackend.Get(),
		CachedExport,
		Event.ExportName,
		Frame.CellCount,
		Frame.Cells,
		EventError);
	EndTypedCallbackEpoch();
	ActiveDelegateOutputTransaction = nullptr;
	ActiveDelegateOutputToken = 0;
	if (bCalled && OutputTransaction.IsValid())
	{
		FString CommitError;
		if (!OutputTransaction->Commit(CommitError))
		{
			bCalled = false;
			EventError.Category = TEXT("delegate_output_commit_failed");
			EventError.Details = CommitError.IsEmpty()
				? TEXT("The prepared delegate callback returned without staging every ref/out value.")
				: MoveTemp(CommitError);
		}
	}
	ReleaseBorrows();
	Metrics.EventCallbackCallMs = MeasureElapsedMs(EventStartSeconds);
	if (!bCalled)
	{
		SetFailureFromVmError(
			OutResult,
			ModuleId,
			Event.ExportName,
			EventError,
			DebugMap.Get());
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	++EventCallbackCount;
	OutResult.Metrics = Metrics;
	CopyObservableStateToResult(OutResult);
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchContinuation(
	const FAvidScriptContinuationCompletion& Completion,
	FAvidScriptWasmSmokeResult& OutResult)
{
	CaptureSnapshot(OutResult);
	const bool bUseV2 = ContinuationV2Export.Handle.IsValid();
	const FString& ExportName = bUseV2
		? AvidScriptContinuationV2ExportName
		: AvidScriptContinuationExportName;
	if (!IsLoaded() || !bHasBegunPlay || bEndPlayAttempted)
	{
		SetFailure(
			OutResult,
			ModuleId,
			ExportName,
			TEXT("invalid_state"),
			TEXT("Continuations require an active runtime between BeginPlay and EndPlay"),
			TEXT("deliver continuation completions only through the active Runtime Session"));
		return false;
	}

	FAvidScriptCachedVmExport& CachedExport = bUseV2
		? ContinuationV2Export
		: ContinuationExport;
	uint32 Args[6] = {
		static_cast<uint32>(Completion.CallbackId),
		0,
		0,
		static_cast<uint32>(Completion.Status),
		static_cast<uint32>(Completion.ObjectSlot),
		static_cast<uint32>(Completion.ObjectGeneration)
	};
	static_assert(sizeof(Completion.Token) == sizeof(uint32) * 2);
	FMemory::Memcpy(&Args[1], &Completion.Token, sizeof(Completion.Token));
	if (bContinuationDispatchActive)
	{
		SetFailure(
			OutResult,
			ModuleId,
			ExportName,
			TEXT("continuation_dispatch_reentrant"),
			TEXT("A continuation callback cannot be nested inside another continuation callback."),
			TEXT("queue nested work through a new continuation"));
		return false;
	}
	FAvidScriptContinuationResultCodecTransaction ResultTransaction;
	bContinuationDispatchActive = true;
	bContinuationResultConsumed = false;
	bContinuationStateConsumed = false;
	ActiveContinuationToken = Completion.Token;
	ActiveContinuationStatus = Completion.Status;
	ActiveContinuationResultSlot = Completion.ObjectSlot;
	ActiveContinuationResultGeneration = Completion.ObjectGeneration;
	ActiveContinuationResultTransaction = &ResultTransaction;
	BeginTypedCallbackEpoch();
	FAvidScriptVmError Error;
	const bool bCalled = InvokeVmExport(
		VmBackend.Get(),
		CachedExport,
		ExportName,
		bUseV2 ? UE_ARRAY_COUNT(Args) : 4,
		Args,
		Error);
	EndTypedCallbackEpoch();
	if (bCalled)
	{
		ResultTransaction.Commit();
	}
	else
	{
		ResultTransaction.Rollback(Utf8ValueHeap, ArrayValueHeap);
	}
	bContinuationDispatchActive = false;
	bContinuationResultConsumed = false;
	bContinuationStateConsumed = false;
	ActiveContinuationToken = 0;
	ActiveContinuationStatus = EAvidScriptContinuationStatus::Failed;
	ActiveContinuationResultSlot = 0;
	ActiveContinuationResultGeneration = 0;
	ActiveContinuationResultTransaction = nullptr;
	if (!bCalled)
	{
		SetFailureFromVmError(
			OutResult,
			ModuleId,
			ExportName,
			Error,
			DebugMap.Get());
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::DispatchGameplayEvent(
	const FAvidScriptGameplayEvent& Event,
	FAvidScriptWasmSmokeResult& OutResult,
	const EAvidScriptWasmResultDetail ResultDetail)
{
	const FString& ExportName = AvidScriptGameplayEventExportName;
	const bool bHotFailureOnly =
		ResultDetail == EAvidScriptWasmResultDetail::HotFailureOnly;
	if (!bHotFailureOnly)
	{
		CaptureSnapshot(OutResult);
	}

	if (!IsLoaded() || !bHasBegunPlay || bEndPlayAttempted)
	{
		if (bHotFailureOnly)
		{
			CaptureSnapshot(OutResult);
		}
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
		if (bHotFailureOnly)
		{
			CaptureSnapshot(OutResult);
		}
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
		FAvidScriptVmExportHandle Handle;
		if (!VmBackend->ResolveExport(ExportName, Handle, ResolveError) &&
			ResolveError.Category != TEXT("missing_export"))
		{
			if (bHotFailureOnly)
			{
				CaptureSnapshot(OutResult);
			}
			SetFailureFromVmError(OutResult, ModuleId, ExportName, ResolveError, DebugMap.Get());
			FAvidScriptLifecycleTransitionResult LifecycleResult;
			LifecycleState.MarkFaulted(LifecycleResult);
			return false;
		}
		if (Handle.IsValid())
		{
			if (!AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
					*VmBackend,
					Handle,
					8,
					GameplayEventExport,
					ResolveError))
			{
				if (bHotFailureOnly)
				{
					CaptureSnapshot(OutResult);
				}
				SetFailureFromVmError(
					OutResult,
					ModuleId,
					ExportName,
					ResolveError,
					DebugMap.Get());
				FAvidScriptLifecycleTransitionResult LifecycleResult;
				LifecycleState.MarkFaulted(LifecycleResult);
				return false;
			}
		}
	}

	if (!GameplayEventExport.Handle.IsValid())
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
	BeginTypedCallbackEpoch();
	FAvidScriptVmError EventError;
	const bool bGameplayEventCalled = bHotFailureOnly
		? InvokeVmExport(
			VmBackend.Get(),
			GameplayEventExport,
			AvidScriptGameplayEventExportName,
			UE_ARRAY_COUNT(EventArgs),
			EventArgs,
			EventError)
		: CallVmExport(
			VmBackend.Get(),
			GameplayEventExport,
			ModuleId,
			AvidScriptGameplayEventExportName,
			UE_ARRAY_COUNT(EventArgs),
			EventArgs,
			DebugMap.Get(),
			OutResult);
	EndTypedCallbackEpoch();
	if (!bGameplayEventCalled)
	{
		Metrics.EventCallbackCallMs = MeasureElapsedMs(EventStartSeconds);
		if (bHotFailureOnly)
		{
			CaptureSnapshot(OutResult);
			SetFailureFromVmError(
				OutResult,
				ModuleId,
				AvidScriptGameplayEventExportName,
				EventError,
				DebugMap.Get());
		}
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
	if (!bHotFailureOnly)
	{
		OutResult.Metrics = Metrics;
		CopyObservableStateToResult(OutResult);
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::EndPlay(FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, ActiveBackendInfo, Metrics);
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
	if (!EndPlayExport.Handle.IsValid())
	{
		FAvidScriptVmExportHandle Handle;
		if (!VmBackend->ResolveExport(
				AvidScriptEndPlayExportName,
				Handle,
				EndPlayResolveError))
		{
			Metrics.EndPlayCallMs = 0.0;
			OutResult.Metrics = Metrics;
			CopyObservableStateToResult(OutResult);
			bEndPlaySucceeded = true;
			LifecycleState.TryTransition(
				EAvidScriptLifecycleState::Stopped,
				LifecycleResult);
			CachedEndPlayResult = OutResult;
			return true;
		}
		if (!AvidScriptWasmRuntimePrivate::CacheResolvedVmExport(
				*VmBackend,
				Handle,
				0,
				EndPlayExport,
				EndPlayResolveError))
		{
			SetFailureFromVmError(
				OutResult,
				ModuleId,
				AvidScriptEndPlayExportName,
				EndPlayResolveError,
				DebugMap.Get());
			Metrics.EndPlayCallMs = 0.0;
			OutResult.Metrics = Metrics;
			CopyObservableStateToResult(OutResult);
			LifecycleState.MarkFaulted(LifecycleResult);
			bEndPlaySucceeded = false;
			CachedEndPlayResult = OutResult;
			return false;
		}
	}

	const double EndPlayStartSeconds = FPlatformTime::Seconds();
	BeginTypedCallbackEpoch();
	const bool bEndPlayCalled = CallVmExport(
		VmBackend.Get(),
		EndPlayExport,
		ModuleId,
		AvidScriptEndPlayExportName,
		0,
		nullptr,
		DebugMap.Get(),
		OutResult);
	EndTypedCallbackEpoch();
	if (!bEndPlayCalled)
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
	ActiveDelegateOutputTransaction = nullptr;
	ActiveDelegateOutputToken = 0;

	if (VmBackend)
	{
		VmBackend->Unload();
		VmBackend.Reset();
	}
	ArrayValueHeap.Reset();
	Utf8ValueHeap.Reset();
	CompositeValueHeap.Reset();
	PreparedGeneratedHostCalls.Reset();
	PreparedReflectionHostCalls.Reset();
	PreparedVmBindingPackage = FAvidScriptVmBindingPackage();
	PreparedDynamicHostCalls.Reset();
	TypedHostImports.Reset();
	BindingPackage.Reset();
	DebugMap.Reset();
	BindingInvocationScratch.Reset();
	FusedCallbackFrameStack.Reset();
	InvalidateSelfCapability();
	BeginPlayExport = {};
	TickExport = {};
	EndPlayExport = {};
	TimerExport = {};
	ContinuationExport = {};
	ContinuationV2Export = {};
	EventExport = {};
	GameplayEventExport = {};
	DelegateEventExports.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	TestingI32PairExport = {};
	TestingI32PairExportName.Reset();
#endif
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
	PrepareResult(OutResult, PreviousModuleId, ActiveBackendInfo, Metrics);
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
	InvalidateSelfCapability();
	HostContext = InHostContext;
	BindingInvocationContext.ObjectRegistry = HostContext.ObjectRegistry;
	BindingInvocationContext.ObjectOwnership = HostContext.ObjectOwnership;
	BindingInvocationContext.ArrayValueHeap = &ArrayValueHeap;
	BindingInvocationContext.Utf8ValueHeap = &Utf8ValueHeap;
	BindingInvocationContext.CompositeValueHeap = &CompositeValueHeap;
	BindingInvocationContext.OwnerHandle = HostContext.OwnerHandle;
	BindingInvocationContext.World = HostContext.World;
	BindingInvocationContext.WritePolicy = HostContext.ActorWritePolicy;
	BindingInvocationContext.HostEffectJournal = HostContext.HostEffectJournal;
	BindingInvocationContext.LatentHost = HostContext.LatentHost;
	BindingInvocationContext.InvocationPolicy = HostContext.BindingInvocationPolicy;
	BindingInvocationContext.InvocationInstrumentation =
		HostContext.BindingInvocationInstrumentation;
}

void FAvidScriptWasmRuntimeInstance::ClearHostContext()
{
	InvalidateSelfCapability();
	HostContext = FAvidScriptWasmHostContext();
	BindingInvocationContext = FAvidScriptBindingInvocationContext();
	BindingInvocationContext.ArrayValueHeap = &ArrayValueHeap;
	BindingInvocationContext.Utf8ValueHeap = &Utf8ValueHeap;
	BindingInvocationContext.CompositeValueHeap = &CompositeValueHeap;
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

int64 FAvidScriptWasmRuntimeInstance::HandleOwnerGetHandleImport()
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = 0;
	LastHostImportResult = 0;
	++HostImportCallCount;

	const FAvidScriptObjectHandle OwnerHandle = HostContext.OwnerHandle;
	if (!OwnerHandle.IsValid())
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_owner_get_handle"),
			TEXT("Missing valid owner handle context for avidscript.avid_owner_get_handle"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	if (!BindingPackage.IsValid()
		|| BindingPackage->GetExpectedSelfClass() == nullptr)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_owner_get_handle"),
			TEXT("Packed owner access requires a binding package with ExpectedSelfClass"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	const uint64 PackedHandle = static_cast<uint64>(OwnerHandle.Slot)
		| (static_cast<uint64>(OwnerHandle.Generation) << 32);
	LastHostImportResult = static_cast<int32>(OwnerHandle.Slot);
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return static_cast<int64>(PackedHandle);
}

int64 FAvidScriptWasmRuntimeInstance::HandleDataLaneGetEpochImport()
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = 0;
	LastHostImportResult = 0;
	++HostImportCallCount;
	++DataBridgeMetrics.BoundaryCrossings;
	if (!IsInGameThread()
		|| FusedCallbackFrameStack.IsEmpty()
		|| !BindingPackage.IsValid()
		|| HostContext.World.IsStale())
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_data_lane_epoch"),
			TEXT("data_lane_epoch_unavailable: no active generated callback epoch."));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	const uint64 Epoch =
		FusedCallbackFrameStack.Last().CallbackEpoch;
	LastHostImportResult = static_cast<int32>(Epoch);
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return static_cast<int64>(Epoch);
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueArrayLengthImport(const int32 Token)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;

	FAvidScriptArrayValueView Value;
	FString Error;
	if (!ArrayValueHeap.Resolve(static_cast<uint32>(Token), FString(), Value, Error))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_array_length"),
			MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = Value.ElementCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return Value.ElementCount;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueArrayLoadImport(
	const int32 Token,
	const int32 ElementIndex,
	const TArrayView<uint8> OutBytes)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;

	FString Error;
	if (!ArrayValueHeap.ReadElement(
			static_cast<uint32>(Token),
			ElementIndex,
			OutBytes,
			Error))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_array_load"),
			MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueArrayStoreImport(
	const int32 Token,
	const int32 ElementIndex,
	const TConstArrayView<uint8> Bytes)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;

	FString Error;
	if (!ArrayValueHeap.WriteElement(
			static_cast<uint32>(Token),
			ElementIndex,
			Bytes,
			Error))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_array_store"),
			MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueArrayRangeImport(
	const bool bReadFromCapability,
	const int32 Token,
	const int32 CapabilityIndex,
	const uint32 GuestArrayReference,
	const int32 GuestIndex,
	const int32 ElementCount)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;
	const TCHAR* ImportName = bReadFromCapability
		? TEXT("avid_value_array_read_range")
		: TEXT("avid_value_array_write_range");
	auto FailRange = [this, HostImportStartSeconds, ImportName](FString Error)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			ImportName,
			MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	};

	FAvidScriptArrayValueView Value;
	FString Error;
	if (!ArrayValueHeap.Resolve(
			static_cast<uint32>(Token),
			FString(),
			Value,
			Error))
	{
		return FailRange(MoveTemp(Error));
	}
	if (GuestArrayReference == 0
		|| FAvidScriptValueCapability::IsToken(GuestArrayReference)
		|| CapabilityIndex < 0
		|| GuestIndex < 0
		|| ElementCount < 0
		|| ElementCount > FAvidScriptArrayValueHeap::MaxElements
		|| static_cast<int64>(CapabilityIndex) + ElementCount > Value.ElementCount)
	{
		return FailRange(
			TEXT("array_value_range_invalid: the capability or guest range is outside the supported bounds."));
	}

	IAvidScriptVmGuestMemory* GuestMemory =
		VmBackend == nullptr ? nullptr : VmBackend->GetGuestMemory();
	int32 GuestElementCount = 0;
	if (GuestMemory == nullptr
		|| !GuestMemory->ReadBytes(
			GuestArrayReference,
			MakeArrayView(
				reinterpret_cast<uint8*>(&GuestElementCount),
				sizeof(GuestElementCount)),
			Error))
	{
		return FailRange(
			Error.IsEmpty()
				? TEXT("guest_memory_invalid: the linear array length header is unavailable.")
				: MoveTemp(Error));
	}
	if (GuestElementCount < 0
		|| static_cast<int64>(GuestIndex) + ElementCount > GuestElementCount)
	{
		return FailRange(
			TEXT("array_value_range_invalid: the guest linear array range is out of bounds."));
	}

	const uint64 PayloadAlignment = static_cast<uint64>(
		FMath::Max(4, Value.ElementAlignment));
	const uint64 PayloadOffset = Align(
		static_cast<uint64>(sizeof(int32)),
		PayloadAlignment);
	const uint64 ByteCount = static_cast<uint64>(ElementCount)
		* static_cast<uint64>(Value.ElementStride);
	const uint64 GuestByteAddress = static_cast<uint64>(GuestArrayReference)
		+ PayloadOffset
		+ static_cast<uint64>(GuestIndex)
			* static_cast<uint64>(Value.ElementStride);
	constexpr uint64 GuestAddressSpaceSize = static_cast<uint64>(MAX_uint32) + 1u;
	if (ByteCount > FAvidScriptArrayValueHeap::MaxValueBytes
		|| GuestByteAddress >= GuestAddressSpaceSize
		|| GuestByteAddress + ByteCount > GuestAddressSpaceSize)
	{
		return FailRange(
			TEXT("guest_memory_invalid: the linear array payload range overflows the guest address space."));
	}

	if (ByteCount > 0)
	{
		if (bReadFromCapability)
		{
			TArrayView<uint8> OutputBytes;
			if (!GuestMemory->BorrowMutableBytes(
					static_cast<uint32>(GuestByteAddress),
					static_cast<uint32>(ByteCount),
					static_cast<uint32>(PayloadAlignment),
					OutputBytes,
					Error)
				|| !ArrayValueHeap.ReadRange(
					static_cast<uint32>(Token),
					CapabilityIndex,
					ElementCount,
					OutputBytes,
					Error))
			{
				return FailRange(
					Error.IsEmpty()
						? TEXT("guest_memory_invalid: the linear array output range is unavailable.")
						: MoveTemp(Error));
			}
		}
		else
		{
			TConstArrayView<uint8> InputBytes;
			if (!GuestMemory->BorrowReadOnlyBytes(
					static_cast<uint32>(GuestByteAddress),
					static_cast<uint32>(ByteCount),
					static_cast<uint32>(PayloadAlignment),
					InputBytes,
					Error)
				|| !ArrayValueHeap.WriteRange(
					static_cast<uint32>(Token),
					CapabilityIndex,
					ElementCount,
					InputBytes,
					Error))
			{
				return FailRange(
					Error.IsEmpty()
						? TEXT("guest_memory_invalid: the linear array input range is unavailable.")
						: MoveTemp(Error));
			}
		}
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueReleaseImport(const int32 Token)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;

	// Guest-owned linear arrays do not require host cleanup.
	if (Token >= 0)
	{
		LastHostImportResult = 1;
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 1;
	}

	FString ArrayError;
	FString Utf8Error;
	FString CompositeError;
	if (!ArrayValueHeap.ReleaseValue(static_cast<uint32>(Token), ArrayError)
		&& !Utf8ValueHeap.ReleaseValue(static_cast<uint32>(Token), Utf8Error)
		&& !CompositeValueHeap.ReleaseValue(static_cast<uint32>(Token), CompositeError))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_release"),
			FString::Printf(
				TEXT("value_token_stale: the capability is invalid or stale. array=[%s] utf8=[%s] composite=[%s]"),
				*ArrayError,
				*Utf8Error,
				*CompositeError));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueTextToStringImport(const int32 Token)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;

	FAvidScriptCompositeValueView Value;
	FString Error;
	if (!IsInGameThread()
		|| Token >= 0
		|| !CompositeValueHeap.Resolve(
			static_cast<uint32>(Token),
			FString(),
			nullptr,
			Value,
			Error)
		|| Value.Kind != EAvidScriptCompositeValueKind::Text
		|| Value.Property == nullptr
		|| !Value.Property->IsA<FTextProperty>()
		|| Value.Value == nullptr)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_text_to_string"),
			Error.IsEmpty()
				? FString(TEXT("text_value_invalid: the capability is not a live FText value."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	const FString Presentation = static_cast<const FText*>(Value.Value)->ToString();
	const FTCHARToUTF8 Utf8(*Presentation, Presentation.Len());
	FAvidScriptUtf8ValueReservation Reservation;
	if (Utf8.Length() < 0
		|| static_cast<uint32>(Utf8.Length()) > FAvidScriptUtf8ValueHeap::MaxValueBytes
		|| !Utf8ValueHeap.Reserve(Reservation, Error))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_text_to_string"),
			Error.IsEmpty()
				? FString(TEXT("text_value_too_large: the FText presentation exceeds the UTF-8 value limit."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	uint32 ResultToken = 0;
	bool bCreated = false;
	const TConstArrayView<uint8> Bytes(
		reinterpret_cast<const uint8*>(Utf8.Get()),
		Utf8.Length());
	if (!Utf8ValueHeap.InternReserved(
			Reservation,
			Bytes,
			ResultToken,
			bCreated,
			Error))
	{
		Utf8ValueHeap.ReleaseReservation(Reservation);
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_text_to_string"),
			MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = static_cast<int32>(ResultToken);
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueContainerCountImport(
	const int32 Token)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;
	FString Error;
	int32 Count = 0;
	if (!IsInGameThread()
		|| Token >= 0
		|| !BindingPackage.IsValid()
		|| !BindingPackage->GetCompositeContainerCount(
			static_cast<uint32>(Token),
			BindingInvocationContext,
			Count,
			Error))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_container_count"),
			Error.IsEmpty()
				? FString(TEXT("composite_container_invalid: the capability cannot be counted."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}
	LastHostImportResult = Count;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return Count;
}

int32 FAvidScriptWasmRuntimeInstance::HandleDelegateOutputWriteImport(
	const int32 TransactionToken,
	const int32 OutputOrdinal,
	const uint32 GuestAddress)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = TransactionToken;
	LastHostImportResult = 0;
	++HostImportCallCount;
	IAvidScriptVmGuestMemory* GuestMemory = VmBackend == nullptr
		? nullptr
		: VmBackend->GetGuestMemory();
	FString Error;
	const bool bSucceeded = IsInGameThread()
		&& TransactionToken < 0
		&& static_cast<uint32>(TransactionToken) == ActiveDelegateOutputToken
		&& OutputOrdinal >= 0
		&& GuestAddress != 0
		&& ActiveDelegateOutputTransaction != nullptr
		&& GuestMemory != nullptr
		&& ActiveDelegateOutputTransaction->StageOutput(
			static_cast<uint32>(OutputOrdinal),
			GuestAddress,
			*GuestMemory,
			BindingInvocationContext,
			Error);
	if (!bSucceeded)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_delegate_output_write"),
			Error.IsEmpty()
				? FString(TEXT("delegate_output_write_invalid: the staged ref/out write does not match the active callback transaction."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}
	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueContainerAccessImport(
	const bool bRead,
	const int32 Token,
	const int32 Index,
	const int32 Lane,
	const uint32 GuestAddress)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;
	const TCHAR* ImportName = bRead
		? TEXT("avid_value_container_read")
		: TEXT("avid_value_container_write");
	IAvidScriptVmGuestMemory* GuestMemory = VmBackend == nullptr
		? nullptr
		: VmBackend->GetGuestMemory();
	FString Error;
	const bool bSucceeded = IsInGameThread()
		&& Token < 0
		&& Index >= 0
		&& Lane >= 0
		&& GuestAddress != 0
		&& BindingPackage.IsValid()
		&& GuestMemory != nullptr
		&& (bRead
			? BindingPackage->ReadCompositeContainerValue(
				static_cast<uint32>(Token),
				Index,
				Lane,
				GuestAddress,
				*GuestMemory,
				BindingInvocationContext,
				Error)
			: BindingPackage->WriteCompositeContainerValue(
				static_cast<uint32>(Token),
				Index,
				Lane,
				GuestAddress,
				*GuestMemory,
				BindingInvocationContext,
				Error));
	if (!bSucceeded)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			ImportName,
			Error.IsEmpty()
				? FString(TEXT("composite_container_access_invalid: the requested operation failed validation."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}
	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueContainerResizeImport(
	const int32 Token,
	const int32 NewCount)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;
	FString Error;
	if (!IsInGameThread()
		|| Token >= 0
		|| !BindingPackage.IsValid()
		|| !BindingPackage->ResizeCompositeArray(
			static_cast<uint32>(Token),
			NewCount,
			BindingInvocationContext,
			Error))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_container_resize"),
			Error.IsEmpty()
				? FString(TEXT("composite_array_resize_invalid: the requested resize failed validation."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}
	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueContainerClearImport(
	const int32 Token)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = 0;
	++HostImportCallCount;
	FString Error;
	if (!IsInGameThread()
		|| Token >= 0
		|| !BindingPackage.IsValid()
		|| !BindingPackage->ClearCompositeContainer(
			static_cast<uint32>(Token),
			BindingInvocationContext,
			Error))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_container_clear"),
			Error.IsEmpty()
				? FString(TEXT("composite_container_clear_invalid: the requested clear failed validation."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}
	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueContainerFindImport(
	const int32 Token,
	const uint32 GuestAddress)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = -1;
	++HostImportCallCount;
	IAvidScriptVmGuestMemory* GuestMemory = VmBackend == nullptr
		? nullptr
		: VmBackend->GetGuestMemory();
	FString Error;
	int32 FoundIndex = -1;
	if (!IsInGameThread()
		|| Token >= 0
		|| GuestAddress == 0
		|| !BindingPackage.IsValid()
		|| GuestMemory == nullptr
		|| !BindingPackage->FindCompositeContainerValue(
			static_cast<uint32>(Token),
			GuestAddress,
			*GuestMemory,
			BindingInvocationContext,
			FoundIndex,
			Error)
		|| FoundIndex < -1)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_container_find"),
			Error.IsEmpty()
				? FString(TEXT("composite_container_find_invalid: the requested lookup failed validation."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return -1;
	}
	LastHostImportResult = FoundIndex;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return FoundIndex;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueContainerUpsertImport(
	const int32 Token,
	const uint32 KeyAddress,
	const uint32 ValueAddress)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = -1;
	++HostImportCallCount;
	IAvidScriptVmGuestMemory* GuestMemory = VmBackend == nullptr
		? nullptr
		: VmBackend->GetGuestMemory();
	FString Error;
	int32 MutationResult = -1;
	if (!IsInGameThread()
		|| Token >= 0
		|| KeyAddress == 0
		|| !BindingPackage.IsValid()
		|| GuestMemory == nullptr
		|| !BindingPackage->UpsertCompositeContainerValue(
			static_cast<uint32>(Token),
			KeyAddress,
			ValueAddress,
			*GuestMemory,
			BindingInvocationContext,
			MutationResult,
			Error)
		|| (MutationResult != 1 && MutationResult != 2))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_container_upsert"),
			Error.IsEmpty()
				? FString(TEXT("composite_container_upsert_invalid: the requested mutation failed validation."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return -1;
	}
	LastHostImportResult = MutationResult;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return MutationResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleValueContainerRemoveImport(
	const int32 Token,
	const uint32 KeyAddress)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Token;
	LastHostImportResult = -1;
	++HostImportCallCount;
	IAvidScriptVmGuestMemory* GuestMemory = VmBackend == nullptr
		? nullptr
		: VmBackend->GetGuestMemory();
	FString Error;
	bool bRemoved = false;
	if (!IsInGameThread()
		|| Token >= 0
		|| KeyAddress == 0
		|| !BindingPackage.IsValid()
		|| GuestMemory == nullptr
		|| !BindingPackage->RemoveCompositeContainerValue(
			static_cast<uint32>(Token),
			KeyAddress,
			*GuestMemory,
			BindingInvocationContext,
			bRemoved,
			Error))
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_value_container_remove"),
			Error.IsEmpty()
				? FString(TEXT("composite_container_remove_invalid: the requested mutation failed validation."))
				: MoveTemp(Error));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return -1;
	}
	LastHostImportResult = bRemoved ? 1 : 0;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleDataLaneSubmitImport(
	const TConstArrayView<uint8> Bytes)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = Bytes.Num();
	LastHostImportResult = 0;
	++HostImportCallCount;
	++DataBridgeMetrics.BoundaryCrossings;
	++DataBridgeMetrics.SubmittedBuffers;
	DataBridgeMetrics.SubmittedBytes += static_cast<uint64>(Bytes.Num());

	FAvidScriptParsedCommandBuffer Buffer;
	FAvidScriptCommandBufferParseResult ParseResult;
	const uint64 ExpectedEpoch = FusedCallbackFrameStack.IsEmpty()
		? 0
		: FusedCallbackFrameStack.Last().CallbackEpoch;
	if (!IsInGameThread()
		|| !BindingPackage.IsValid()
		|| HostContext.World.IsStale()
		|| !FAvidScriptCommandBufferParser::Parse(
			Bytes,
			ExpectedEpoch,
			DataBridgeBudget,
			Buffer,
			ParseResult))
	{
		++DataBridgeMetrics.RejectedBuffers;
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("avid_data_lane_submit"),
			ParseResult.ErrorCategory.IsEmpty()
				? TEXT("data_lane_context_invalid: submission requires an active generated callback on the Game Thread.")
				: FString::Printf(
					TEXT("%s at %s: %s"),
					*ParseResult.ErrorCategory,
					*ParseResult.ErrorSource,
					*ParseResult.ErrorDetails));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	DataBridgeMetrics.SubmittedCommands +=
		static_cast<uint64>(Buffer.Commands.Num());
	TArray<FAvidScriptPreparedDataLaneWrite, TInlineAllocator<32>> PreparedWrites;
	PreparedWrites.Reserve(Buffer.Commands.Num());
	TArray<uint64, TInlineAllocator<32>> UniqueObjects;
	for (int32 CommandIndex = 0; CommandIndex < Buffer.Commands.Num(); ++CommandIndex)
	{
		const FAvidScriptParsedCommand& Command = Buffer.Commands[CommandIndex];
		const FAvidScriptGeneratedBindingEntry* Entry = nullptr;
		UClass* ExpectedClass = nullptr;
		FProperty* ReflectedProperty = nullptr;
		bool bRequiresWriteAccess = false;
		UObject* Receiver = nullptr;
		const FAvidScriptObjectHandle ReceiverHandle{
			static_cast<uint32>(Command.SelfSlot),
			static_cast<uint32>(Command.SelfGeneration)
			};
		FIntProperty* IntProperty = nullptr;
		const FAvidScriptPreparedDataLaneWrite* PreviousPrepared =
			PreparedWrites.IsEmpty()
				? nullptr
				: &PreparedWrites.Last();
		const bool bReusePreparedTarget = PreviousPrepared != nullptr
			&& PreviousPrepared->BindingOrdinal == Command.BindingOrdinal
			&& PreviousPrepared->ReceiverHandle == ReceiverHandle;
		if (bReusePreparedTarget)
		{
			Entry = PreviousPrepared->Entry;
			IntProperty = PreviousPrepared->Property;
			Receiver = PreviousPrepared->Receiver;
		}
		else
		{
			const bool bBindingResolved =
				BindingPackage->TryGetGeneratedPropertyBinding(
					Command.BindingOrdinal,
					Entry,
					ExpectedClass,
					ReflectedProperty,
					bRequiresWriteAccess);
			const bool bSplitSetter = Entry != nullptr
				&& Entry->Shape == EAvidScriptGeneratedBindingShape::PropertyI32Set
				&& Entry->PropertyI32SetCall != nullptr;
			const bool bLegacySetter = Entry != nullptr
				&& Entry->Shape
					== EAvidScriptGeneratedBindingShape::PropertyI32GetSet
				&& Entry->PropertyI32Call != nullptr;
			if (!bBindingResolved
				|| (!bSplitSetter && !bLegacySetter)
				|| Entry->ReceiverMode
					!= EAvidScriptGeneratedReceiverMode::SelfBound
				|| (bRequiresWriteAccess
					&& HostContext.ActorWritePolicy
						!= EAvidScriptActorWritePolicy::AllowWrites)
				|| !ResolveSelfCapability(
					Command.SelfSlot,
					Command.SelfGeneration,
					ExpectedClass,
					Receiver)
				|| (IntProperty = CastField<FIntProperty>(ReflectedProperty)) == nullptr)
			{
				++DataBridgeMetrics.RejectedBuffers;
				SetPendingHostImportFailure(
					TEXT("avidscript"),
					TEXT("avid_data_lane_submit"),
					FString::Printf(
						TEXT("data_lane_binding_rejected at command[%d]."),
						CommandIndex));
				Metrics.HostImportCallMs =
					MeasureElapsedMs(HostImportStartSeconds);
				return 0;
			}
		}

		const uint64 PackedHandle = static_cast<uint64>(ReceiverHandle.Slot)
			| (static_cast<uint64>(ReceiverHandle.Generation) << 32);
		if (!bReusePreparedTarget)
		{
			UniqueObjects.AddUnique(PackedHandle);
		}
		if (static_cast<uint32>(UniqueObjects.Num()) > DataBridgeBudget.MaxObjects)
		{
			++DataBridgeMetrics.RejectedBuffers;
			SetPendingHostImportFailure(
				TEXT("avidscript"),
				TEXT("avid_data_lane_submit"),
				TEXT("data_lane_object_budget_exceeded."));
			Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
			return 0;
		}

		const int32 PreviousValue = bReusePreparedTarget
			? PreviousPrepared->PreviousValue
			: IntProperty->GetPropertyValue_InContainer(Receiver);
		FAvidScriptPreparedDataLaneWrite& Prepared =
			PreparedWrites.AddDefaulted_GetRef();
		Prepared.Entry = Entry;
		Prepared.SetCall = Entry->PropertyI32SetCall;
		Prepared.Property = IntProperty;
		Prepared.Receiver = Receiver;
		Prepared.ReceiverHandle = ReceiverHandle;
		Prepared.BindingOrdinal = Command.BindingOrdinal;
		Prepared.PreviousValue = PreviousValue;
		Prepared.Value = Command.Arg0;
	}

	for (int32 CommandIndex = 0; CommandIndex < PreparedWrites.Num(); ++CommandIndex)
	{
		const FAvidScriptPreparedDataLaneWrite& Prepared = PreparedWrites[CommandIndex];
		if (CommandIndex > 0)
		{
			const FAvidScriptPreparedDataLaneWrite& Previous =
				PreparedWrites[CommandIndex - 1];
			if (Previous.BindingOrdinal == Prepared.BindingOrdinal
				&& Previous.ReceiverHandle == Prepared.ReceiverHandle)
			{
				continue;
			}
		}
		if (!BindingPackage->PrepareGeneratedHostEffect(
				Prepared.BindingOrdinal,
				Prepared.ReceiverHandle,
				*Prepared.Receiver,
				BindingInvocationContext))
		{
			++DataBridgeMetrics.RejectedBuffers;
			SetPendingHostImportFailure(
				TEXT("avidscript"),
				TEXT("avid_data_lane_submit"),
				FString::Printf(
					TEXT("data_lane_effect_prepare_failed at command[%d]."),
					CommandIndex));
			Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
			return 0;
		}
	}

	const double ApplyStartSeconds = FPlatformTime::Seconds();
	int32 AppliedCount = 0;
	for (; AppliedCount < PreparedWrites.Num(); ++AppliedCount)
	{
		FAvidScriptPreparedDataLaneWrite& Prepared = PreparedWrites[AppliedCount];
		int32 Value = Prepared.Value;
		const EAvidScriptVmTypedHostStatus Status = Prepared.SetCall != nullptr
			? Prepared.SetCall(*Prepared.Receiver, Value)
			: Prepared.Entry->PropertyI32Call(*Prepared.Receiver, true, Value);
		const bool bCheckApplyBudget =
			(AppliedCount + 1) % AvidScriptDataBridgeBudgetCheckStride == 0
			|| AppliedCount + 1 == PreparedWrites.Num();
		const bool bApplyBudgetExceeded =
			Status == EAvidScriptVmTypedHostStatus::Succeeded
			&& bCheckApplyBudget
			&& (FPlatformTime::Seconds() - ApplyStartSeconds) * 1000.0
				> DataBridgeBudget.MaxApplyMilliseconds;
		if (Status != EAvidScriptVmTypedHostStatus::Succeeded
			|| bApplyBudgetExceeded)
		{
			for (int32 RollbackIndex = AppliedCount; RollbackIndex >= 0; --RollbackIndex)
			{
				const FAvidScriptPreparedDataLaneWrite& Rollback =
					PreparedWrites[RollbackIndex];
				Rollback.Property->SetPropertyValue_InContainer(
					Rollback.Receiver,
					Rollback.PreviousValue);
			}
			++DataBridgeMetrics.RejectedBuffers;
			SetPendingHostImportFailure(
				TEXT("avidscript"),
				TEXT("avid_data_lane_submit"),
				Status != EAvidScriptVmTypedHostStatus::Succeeded
					? FString::Printf(
						TEXT("data_lane_apply_failed at command[%d]."),
						AppliedCount)
					: FString::Printf(
						TEXT("data_lane_apply_budget_exceeded after command[%d]."),
						AppliedCount));
			Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
			return 0;
		}
	}

	DataBridgeMetrics.AppliedCommands += static_cast<uint64>(AppliedCount);
	LastHostImportResult = AppliedCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return AppliedCount;
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
	if (!FAvidScriptActorBinding::GetActorLocation(
		*HostContext.ObjectRegistry,
		ActorHandle,
		OutLocation,
		BindingResult,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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
		HostContext.HostEffectJournal,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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
		HostContext.HostEffectJournal,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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
	if (!FAvidScriptActorBinding::GetActorRotation(
		*HostContext.ObjectRegistry,
		ActorHandle,
		OutRotation,
		BindingResult,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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
		HostContext.HostEffectJournal,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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
	if (!FAvidScriptActorBinding::GetActorScale3D(
		*HostContext.ObjectRegistry,
		ActorHandle,
		OutScale3D,
		BindingResult,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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
		HostContext.HostEffectJournal,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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
			BatchResult,
			EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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
	if (HostContext.ObjectOwnership == nullptr)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_root_component"),
			TEXT("Missing ownership domain for borrowed root component handle"));
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
	if (!FAvidScriptActorBinding::GetRootComponentHandle(
		*HostContext.ObjectRegistry,
		ActorHandle,
		OutComponentHandle,
		BindingResult,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath,
		HostContext.ObjectOwnership))
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
	if (!FAvidScriptSceneComponentBinding::GetWorldLocation(
		*HostContext.ObjectRegistry,
		ComponentHandle,
		OutWorldLocation,
		BindingResult,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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
		HostContext.HostEffectJournal,
		EAvidScriptBindingDiagnosticsPolicy::OmitObjectPath))
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

int64 FAvidScriptWasmRuntimeInstance::HandleContinuationDelayImport(
	const float DelaySeconds,
	const int32 CallbackId)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = CallbackId;
	LastHostImportResult = 0;
	++HostImportCallCount;

	const int64 Token = HostContext.Continuations != nullptr
		? HostContext.Continuations->ScheduleDelay(DelaySeconds, CallbackId)
		: 0;
	LastHostImportResult = Token != 0 ? 1 : 0;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return Token;
}

int64 FAvidScriptWasmRuntimeInstance::HandleContinuationLoadObjectImport(
	const int32 Utf8ValueReference,
	const int32 CallbackId)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = CallbackId;
	LastHostImportResult = 0;
	++HostImportCallCount;

	FString ObjectPath;
	FString DecodeError;
	IAvidScriptVmGuestMemory* const GuestMemory = VmBackend
		? VmBackend->GetGuestMemory()
		: nullptr;
	if (!DecodeAvidScriptUtf8ValueReference(
			static_cast<uint32>(Utf8ValueReference),
			GuestMemory,
			Utf8ValueHeap,
			ObjectPath,
			DecodeError))
	{
		SetPendingHostImportFailure(
			TEXT("env"),
			TEXT("continuation_load_object"),
			DecodeError.IsEmpty()
				? TEXT("continuation_object_path_decode_failed")
				: MoveTemp(DecodeError));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FSoftObjectPath SoftObjectPath(ObjectPath);
	const FString LongPackageName = SoftObjectPath.GetLongPackageName();
	if (SoftObjectPath.IsNull()
		|| !SoftObjectPath.IsValid()
		|| !FPackageName::IsValidLongPackageName(LongPackageName))
	{
		SetPendingHostImportFailure(
			TEXT("env"),
			TEXT("continuation_load_object"),
			TEXT("continuation_object_path_invalid"));
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	const int64 Token = HostContext.Continuations != nullptr
		? HostContext.Continuations->ScheduleObjectLoad(
			SoftObjectPath.ToString(),
			CallbackId)
		: 0;
	LastHostImportResult = Token != 0 ? 1 : 0;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return Token;
}

int32 FAvidScriptWasmRuntimeInstance::HandleContinuationCancelImport(
	const int64 ContinuationToken)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = 0;
	LastHostImportResult = HostContext.Continuations != nullptr
		&& HostContext.Continuations->Cancel(ContinuationToken)
		? 1
		: 0;
	++HostImportCallCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int64 FAvidScriptWasmRuntimeInstance::HandleContinuationCancelSourceCreateImport()
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = 0;
	const int64 Token = HostContext.Continuations != nullptr
		? HostContext.Continuations->CreateCancellationSource()
		: 0;
	LastHostImportResult = Token != 0 ? 1 : 0;
	++HostImportCallCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return Token;
}

int32 FAvidScriptWasmRuntimeInstance::HandleContinuationCancelSourceCancelImport(
	const int64 SourceToken)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = 0;
	LastHostImportResult = HostContext.Continuations != nullptr
		&& HostContext.Continuations->CancelCancellationSource(SourceToken)
		? 1
		: 0;
	++HostImportCallCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleContinuationCancelSourceReleaseImport(
	const int64 SourceToken)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = 0;
	LastHostImportResult = HostContext.Continuations != nullptr
		&& HostContext.Continuations->ReleaseCancellationSource(SourceToken)
		? 1
		: 0;
	++HostImportCallCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleContinuationBindCancelImport(
	const int64 SourceToken,
	const int64 ContinuationToken)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = 0;
	LastHostImportResult = HostContext.Continuations != nullptr
		&& HostContext.Continuations->BindCancellationSource(
			SourceToken,
			ContinuationToken)
		? 1
		: 0;
	++HostImportCallCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleContinuationStateStoreImport(
	const int64 ContinuationToken,
	const TConstArrayView<uint8> StateBytes)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = StateBytes.Num();
	LastHostImportResult = HostContext.Continuations != nullptr
		&& HostContext.Continuations->StoreState(ContinuationToken, StateBytes)
		? 1
		: 0;
	++HostImportCallCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleContinuationStateReadImport(
	const int64 ContinuationToken,
	const TArrayView<uint8> OutStateBytes)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = OutStateBytes.Num();
	LastHostImportResult = bContinuationDispatchActive
		&& !bContinuationStateConsumed
		&& ContinuationToken == ActiveContinuationToken
		&& HostContext.Continuations != nullptr
		&& HostContext.Continuations->ReadState(
			ContinuationToken,
			OutStateBytes)
		? 1
		: 0;
	if (LastHostImportResult != 0)
	{
		bContinuationStateConsumed = true;
	}
	++HostImportCallCount;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

int32 FAvidScriptWasmRuntimeInstance::HandleContinuationResultReadImport(
	const int32 BindingOrdinal,
	const int32 ResultSlot,
	const int32 ResultGeneration,
	TArrayView<uint8> OutBytes)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = BindingOrdinal;
	LastHostImportResult = 0;
	++HostImportCallCount;
	const auto Fail = [this, HostImportStartSeconds](const FString& Details)
	{
		SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("continuation_result_read"),
			Details);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	};

	const FAvidScriptBindingTypeModel* ResultType = nullptr;
	if (!bContinuationDispatchActive
		|| bContinuationResultConsumed
		|| ActiveContinuationToken == 0
		|| ActiveContinuationResultTransaction == nullptr
		|| !BindingPackage.IsValid()
		|| BindingOrdinal < 0
		|| !BindingPackage->TryGetLatentCompletionResultType(
			static_cast<uint32>(BindingOrdinal),
			ResultType)
		|| ResultType == nullptr
		|| ResultType->Size <= 0
		|| OutBytes.Num() != ResultType->Size)
	{
		return Fail(TEXT("continuation_result_dispatch_contract_mismatch"));
	}

	if (ActiveContinuationStatus == EAvidScriptContinuationStatus::Failed
		|| ActiveContinuationStatus == EAvidScriptContinuationStatus::Cancelled)
	{
		if (ResultSlot != 0
			|| ResultGeneration != 0
			|| ActiveContinuationResultSlot != 0
			|| ActiveContinuationResultGeneration != 0)
		{
			return Fail(TEXT("continuation_result_terminal_capability_invalid"));
		}
		FMemory::Memzero(OutBytes.GetData(), OutBytes.Num());
		bContinuationResultConsumed = true;
		LastHostImportResult = 1;
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 1;
	}
	if (ActiveContinuationStatus != EAvidScriptContinuationStatus::Completed
		|| ResultSlot <= 0
		|| ResultGeneration <= 0
		|| ResultSlot != ActiveContinuationResultSlot
		|| ResultGeneration != ActiveContinuationResultGeneration
		|| HostContext.Continuations == nullptr)
	{
		return Fail(TEXT("continuation_result_capability_invalid"));
	}

	FAvidScriptBindingLatentCompletionPayload Payload;
	if (!HostContext.Continuations->ConsumeResult(
			ActiveContinuationToken,
			ResultSlot,
			ResultGeneration,
			ResultType->StableId,
			Payload))
	{
		return Fail(TEXT("continuation_result_capability_stale"));
	}
	bContinuationResultConsumed = true;
	FString CodecError;
	if (!FAvidScriptContinuationResultCodec::Encode(
			Payload,
			*ResultType,
			*BindingPackage,
			HostContext.ObjectRegistry,
			HostContext.ObjectOwnership,
			Utf8ValueHeap,
			ArrayValueHeap,
			OutBytes,
			*ActiveContinuationResultTransaction,
			CodecError))
	{
		return Fail(CodecError.IsEmpty()
			? TEXT("continuation_result_codec_failed")
			: CodecError);
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
}

int64 FAvidScriptWasmRuntimeInstance::HandleEventSubscribeImport(
	const int32 Slot,
	const int32 Generation,
	const int32 EventOrdinal)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = EventOrdinal;
	LastHostImportResult = 0;
	++HostImportCallCount;

	const auto Fail = [this, HostImportStartSeconds](const FString& Details)
	{
		if (Details.StartsWith(TEXT("delegate_bridge_"))
			|| Details == TEXT("delegate_prepared_plan_invalid"))
		{
			SetPendingHostImportFailure(
				TEXT("env"),
				TEXT("event_subscribe"),
				Details);
		}
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	};
	if (!IsLoaded()
		|| HostContext.ObjectRegistry == nullptr
		|| HostContext.EventSubscriptions == nullptr
		|| HostContext.World.IsStale()
		|| HostContext.World.Get() == nullptr)
	{
		return Fail(TEXT("delegate_subscription_context_unavailable"));
	}
	if (Slot <= 0 || Generation <= 0 || EventOrdinal < 0)
	{
		return Fail(TEXT("delegate_subscription_argument_invalid"));
	}

	const FAvidScriptObjectHandle SourceHandle{
		static_cast<uint32>(Slot),
		static_cast<uint32>(Generation)
	};
	FAvidScriptObjectHandleResult ResolveResult;
	UObject* const Source = HostContext.ObjectRegistry->ResolveObject(
		SourceHandle,
		ResolveResult,
		false);
	if (Source == nullptr)
	{
		return Fail(
			ResolveResult.ErrorCategory.IsEmpty()
				? TEXT("delegate_source_handle_invalid")
				: ResolveResult.ErrorCategory);
	}
	if (SourceHandle != HostContext.OwnerHandle
		&& (HostContext.ObjectOwnership == nullptr
			|| !HostContext.ObjectOwnership->HasCapability(
				SourceHandle,
				Source)))
	{
		return Fail(TEXT("delegate_source_capability_denied"));
	}
	if (Source->GetWorld() == nullptr
		|| Source->GetWorld() != HostContext.World.Get())
	{
		return Fail(TEXT("delegate_source_world_mismatch"));
	}

	FString SubscribeError;
	const int64 SubscriptionToken = HostContext.EventSubscriptions->Subscribe(
		*Source,
		static_cast<uint32>(EventOrdinal),
		SubscribeError);
	if (SubscriptionToken <= 0)
	{
		return Fail(
			SubscribeError.IsEmpty()
				? TEXT("delegate_subscription_rejected")
				: SubscribeError);
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return SubscriptionToken;
}

int32 FAvidScriptWasmRuntimeInstance::HandleEventUnsubscribeImport(
	const int64 SubscriptionToken)
{
	const double HostImportStartSeconds = FPlatformTime::Seconds();
	LastHostImportInput = SubscriptionToken;
	LastHostImportResult = 0;
	++HostImportCallCount;
	if (!IsLoaded() || HostContext.EventSubscriptions == nullptr)
	{
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	FString UnsubscribeError;
	if (!HostContext.EventSubscriptions->Unsubscribe(
			SubscriptionToken,
			UnsubscribeError))
	{
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
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

bool FAvidScriptWasmRuntimeInstance::ExecuteDueTimerCallbacks(
	FAvidScriptVmError& OutError)
{
	OutError = FAvidScriptVmError();
	for (const FAvidScriptWasmTimerEntry& Timer : DueTimerScratch)
	{
		uint32 TimerArgs[2] = {
			static_cast<uint32>(Timer.CallbackId),
			static_cast<uint32>(Timer.Handle)
		};
		const double CallbackStartSeconds = FPlatformTime::Seconds();
		BeginTypedCallbackEpoch();
		const bool bTimerCalled = InvokeVmExport(
			VmBackend.Get(),
			TimerExport,
			AvidScriptTimerExportName,
			UE_ARRAY_COUNT(TimerArgs),
			TimerArgs,
			OutError);
		EndTypedCallbackEpoch();
		if (!bTimerCalled)
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

void FAvidScriptWasmRuntimeInstance::BeginTypedCallbackEpoch()
{
	++NextCallbackEpoch;
	if (NextCallbackEpoch == 0)
	{
		++NextCallbackEpoch;
	}
	FAvidScriptFusedCallbackFrame& Frame =
		FusedCallbackFrameStack.AddDefaulted_GetRef();
	Frame.CallbackEpoch = NextCallbackEpoch;
}

void FAvidScriptWasmRuntimeInstance::InvalidateSelfCapability()
{
	SelfCapability = FAvidScriptSelfCapability();
	++ReloadEpoch;
	if (ReloadEpoch == 0)
	{
		++ReloadEpoch;
	}
}

void FAvidScriptWasmRuntimeInstance::EndTypedCallbackEpoch()
{
	if (!FusedCallbackFrameStack.IsEmpty())
	{
		FusedCallbackFrameStack.Pop(EAllowShrinking::No);
	}
}

bool FAvidScriptWasmRuntimeInstance::ResolveSelfCapability(
	const int32 SelfSlot,
	const int32 SelfGeneration,
	UClass* ExpectedClass,
	UObject*& OutObject)
{
	OutObject = nullptr;
	if (!IsInGameThread()
		|| FusedCallbackFrameStack.IsEmpty()
		|| HostContext.World.IsStale()
		|| SelfSlot <= 0
		|| SelfGeneration <= 0
		|| HostContext.ObjectRegistry == nullptr)
	{
		return false;
	}
	const FAvidScriptObjectHandle RequestedHandle{
		static_cast<uint32>(SelfSlot),
		static_cast<uint32>(SelfGeneration)
	};
	if (!(RequestedHandle == HostContext.OwnerHandle))
	{
		return false;
	}

	const uint64 CallbackEpoch =
		FusedCallbackFrameStack.Last().CallbackEpoch;
	const uint64 RegistryRevision =
		HostContext.ObjectRegistry->GetRevision();
	if (SelfCapability.ReloadEpoch == ReloadEpoch
		&& SelfCapability.RegistryRevision == RegistryRevision
		&& SelfCapability.Handle == RequestedHandle)
	{
		OutObject = SelfCapability.Object.Get();
		return OutObject != nullptr
			&& (ExpectedClass == nullptr || OutObject->IsA(ExpectedClass))
			&& (HostContext.World.Get() == nullptr
				|| OutObject->GetWorld() == HostContext.World.Get());
	}

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* Object = HostContext.ObjectRegistry->ResolveObject(
		RequestedHandle,
		ResolveResult,
		false);
	if (Object == nullptr
		|| (ExpectedClass != nullptr && !Object->IsA(ExpectedClass))
		|| (HostContext.World.Get() != nullptr
			&& Object->GetWorld() != HostContext.World.Get()))
	{
		return false;
	}

	SelfCapability.Object = Object;
	SelfCapability.Handle = RequestedHandle;
	SelfCapability.ReloadEpoch = ReloadEpoch;
	SelfCapability.CallbackEpoch = CallbackEpoch;
	SelfCapability.RegistryRevision = RegistryRevision;
	OutObject = Object;
	return true;
}

UObject* FAvidScriptWasmRuntimeInstance::ResolveStableBorrow(
	const int32 Slot,
	const int32 Generation,
	UClass* ExpectedClass) const
{
	if (!IsInGameThread()
		|| FusedCallbackFrameStack.IsEmpty()
		|| HostContext.World.IsStale()
		|| Slot <= 0
		|| Generation <= 0
		|| HostContext.ObjectRegistry == nullptr)
	{
		return nullptr;
	}
	const FAvidScriptObjectHandle Handle{
		static_cast<uint32>(Slot),
		static_cast<uint32>(Generation)
	};
	FAvidScriptObjectHandleResult ResolveResult;
	UObject* Object = HostContext.ObjectRegistry->ResolveObject(
		Handle,
		ResolveResult,
		false);
	const bool bOwnerCapability = Handle == HostContext.OwnerHandle;
	if (Object == nullptr
		|| (ExpectedClass != nullptr && !Object->IsA(ExpectedClass))
		|| (HostContext.World.Get() != nullptr
			&& Object->GetWorld() != HostContext.World.Get())
		|| (!bOwnerCapability
			&& HostContext.ObjectOwnership != nullptr
			&& !HostContext.ObjectOwnership->Owns(Handle, Object)))
	{
		UE_LOG(
			LogAvidScriptWasmRuntime,
			Warning,
			TEXT("Stable borrow rejected | handle=%u:%u | registry=%s | object=%s | expected=%s | object_world=%s | host_world=%s | owned=%d"),
			Handle.Slot,
			Handle.Generation,
			ResolveResult.ErrorCategory.IsEmpty()
				? TEXT("resolved")
				: *ResolveResult.ErrorCategory,
			Object == nullptr ? TEXT("<none>") : *Object->GetPathName(),
			ExpectedClass == nullptr
				? TEXT("<none>")
				: *ExpectedClass->GetPathName(),
			Object == nullptr || Object->GetWorld() == nullptr
				? TEXT("<none>")
				: *Object->GetWorld()->GetPathName(),
			HostContext.World.Get() == nullptr
				? TEXT("<none>")
				: *HostContext.World->GetPathName(),
			Object != nullptr
				&& (bOwnerCapability
					|| HostContext.ObjectOwnership == nullptr
					|| HostContext.ObjectOwnership->Owns(Handle, Object))
				? 1
				: 0);
		return nullptr;
	}
	return Object;
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::RecordGeneratedStatus(
	const EAvidScriptVmTypedHostStatus Status)
{
	++HostImportCallCount;
	FAvidScriptBindingInvocationInstrumentation* Instrumentation =
		BindingInvocationContext.InvocationInstrumentation;
	if (Status == EAvidScriptVmTypedHostStatus::Succeeded)
	{
		if (Instrumentation != nullptr)
		{
			++Instrumentation->GeneratedNativeS1HitCount;
		}
		return Status;
	}
	if (Status == EAvidScriptVmTypedHostStatus::FallbackRequired)
	{
		if (Instrumentation != nullptr)
		{
			++Instrumentation->GeneratedNativeS1FallbackCount;
		}
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	if (Instrumentation != nullptr)
	{
		++Instrumentation->GeneratedNativeS1RejectCount;
	}
	return EAvidScriptVmTypedHostStatus::Rejected;
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::InvokePreparedSelfI32Pair(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 Left,
	const int32 Right,
	int32& OutValue)
{
	FAvidScriptPreparedGeneratedHostCall* Call =
		static_cast<FAvidScriptPreparedGeneratedHostCall*>(Context);
	if (Call == nullptr || Call->Runtime == nullptr)
	{
		OutValue = 0;
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	return Call->Runtime->DispatchPreparedSelfI32Pair(
		*Call,
		SelfSlot,
		SelfGeneration,
		Left,
		Right,
		OutValue);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchPreparedSelfI32Pair(
	FAvidScriptPreparedGeneratedHostCall& Call,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 Left,
	const int32 Right,
	int32& OutValue)
{
	OutValue = 0;
	if (Call.I32PairCall == nullptr)
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}

	UObject* Receiver = nullptr;
	if (!TryResolveFusedCallbackReceiver(
			SelfSlot,
			SelfGeneration,
			Receiver)
		|| !PrepareFusedGeneratedHostEffect(Call, *Receiver))
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}
	return RecordGeneratedStatus(
		Call.I32PairCall(*Receiver, Left, Right, OutValue));
}

bool
FAvidScriptWasmRuntimeInstance::ResolvePreparedReflectionCallMode(
	FAvidScriptPreparedReflectionHostCall& Call,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	UObject*& OutReceiver,
	bool& bOutUseNative,
	EAvidScriptBindingInvocationMode& OutMode,
	bool& bOutAdaptiveGuardRejected)
{
	OutReceiver = nullptr;
	bOutUseNative = false;
	bOutAdaptiveGuardRejected = false;
	OutMode = EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	if (!Call.Package.IsValid()
		|| !TryResolveFusedCallbackReceiver(
			SelfSlot,
			SelfGeneration,
			OutReceiver)
		|| OutReceiver == nullptr)
	{
		SetPendingHostImportFailure(
			Call.Binding.TypedHostImport.ModuleName,
			Call.Binding.TypedHostImport.ImportName,
			TEXT("The prepared reflection receiver or call cell is unavailable."));
		return false;
	}

	const bool bAdaptiveRequested =
		BindingInvocationContext.InvocationPolicy
			== EAvidScriptBindingInvocationPolicy::AdaptiveSemantic
		&& Call.Binding.bAdaptiveNativeEligible;
	const bool bQualifiedRequested =
		BindingInvocationContext.InvocationPolicy
			== EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect
		&& Call.Binding.bQualifiedNativeEligible;
	if (bAdaptiveRequested || bQualifiedRequested)
	{
		FAvidScriptFusedCallbackFrame& Frame =
			FusedCallbackFrameStack.Last();
		if (Frame.PreparedReflectionGuardIdentity
			!= Call.Binding.ImmutablePlanIdentity)
		{
			Frame.PreparedReflectionGuardIdentity =
				Call.Binding.ImmutablePlanIdentity;
			Frame.bPreparedReflectionNativeGuardAllowed =
				Call.Binding.NativeGuard != nullptr
				&& Call.Binding.NativeGuard(
					Call.Binding.ImmutablePlanIdentity,
					*OutReceiver);
		}
		bOutUseNative = Frame.bPreparedReflectionNativeGuardAllowed;
		bOutAdaptiveGuardRejected =
			bAdaptiveRequested && !bOutUseNative;
		if (bQualifiedRequested && !bOutUseNative)
		{
			SetPendingHostImportFailure(
				Call.Binding.TypedHostImport.ModuleName,
				Call.Binding.TypedHostImport.ImportName,
				TEXT("The qualified prepared reflection native guard rejected the receiver."));
			return false;
		}
		if (bOutUseNative)
		{
			OutMode = bQualifiedRequested
				? EAvidScriptBindingInvocationMode::QualifiedNativeDirect
				: EAvidScriptBindingInvocationMode::AdaptivePreparedNative;
		}
	}
	if (!bOutUseNative && !OutReceiver->IsA(Call.Binding.ExpectedClass))
	{
		SetPendingHostImportFailure(
			Call.Binding.TypedHostImport.ModuleName,
			Call.Binding.TypedHostImport.ImportName,
			TEXT("The prepared reflection receiver is incompatible with the call cell."));
		return false;
	}
	return true;
}

void FAvidScriptWasmRuntimeInstance::RecordPreparedReflectionInvocation(
	const EAvidScriptBindingInvocationMode Mode,
	const bool bAdaptiveGuardRejected)
{
	FAvidScriptBindingInvocationInstrumentation* Instrumentation =
		BindingInvocationContext.InvocationInstrumentation;
	if (Instrumentation == nullptr)
	{
		return;
	}
	if (Mode == EAvidScriptBindingInvocationMode::AdaptivePreparedNative)
	{
		++Instrumentation->AdaptivePreparedNativeHitCount;
		return;
	}
	if (Mode == EAvidScriptBindingInvocationMode::QualifiedNativeDirect)
	{
		++Instrumentation->QualifiedNativeDirectCount;
		return;
	}
	++Instrumentation->SemanticProcessEventCount;
	if (BindingInvocationContext.InvocationPolicy
		== EAvidScriptBindingInvocationPolicy::AdaptiveSemantic)
	{
		++Instrumentation->AdaptiveProcessEventFallbackCount;
		if (bAdaptiveGuardRejected)
		{
			++Instrumentation->AdaptiveGuardRejectCount;
		}
	}
	else if (BindingInvocationContext.InvocationPolicy
		== EAvidScriptBindingInvocationPolicy::QualifiedNativeDirect)
	{
		++Instrumentation->RequestedNativeDirectFallbackCount;
	}
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::
	InvokePreparedReflectionSelfI32PairGuestResult(
		void* Context,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 Left,
		const int32 Right,
		const int32 GuestAddress,
		int32& OutStatus)
{
	FAvidScriptPreparedReflectionHostCall* Call =
		static_cast<FAvidScriptPreparedReflectionHostCall*>(
			Context);
	if (Call == nullptr || Call->Runtime == nullptr)
	{
		OutStatus = 0;
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	return Call->Runtime->
		DispatchPreparedReflectionSelfI32PairGuestResult(
			*Call,
			SelfSlot,
			SelfGeneration,
			Left,
			Right,
			GuestAddress,
			OutStatus);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::
	DispatchPreparedReflectionSelfI32PairGuestResult(
		FAvidScriptPreparedReflectionHostCall& Call,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 Left,
		const int32 Right,
		const int32 GuestAddress,
		int32& OutStatus)
{
	++HostImportCallCount;
	OutStatus = 0;
	if (GuestAddress < 0)
	{
		SetPendingHostImportFailure(
			Call.Binding.TypedHostImport.ModuleName,
			Call.Binding.TypedHostImport.ImportName,
			TEXT("The prepared reflection receiver or call-site is unavailable."));
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	bool bUseNative = false;
	bool bAdaptiveGuardRejected = false;
	EAvidScriptBindingInvocationMode ActualMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	UObject* Receiver = nullptr;
	if (!ResolvePreparedReflectionCallMode(
			Call,
			SelfSlot,
			SelfGeneration,
			Receiver,
			bUseNative,
			ActualMode,
			bAdaptiveGuardRejected))
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	int32 ReturnValue = 0;
	FString ErrorCategory;
	FString ErrorDetails;
	if (!Call.Binding.I32PairCall(
			Call.Binding.ImmutablePlanIdentity,
			*Receiver,
			Left,
			Right,
			bUseNative,
			ReturnValue,
			ErrorCategory,
			ErrorDetails))
	{
		SetPendingHostImportFailure(
			Call.Binding.TypedHostImport.ModuleName,
			Call.Binding.TypedHostImport.ImportName,
			ErrorCategory.IsEmpty()
				? ErrorDetails
				: ErrorCategory + TEXT(": ") + ErrorDetails);
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	IAvidScriptVmGuestMemory* GuestMemory =
		VmBackend ? VmBackend->GetGuestMemory() : nullptr;
	FString MemoryError;
	if (GuestMemory == nullptr
		|| !GuestMemory->WriteBytes(
			static_cast<uint32>(GuestAddress),
			MakeArrayView(
				reinterpret_cast<const uint8*>(&ReturnValue),
				sizeof(ReturnValue)),
			MemoryError))
	{
		SetPendingHostImportFailure(
			Call.Binding.TypedHostImport.ModuleName,
			Call.Binding.TypedHostImport.ImportName,
			MemoryError.IsEmpty()
				? FString(TEXT("The prepared reflection return write failed."))
				: MoveTemp(MemoryError));
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	OutStatus = 1;
	RecordPreparedReflectionInvocation(
		ActualMode,
		bAdaptiveGuardRejected);
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::
	InvokePreparedReflectionSelfGuestAddress(
		void* Context,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 GuestAddressOrValue,
		int32& OutStatus)
{
	FAvidScriptPreparedReflectionHostCall* Call =
		static_cast<FAvidScriptPreparedReflectionHostCall*>(Context);
	if (Call == nullptr || Call->Runtime == nullptr)
	{
		OutStatus = 0;
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	return Call->Runtime->DispatchPreparedReflectionSelfGuestAddress(
		*Call,
		SelfSlot,
		SelfGeneration,
		GuestAddressOrValue,
		OutStatus);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::
	DispatchPreparedReflectionSelfGuestAddress(
		FAvidScriptPreparedReflectionHostCall& Call,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 GuestAddressOrValue,
		int32& OutStatus)
{
	++HostImportCallCount;
	OutStatus = 0;
	const auto Reject = [this, &Call](const FString& Details)
	{
		SetPendingHostImportFailure(
			Call.Binding.TypedHostImport.ModuleName,
			Call.Binding.TypedHostImport.ImportName,
			Details);
		return EAvidScriptVmTypedHostStatus::Rejected;
	};

	bool bUseNative = false;
	bool bAdaptiveGuardRejected = false;
	EAvidScriptBindingInvocationMode ActualMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	UObject* Receiver = nullptr;
	if (!ResolvePreparedReflectionCallMode(
			Call,
			SelfSlot,
			SelfGeneration,
			Receiver,
			bUseNative,
			ActualMode,
			bAdaptiveGuardRejected))
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	IAvidScriptVmGuestMemory* GuestMemory =
		VmBackend == nullptr ? nullptr : VmBackend->GetGuestMemory();
	TArrayView<uint8> OutputBytes;
	FString MemoryError;
	if (!Call.Binding.bPropertyWrite
		&& (GuestAddressOrValue < 0
			|| GuestMemory == nullptr
			|| !GuestMemory->BorrowMutableBytes(
				static_cast<uint32>(GuestAddressOrValue),
				sizeof(int32),
				alignof(int32),
				OutputBytes,
				MemoryError)))
	{
		return Reject(
			MemoryError.IsEmpty()
				? FString(TEXT("The prepared reflection property output buffer is unavailable."))
				: MoveTemp(MemoryError));
	}

	if (Call.Binding.bRequiresWriteAccess
		&& BindingInvocationContext.WritePolicy
			!= EAvidScriptActorWritePolicy::AllowWrites)
	{
		return Reject(
			TEXT("binding_write_denied: The reflected property requires an explicitly writable host context."));
	}
	if (Call.Binding.bPropertyWrite
		&& BindingInvocationContext.HostEffectJournal != nullptr)
	{
		if (BindingInvocationContext.ObjectRegistry == nullptr
			|| Call.Binding.ReloadEffect
				!= EAvidScriptBindingReloadEffect::ReflectedProperty
			|| Call.Binding.ReflectedProperty == nullptr)
		{
			return Reject(
				TEXT("binding_reload_effect_unsupported: The prepared property write has no reversible journal adapter."));
		}
		FAvidScriptBindingHostEffectPrepareResult PrepareResult;
		const FAvidScriptObjectHandle ReceiverHandle{
			static_cast<uint32>(SelfSlot),
			static_cast<uint32>(SelfGeneration)
		};
		if (!BindingInvocationContext.HostEffectJournal->
				PrepareReflectedProperty(
					*BindingInvocationContext.ObjectRegistry,
					ReceiverHandle,
					*Receiver,
					*Call.Binding.ReflectedProperty,
					PrepareResult))
		{
			const FString Category = PrepareResult.ErrorCategory.IsEmpty()
				? FString(TEXT("binding_host_effect_prepare_failed"))
				: PrepareResult.ErrorCategory;
			const FString Details = PrepareResult.ErrorDetails.IsEmpty()
				? FString(TEXT("The prepared property write could not be journaled."))
				: PrepareResult.ErrorDetails;
			return Reject(Category + TEXT(": ") + Details);
		}
	}

	if (Call.Binding.bPropertyWrite)
	{
		if (Call.Binding.PropertyI32Set == nullptr
			|| !Call.Binding.PropertyI32Set(
				Call.Binding.ImmutablePlanIdentity,
				*Receiver,
				GuestAddressOrValue))
		{
			return Reject(
				TEXT("binding_property_write_failed: The prepared property call cell rejected the write."));
		}
	}
	else
	{
		int32 Value = 0;
		if (Call.Binding.PropertyI32Get == nullptr
			|| !Call.Binding.PropertyI32Get(
				Call.Binding.ImmutablePlanIdentity,
				*Receiver,
				Value))
		{
			return Reject(
				TEXT("binding_property_read_failed: The prepared property call cell rejected the read."));
		}
		StoreAvidScriptLittleEndianU32(
			OutputBytes,
			0,
			static_cast<uint32>(Value));
	}

	OutStatus = 1;
	RecordPreparedReflectionInvocation(
		ActualMode,
		bAdaptiveGuardRejected);
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::
	InvokePreparedReflectionSelfF32TripleGuestVector(
		void* Context,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const float X,
		const float Y,
		const float Z,
		const int32 GuestAddress,
		int32& OutStatus)
{
	FAvidScriptPreparedReflectionHostCall* Call =
		static_cast<FAvidScriptPreparedReflectionHostCall*>(Context);
	if (Call == nullptr || Call->Runtime == nullptr)
	{
		OutStatus = 0;
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	return Call->Runtime->
		DispatchPreparedReflectionSelfF32TripleGuestVector(
			*Call,
			SelfSlot,
			SelfGeneration,
			X,
			Y,
			Z,
			GuestAddress,
			OutStatus);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::
	DispatchPreparedReflectionSelfF32TripleGuestVector(
		FAvidScriptPreparedReflectionHostCall& Call,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const float X,
		const float Y,
		const float Z,
		const int32 GuestAddress,
		int32& OutStatus)
{
	++HostImportCallCount;
	OutStatus = 0;
	const auto Reject = [this, &Call](const FString& Details)
	{
		SetPendingHostImportFailure(
			Call.Binding.TypedHostImport.ModuleName,
			Call.Binding.TypedHostImport.ImportName,
			Details);
		return EAvidScriptVmTypedHostStatus::Rejected;
	};
	if (GuestAddress < 0
		|| !FMath::IsFinite(X)
		|| !FMath::IsFinite(Y)
		|| !FMath::IsFinite(Z))
	{
		return Reject(
			TEXT("The prepared reflection vector input or output address is invalid."));
	}

	IAvidScriptVmGuestMemory* GuestMemory =
		VmBackend == nullptr ? nullptr : VmBackend->GetGuestMemory();
	TArrayView<uint8> OutputBytes;
	FString MemoryError;
	if (GuestMemory == nullptr
		|| !GuestMemory->BorrowMutableBytes(
			static_cast<uint32>(GuestAddress),
			3 * sizeof(float),
			alignof(float),
			OutputBytes,
			MemoryError))
	{
		return Reject(
			MemoryError.IsEmpty()
				? FString(TEXT("The prepared reflection vector output buffer is unavailable."))
				: MoveTemp(MemoryError));
	}

	bool bUseNative = false;
	bool bAdaptiveGuardRejected = false;
	EAvidScriptBindingInvocationMode ActualMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	UObject* Receiver = nullptr;
	if (!ResolvePreparedReflectionCallMode(
			Call,
			SelfSlot,
			SelfGeneration,
			Receiver,
			bUseNative,
			ActualMode,
			bAdaptiveGuardRejected))
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	FVector ReturnValue = FVector::ZeroVector;
	FString ErrorCategory;
	FString ErrorDetails;
	if (Call.Binding.VectorCall == nullptr
		|| !Call.Binding.VectorCall(
			Call.Binding.ImmutablePlanIdentity,
			*Receiver,
			FVector(
				static_cast<double>(X),
				static_cast<double>(Y),
				static_cast<double>(Z)),
			bUseNative,
			ReturnValue,
			ErrorCategory,
			ErrorDetails))
	{
		return Reject(
			ErrorCategory.IsEmpty()
				? ErrorDetails
				: ErrorCategory + TEXT(": ") + ErrorDetails);
	}

	StoreAvidScriptLittleEndianF32(
		OutputBytes,
		0,
		static_cast<float>(ReturnValue.X));
	StoreAvidScriptLittleEndianF32(
		OutputBytes,
		4,
		static_cast<float>(ReturnValue.Y));
	StoreAvidScriptLittleEndianF32(
		OutputBytes,
		8,
		static_cast<float>(ReturnValue.Z));
	OutStatus = 1;
	RecordPreparedReflectionInvocation(
		ActualMode,
		bAdaptiveGuardRejected);
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::
	InvokePreparedReflectionStableObjectRoundtrip(
		void* Context,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 ObjectSlot,
		const int32 ObjectGeneration,
		const int32 GuestAddress,
		int32& OutStatus)
{
	FAvidScriptPreparedReflectionHostCall* Call =
		static_cast<FAvidScriptPreparedReflectionHostCall*>(Context);
	if (Call == nullptr || Call->Runtime == nullptr)
	{
		OutStatus = 0;
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	return Call->Runtime->
		DispatchPreparedReflectionStableObjectRoundtrip(
			*Call,
			SelfSlot,
			SelfGeneration,
			ObjectSlot,
			ObjectGeneration,
			GuestAddress,
			OutStatus);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::
	DispatchPreparedReflectionStableObjectRoundtrip(
		FAvidScriptPreparedReflectionHostCall& Call,
		const int32 SelfSlot,
		const int32 SelfGeneration,
		const int32 ObjectSlot,
		const int32 ObjectGeneration,
		const int32 GuestAddress,
		int32& OutStatus)
{
	++HostImportCallCount;
	OutStatus = 0;
	const auto Reject = [this, &Call](const FString& Details)
	{
		SetPendingHostImportFailure(
			Call.Binding.TypedHostImport.ModuleName,
			Call.Binding.TypedHostImport.ImportName,
			Details);
		return EAvidScriptVmTypedHostStatus::Rejected;
	};
	if (GuestAddress < 0)
	{
		return Reject(
			TEXT("The prepared reflection object output address is invalid."));
	}

	IAvidScriptVmGuestMemory* GuestMemory =
		VmBackend == nullptr ? nullptr : VmBackend->GetGuestMemory();
	TArrayView<uint8> OutputBytes;
	FString MemoryError;
	if (GuestMemory == nullptr
		|| !GuestMemory->BorrowMutableBytes(
			static_cast<uint32>(GuestAddress),
			2 * sizeof(uint32),
			alignof(uint32),
			OutputBytes,
			MemoryError))
	{
		return Reject(
			MemoryError.IsEmpty()
				? FString(TEXT("The prepared reflection object output buffer is unavailable."))
				: MoveTemp(MemoryError));
	}

	bool bUseNative = false;
	bool bAdaptiveGuardRejected = false;
	EAvidScriptBindingInvocationMode ActualMode =
		EAvidScriptBindingInvocationMode::SemanticProcessEvent;
	UObject* Receiver = nullptr;
	if (!ResolvePreparedReflectionCallMode(
			Call,
			SelfSlot,
			SelfGeneration,
			Receiver,
			bUseNative,
			ActualMode,
			bAdaptiveGuardRejected))
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	UObject* InputObject = nullptr;
	const FAvidScriptObjectHandle InputHandle{
		static_cast<uint32>(ObjectSlot),
		static_cast<uint32>(ObjectGeneration)
	};
	if (ObjectSlot != 0 || ObjectGeneration != 0)
	{
		const bool bInputAliasesReceiver =
			ObjectSlot == SelfSlot
			&& ObjectGeneration == SelfGeneration;
		InputObject = bInputAliasesReceiver
			? Receiver
			: ResolveStableBorrow(
				ObjectSlot,
				ObjectGeneration,
				Call.Binding.ExpectedObjectClass);
		if (InputObject == nullptr)
		{
			return Reject(
				TEXT("The prepared reflection input object capability is invalid."));
		}
		if (bInputAliasesReceiver
			&& Call.Binding.ExpectedObjectClass != nullptr
			&& !InputObject->IsA(Call.Binding.ExpectedObjectClass))
		{
			return Reject(
				TEXT("The prepared reflection input object type is invalid."));
		}
	}

	UObject* OutputObject = nullptr;
	FString ErrorCategory;
	FString ErrorDetails;
	if (Call.Binding.ObjectCall == nullptr
		|| !Call.Binding.ObjectCall(
			Call.Binding.ImmutablePlanIdentity,
			*Receiver,
			InputObject,
			bUseNative,
			OutputObject,
			ErrorCategory,
			ErrorDetails))
	{
		return Reject(
			ErrorCategory.IsEmpty()
				? ErrorDetails
				: ErrorCategory + TEXT(": ") + ErrorDetails);
	}

	FAvidScriptObjectHandle OutputHandle;
	if (OutputObject != nullptr)
	{
		if (OutputObject == InputObject && InputHandle.IsValid())
		{
			OutputHandle = InputHandle;
		}
		else if (OutputObject == Receiver)
		{
			OutputHandle = {
				static_cast<uint32>(SelfSlot),
				static_cast<uint32>(SelfGeneration)
			};
		}
		else if (BindingInvocationContext.ObjectRegistry == nullptr
			|| BindingInvocationContext.ObjectOwnership == nullptr)
		{
			return Reject(
				TEXT("The prepared reflection object result requires registry and ownership services."));
		}
		else
		{
			FAvidScriptObjectHandleResult BorrowResult;
			if (!BindingInvocationContext.ObjectOwnership->Borrow(
					*BindingInvocationContext.ObjectRegistry,
					*OutputObject,
					BorrowResult)
				|| !BorrowResult.Handle.IsValid())
			{
				return Reject(
					BorrowResult.ErrorMessage.IsEmpty()
						? FString(TEXT("The prepared reflection object result could not be borrowed."))
						: BorrowResult.ErrorMessage);
			}
			OutputHandle = BorrowResult.Handle;
		}
	}

	StoreAvidScriptLittleEndianU32(
		OutputBytes,
		0,
		OutputHandle.Slot);
	StoreAvidScriptLittleEndianU32(
		OutputBytes,
		4,
		OutputHandle.Generation);
	OutStatus = 1;
	RecordPreparedReflectionInvocation(
		ActualMode,
		bAdaptiveGuardRejected);
	return EAvidScriptVmTypedHostStatus::Succeeded;
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::InvokePreparedSelfPropertyI32Get(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	int32& OutValue)
{
	FAvidScriptPreparedGeneratedHostCall* Call =
		static_cast<FAvidScriptPreparedGeneratedHostCall*>(Context);
	if (Call == nullptr || Call->Runtime == nullptr)
	{
		OutValue = 0;
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	return Call->Runtime->DispatchPreparedSelfPropertyI32Get(
		*Call,
		SelfSlot,
		SelfGeneration,
		OutValue);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchPreparedSelfPropertyI32Get(
	FAvidScriptPreparedGeneratedHostCall& Call,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	int32& OutValue)
{
	OutValue = 0;
	if (Call.PropertyI32GetCall == nullptr)
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}

	UObject* Receiver = nullptr;
	if (!TryResolveFusedCallbackReceiver(
			SelfSlot,
			SelfGeneration,
			Receiver)
		|| !PrepareFusedGeneratedHostEffect(Call, *Receiver))
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}
	return RecordGeneratedStatus(
		Call.PropertyI32GetCall(*Receiver, OutValue));
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::InvokePreparedSelfPropertyI32Set(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 Value)
{
	FAvidScriptPreparedGeneratedHostCall* Call =
		static_cast<FAvidScriptPreparedGeneratedHostCall*>(Context);
	if (Call == nullptr || Call->Runtime == nullptr)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	return Call->Runtime->DispatchPreparedSelfPropertyI32Set(
		*Call,
		SelfSlot,
		SelfGeneration,
		Value);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchPreparedSelfPropertyI32Set(
	FAvidScriptPreparedGeneratedHostCall& Call,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 Value)
{
	if (Call.PropertyI32SetCall == nullptr)
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}

	UObject* Receiver = nullptr;
	if (!TryResolveFusedCallbackReceiver(
			SelfSlot,
			SelfGeneration,
			Receiver)
		|| !PrepareFusedGeneratedHostEffect(Call, *Receiver))
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}
	return RecordGeneratedStatus(
		Call.PropertyI32SetCall(*Receiver, Value));
}

bool FAvidScriptWasmRuntimeInstance::TryResolveFusedCallbackReceiver(
	const int32 SelfSlot,
	const int32 SelfGeneration,
	UObject*& OutReceiver)
{
	OutReceiver = nullptr;
	if (!IsInGameThread()
		|| FusedCallbackFrameStack.IsEmpty()
		|| HostContext.World.IsStale()
		|| SelfSlot <= 0
		|| SelfGeneration <= 0
		|| HostContext.ObjectRegistry == nullptr)
	{
		return false;
	}

	const FAvidScriptObjectHandle RequestedHandle{
		static_cast<uint32>(SelfSlot),
		static_cast<uint32>(SelfGeneration)
	};
	FAvidScriptFusedCallbackFrame& Frame =
		FusedCallbackFrameStack.Last();
	const uint64 RegistryRevision =
		HostContext.ObjectRegistry->GetRevisionGameThreadFast();
	FAvidScriptBindingInvocationInstrumentation* Instrumentation =
		BindingInvocationContext.InvocationInstrumentation;
	const bool bCaptureTiming =
		Instrumentation != nullptr
		&& HostContext.DynamicHostCallTimingPolicy
			== EAvidScriptDynamicHostCallTimingPolicy::PerCall;
	const uint64 ResolveStartCycles =
		bCaptureTiming ? FPlatformTime::Cycles64() : 0;
	if (Frame.ReloadEpoch == ReloadEpoch
		&& Frame.RegistryRevision == RegistryRevision
		&& Frame.Handle == RequestedHandle
		&& IsValid(Frame.Receiver))
	{
		if (Instrumentation != nullptr)
		{
			++Instrumentation->GeneratedFusedFastHitCount;
			if (bCaptureTiming)
			{
				Instrumentation->GeneratedFusedFastHitCycles +=
					FMath::Max<uint64>(
						FPlatformTime::Cycles64() - ResolveStartCycles,
						1);
			}
		}
		OutReceiver = Frame.Receiver;
		return true;
	}

	if (Instrumentation != nullptr)
	{
		++Instrumentation->GeneratedFusedRevalidateCount;
	}

	UObject* Receiver = nullptr;
	if (!ResolveSelfCapability(
			SelfSlot,
			SelfGeneration,
			nullptr,
			Receiver))
	{
		return false;
	}

	Frame.Receiver = Receiver;
	Frame.Handle = RequestedHandle;
	Frame.ReloadEpoch = ReloadEpoch;
	Frame.RegistryRevision = RegistryRevision;
	OutReceiver = Receiver;
	if (bCaptureTiming)
	{
		Instrumentation->GeneratedFusedRevalidateCycles +=
			FMath::Max<uint64>(
				FPlatformTime::Cycles64() - ResolveStartCycles,
				1);
	}
	return true;
}

bool FAvidScriptWasmRuntimeInstance::PrepareFusedGeneratedHostEffect(
	FAvidScriptPreparedGeneratedHostCall& Call,
	UObject& Receiver)
{
	if (Call.Binding.Lease.GetEntryGameThreadFast()
		!= Call.Binding.Entry)
	{
		Call.PreparedCallbackEpoch = 0;
		Call.PreparedReloadEpoch = 0;
		Call.EffectMode = EAvidScriptPreparedHostEffectMode::Rejected;
		return false;
	}
	const FAvidScriptFusedCallbackFrame& Frame =
		FusedCallbackFrameStack.Last();
	if (Call.PreparedCallbackEpoch != Frame.CallbackEpoch
		|| Call.PreparedReloadEpoch != ReloadEpoch)
	{
		const bool bCaptureTiming =
			HostContext.DynamicHostCallTimingPolicy
				== EAvidScriptDynamicHostCallTimingPolicy::PerCall;
		const uint64 PrepareStartCycles =
			bCaptureTiming ? FPlatformTime::Cycles64() : 0;
		if (FAvidScriptBindingInvocationInstrumentation* Instrumentation =
				BindingInvocationContext.InvocationInstrumentation)
		{
			++Instrumentation->GeneratedFusedCallSitePrepareCount;
		}
		Call.PreparedCallbackEpoch = 0;
		Call.PreparedReloadEpoch = 0;
		Call.EffectMode = EAvidScriptPreparedHostEffectMode::Rejected;
		if (!BindingPackage.IsValid()
			|| Call.Binding.Entry == nullptr
			|| (Call.Binding.ExpectedClass != nullptr
				&& !Receiver.IsA(Call.Binding.ExpectedClass)))
		{
			return false;
		}
		Call.EffectMode =
			BindingPackage->ResolvePreparedHostEffectMode(
				Call.Binding,
				BindingInvocationContext);
		if (Call.EffectMode
			== EAvidScriptPreparedHostEffectMode::Rejected)
		{
			return false;
		}
		if (FAvidScriptBindingInvocationInstrumentation* Instrumentation =
				BindingInvocationContext.InvocationInstrumentation)
		{
			if (Call.EffectMode
				== EAvidScriptPreparedHostEffectMode::DirectRead)
			{
				++Instrumentation->GeneratedDirectReadPrepareCount;
			}
			else if (Call.EffectMode
				== EAvidScriptPreparedHostEffectMode::DirectWrite)
			{
				++Instrumentation->GeneratedDirectWritePrepareCount;
			}
		}
		Call.PreparedCallbackEpoch = Frame.CallbackEpoch;
		Call.PreparedReloadEpoch = ReloadEpoch;
		if (bCaptureTiming)
		{
			if (FAvidScriptBindingInvocationInstrumentation* Instrumentation =
					BindingInvocationContext.InvocationInstrumentation)
			{
				Instrumentation->GeneratedFusedCallSitePrepareCycles +=
					FMath::Max<uint64>(
						FPlatformTime::Cycles64() - PrepareStartCycles,
						1);
			}
		}
	}
	if (Call.EffectMode != EAvidScriptPreparedHostEffectMode::Journaled)
	{
		return Call.EffectMode
			== EAvidScriptPreparedHostEffectMode::DirectRead
			|| Call.EffectMode
				== EAvidScriptPreparedHostEffectMode::DirectWrite;
	}
	if (FAvidScriptBindingInvocationInstrumentation* Instrumentation =
			BindingInvocationContext.InvocationInstrumentation)
	{
		++Instrumentation->GeneratedJournalSlowPathCount;
	}
	const bool bCaptureTiming =
		HostContext.DynamicHostCallTimingPolicy
			== EAvidScriptDynamicHostCallTimingPolicy::PerCall;
	const uint64 JournalStartCycles =
		bCaptureTiming ? FPlatformTime::Cycles64() : 0;
	const bool bPrepared = BindingPackage.IsValid()
		&& BindingPackage->PrepareGeneratedHostEffect(
			Call.Binding,
			HostContext.OwnerHandle,
			Receiver,
			BindingInvocationContext);
	if (bCaptureTiming)
	{
		if (FAvidScriptBindingInvocationInstrumentation* Instrumentation =
				BindingInvocationContext.InvocationInstrumentation)
		{
			Instrumentation->GeneratedJournalSlowPathCycles +=
				FMath::Max<uint64>(
					FPlatformTime::Cycles64() - JournalStartCycles,
					1);
		}
	}
	return bPrepared;
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchEmptyI32(
	const uint32 BindingOrdinal,
	int32& OutValue)
{
	OutValue = 0;
	return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchI32PairToI32(
	const uint32 BindingOrdinal,
	const int32 Left,
	const int32 Right,
	int32& OutValue)
{
	OutValue = 0;
	// This control shape is deliberately receiver-free and never resolves UObject.
	return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchSelfI32PairToI32(
	const uint32 BindingOrdinal,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 Left,
	const int32 Right,
	int32& OutValue)
{
	OutValue = 0;
	const FAvidScriptGeneratedBindingEntry* Entry = nullptr;
	UClass* ExpectedClass = nullptr;
	bool bPropertyWrite = false;
	bool bRequiresWriteAccess = false;
	if (!IsInGameThread()
		|| !BindingPackage.IsValid()
		|| !BindingPackage->TryGetGeneratedBinding(
			BindingOrdinal,
			Entry,
			ExpectedClass,
			bPropertyWrite,
			bRequiresWriteAccess)
		|| Entry->Shape != EAvidScriptGeneratedBindingShape::I32PairToI32
		|| Entry->ReceiverMode != EAvidScriptGeneratedReceiverMode::SelfBound
		|| Entry->I32PairCall == nullptr
		|| (bRequiresWriteAccess
			&& HostContext.ActorWritePolicy
				!= EAvidScriptActorWritePolicy::AllowWrites))
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}
	UObject* Receiver = nullptr;
	if (!ResolveSelfCapability(
			SelfSlot,
			SelfGeneration,
			ExpectedClass,
			Receiver))
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}
	if (!BindingPackage->PrepareGeneratedHostEffect(
			BindingOrdinal,
			HostContext.OwnerHandle,
			*Receiver,
			BindingInvocationContext))
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}
	return RecordGeneratedStatus(
		Entry->I32PairCall(*Receiver, Left, Right, OutValue));
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchSelfPropertyI32GetSet(
	const uint32 BindingOrdinal,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 GuestAddress,
	int32& OutValue)
{
	OutValue = 0;
	const FAvidScriptGeneratedBindingEntry* Entry = nullptr;
	UClass* ExpectedClass = nullptr;
	bool bPropertyWrite = false;
	bool bRequiresWriteAccess = false;
	UObject* Receiver = nullptr;
	if (!BindingPackage.IsValid()
		|| !BindingPackage->TryGetGeneratedBinding(
			BindingOrdinal,
			Entry,
			ExpectedClass,
			bPropertyWrite,
			bRequiresWriteAccess)
		|| Entry->Shape != EAvidScriptGeneratedBindingShape::PropertyI32GetSet
		|| Entry->ReceiverMode != EAvidScriptGeneratedReceiverMode::SelfBound
		|| Entry->PropertyI32Call == nullptr
		|| (bRequiresWriteAccess
			&& HostContext.ActorWritePolicy
				!= EAvidScriptActorWritePolicy::AllowWrites)
		|| !ResolveSelfCapability(
			SelfSlot,
			SelfGeneration,
			ExpectedClass,
			Receiver)
		|| (!bPropertyWrite
			&& (GuestAddress < 0
				|| VmBackend == nullptr
				|| VmBackend->GetGuestMemory() == nullptr)))
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}

	IAvidScriptVmGuestMemory* GuestMemory =
		VmBackend == nullptr ? nullptr : VmBackend->GetGuestMemory();
	FString Error;
	if (!bPropertyWrite)
	{
		TArrayView<uint8> ValidationBytes;
		if (GuestMemory == nullptr
			|| !GuestMemory->BorrowMutableBytes(
				static_cast<uint32>(GuestAddress),
				sizeof(int32),
				alignof(int32),
				ValidationBytes,
				Error))
		{
			return RecordGeneratedStatus(
				EAvidScriptVmTypedHostStatus::Rejected);
		}
	}
	if (!BindingPackage->PrepareGeneratedHostEffect(
			BindingOrdinal,
			HostContext.OwnerHandle,
			*Receiver,
			BindingInvocationContext))
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}

	int32 Value = bPropertyWrite ? GuestAddress : 0;
	const EAvidScriptVmTypedHostStatus Status =
		Entry->PropertyI32Call(*Receiver, bPropertyWrite, Value);
	if (Status != EAvidScriptVmTypedHostStatus::Succeeded)
	{
		return RecordGeneratedStatus(Status);
	}
	if (!bPropertyWrite)
	{
		TArrayView<uint8> OutputBytes;
		if (!GuestMemory->BorrowMutableBytes(
				static_cast<uint32>(GuestAddress),
				sizeof(int32),
				alignof(int32),
				OutputBytes,
				Error))
		{
			return RecordGeneratedStatus(
				EAvidScriptVmTypedHostStatus::Rejected);
		}
		StoreAvidScriptLittleEndianU32(
			OutputBytes,
			0,
			static_cast<uint32>(Value));
	}
	OutValue = 1;
	return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Succeeded);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchSelfVectorValue(
	const uint32 BindingOrdinal,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 GuestAddress,
	int32& OutValue)
{
	OutValue = 0;
	const FAvidScriptGeneratedBindingEntry* Entry = nullptr;
	UClass* ExpectedClass = nullptr;
	bool bPropertyWrite = false;
	bool bRequiresWriteAccess = false;
	UObject* Receiver = nullptr;
	if (!BindingPackage.IsValid()
		|| !BindingPackage->TryGetGeneratedBinding(
			BindingOrdinal,
			Entry,
			ExpectedClass,
			bPropertyWrite,
			bRequiresWriteAccess)
		|| Entry->Shape != EAvidScriptGeneratedBindingShape::VectorValue
		|| Entry->ReceiverMode != EAvidScriptGeneratedReceiverMode::SelfBound
		|| Entry->VectorValueCall == nullptr
		|| (bRequiresWriteAccess
			&& HostContext.ActorWritePolicy
				!= EAvidScriptActorWritePolicy::AllowWrites)
		|| !ResolveSelfCapability(
			SelfSlot,
			SelfGeneration,
			ExpectedClass,
			Receiver)
		|| GuestAddress < 0
		|| VmBackend == nullptr
		|| VmBackend->GetGuestMemory() == nullptr)
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}

	FString Error;
	IAvidScriptVmGuestMemory* GuestMemory = VmBackend->GetGuestMemory();
	FVector Input = FVector::ZeroVector;
	{
		TArrayView<uint8> InputBytes;
		if (!GuestMemory->BorrowMutableBytes(
				static_cast<uint32>(GuestAddress),
				24,
				4,
				InputBytes,
				Error))
		{
			return RecordGeneratedStatus(
				EAvidScriptVmTypedHostStatus::Rejected);
		}
		Input = FVector(
			LoadAvidScriptLittleEndianF32(InputBytes, 0),
			LoadAvidScriptLittleEndianF32(InputBytes, 4),
			LoadAvidScriptLittleEndianF32(InputBytes, 8));
	}
	if (!BindingPackage->PrepareGeneratedHostEffect(
			BindingOrdinal,
			HostContext.OwnerHandle,
			*Receiver,
			BindingInvocationContext))
	{
		return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Rejected);
	}
	FVector Output = FVector::ZeroVector;
	const EAvidScriptVmTypedHostStatus Status =
		Entry->VectorValueCall(*Receiver, Input, Output);
	if (Status != EAvidScriptVmTypedHostStatus::Succeeded)
	{
		return RecordGeneratedStatus(Status);
	}
	{
		TArrayView<uint8> OutputBytes;
		if (!GuestMemory->BorrowMutableBytes(
				static_cast<uint32>(GuestAddress),
				24,
				4,
				OutputBytes,
				Error))
		{
			return RecordGeneratedStatus(
				EAvidScriptVmTypedHostStatus::Rejected);
		}
		StoreAvidScriptLittleEndianF32(
			OutputBytes,
			12,
			static_cast<float>(Output.X));
		StoreAvidScriptLittleEndianF32(
			OutputBytes,
			16,
			static_cast<float>(Output.Y));
		StoreAvidScriptLittleEndianF32(
			OutputBytes,
			20,
			static_cast<float>(Output.Z));
	}
	OutValue = 1;
	return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Succeeded);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchStableObjectRoundtrip(
	const uint32 BindingOrdinal,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 ObjectSlot,
	const int32 ObjectGeneration,
	const int32 GuestAddress,
	int32& OutValue)
{
	OutValue = 0;
	const auto Reject = [this, BindingOrdinal](const TCHAR* Reason)
	{
		UE_LOG(
			LogAvidScriptWasmRuntime,
			Warning,
			TEXT("Generated stable object roundtrip rejected | ordinal=%u | reason=%s"),
			BindingOrdinal,
			Reason);
		return RecordGeneratedStatus(
			EAvidScriptVmTypedHostStatus::Rejected);
	};
	const FAvidScriptGeneratedBindingEntry* Entry = nullptr;
	UClass* ExpectedClass = nullptr;
	bool bPropertyWrite = false;
	bool bRequiresWriteAccess = false;
	if (!BindingPackage.IsValid()
		|| !BindingPackage->TryGetGeneratedBinding(
			BindingOrdinal,
			Entry,
			ExpectedClass,
			bPropertyWrite,
			bRequiresWriteAccess)
		)
	{
		return Reject(TEXT("binding_plan"));
	}
	if (Entry->Shape
			!= EAvidScriptGeneratedBindingShape::StableObjectRoundtrip
		|| Entry->ReceiverMode
			!= EAvidScriptGeneratedReceiverMode::StableBorrow
		|| Entry->ObjectRoundtripCall == nullptr)
	{
		return Reject(TEXT("generated_entry"));
	}
	if ((bRequiresWriteAccess
			&& HostContext.ActorWritePolicy
				!= EAvidScriptActorWritePolicy::AllowWrites))
	{
		return Reject(TEXT("write_policy"));
	}
	if (GuestAddress < 0
		|| VmBackend == nullptr
		|| VmBackend->GetGuestMemory() == nullptr)
	{
		return Reject(TEXT("guest_memory"));
	}

	IAvidScriptVmGuestMemory* GuestMemory = VmBackend->GetGuestMemory();
	FString Error;
	{
		TArrayView<uint8> ValidationBytes;
		if (!GuestMemory->BorrowMutableBytes(
				static_cast<uint32>(GuestAddress),
				8,
				4,
				ValidationBytes,
				Error))
		{
			return Reject(TEXT("guest_input_buffer"));
		}
	}

	UObject* Receiver = ResolveStableBorrow(
		SelfSlot,
		SelfGeneration,
		ExpectedClass);
	UObject* InputObject = nullptr;
	if (ObjectSlot != 0 || ObjectGeneration != 0)
	{
		InputObject = ResolveStableBorrow(
			ObjectSlot,
			ObjectGeneration,
			nullptr);
	}
	if (Receiver == nullptr)
	{
		UE_LOG(
			LogAvidScriptWasmRuntime,
			Warning,
			TEXT("Generated receiver borrow context | self=%d:%d | owner=%u:%u | expected=%s | callbacks=%d | world_stale=%d"),
			SelfSlot,
			SelfGeneration,
			HostContext.OwnerHandle.Slot,
			HostContext.OwnerHandle.Generation,
			ExpectedClass == nullptr
				? TEXT("<none>")
				: *ExpectedClass->GetPathName(),
			FusedCallbackFrameStack.Num(),
			HostContext.World.IsStale() ? 1 : 0);
		return Reject(TEXT("receiver_stable_borrow"));
	}
	if ((ObjectSlot != 0 || ObjectGeneration != 0)
		&& InputObject == nullptr)
	{
		return Reject(TEXT("input_stable_borrow"));
	}
	const FAvidScriptObjectHandle ReceiverHandle{
		static_cast<uint32>(SelfSlot),
		static_cast<uint32>(SelfGeneration)
	};
	if (!BindingPackage->PrepareGeneratedHostEffect(
			BindingOrdinal,
			ReceiverHandle,
			*Receiver,
			BindingInvocationContext))
	{
		return Reject(TEXT("host_effect"));
	}

	UObject* OutputObject = nullptr;
	const EAvidScriptVmTypedHostStatus Status =
		Entry->ObjectRoundtripCall(*Receiver, InputObject, OutputObject);
	if (Status != EAvidScriptVmTypedHostStatus::Succeeded)
	{
		UE_LOG(
			LogAvidScriptWasmRuntime,
			Warning,
			TEXT("Generated stable object roundtrip call failed | ordinal=%u | status=%d"),
			BindingOrdinal,
			static_cast<int32>(Status));
		return RecordGeneratedStatus(Status);
	}

	FAvidScriptObjectHandle OutputHandle;
	if (OutputObject != nullptr)
	{
		if (HostContext.World.IsStale()
			|| (HostContext.World.Get() != nullptr
				&& OutputObject->GetWorld() != HostContext.World.Get()))
		{
			return Reject(TEXT("output_world"));
		}
		FAvidScriptObjectHandleResult HandleResult;
		if (HostContext.ObjectOwnership != nullptr)
		{
			if (!HostContext.ObjectOwnership->Borrow(
					*HostContext.ObjectRegistry,
					*OutputObject,
					HandleResult))
			{
				return Reject(TEXT("output_borrow"));
			}
			OutputHandle = HandleResult.Handle;
		}
		else
		{
			OutputHandle = HostContext.ObjectRegistry->AcquireBorrowedObject(
				OutputObject,
				HandleResult,
				false);
		}
		if (!OutputHandle.IsValid())
		{
			return Reject(TEXT("output_handle"));
		}
	}

	TArrayView<uint8> OutputBytes;
	if (!GuestMemory->BorrowMutableBytes(
			static_cast<uint32>(GuestAddress),
			8,
			4,
			OutputBytes,
			Error))
	{
		if (OutputHandle.IsValid())
		{
			FAvidScriptObjectHandleResult ReleaseResult;
			if (HostContext.ObjectOwnership != nullptr)
			{
				HostContext.ObjectOwnership->Release(
					OutputHandle,
					*HostContext.ObjectRegistry,
					ReleaseResult);
			}
			else
			{
				HostContext.ObjectRegistry->ReleaseBorrowedHandle(
					OutputHandle,
					ReleaseResult,
					false);
			}
		}
		return Reject(TEXT("guest_output_buffer"));
	}
	StoreAvidScriptLittleEndianU32(OutputBytes, 0, OutputHandle.Slot);
	StoreAvidScriptLittleEndianU32(
		OutputBytes,
		4,
		OutputHandle.Generation);

	OutValue = 1;
	return RecordGeneratedStatus(EAvidScriptVmTypedHostStatus::Succeeded);
}

EAvidScriptVmTypedHostStatus
FAvidScriptWasmRuntimeInstance::DispatchCommandBufferSubmit(
	const uint32 BindingOrdinal,
	const int32 GuestAddress,
	const int32 ByteCount,
	int32& OutValue)
{
	OutValue = 0;
	IAvidScriptVmGuestMemory* GuestMemory =
		VmBackend == nullptr ? nullptr : VmBackend->GetGuestMemory();
	TConstArrayView<uint8> Bytes;
	FString Error;
	if (BindingOrdinal == MAX_uint32
		|| GuestAddress <= 0
		|| ByteCount <= 0
		|| GuestMemory == nullptr
		|| !GuestMemory->BorrowReadOnlyBytes(
			static_cast<uint32>(GuestAddress),
			static_cast<uint32>(ByteCount),
			alignof(uint32),
			Bytes,
			Error))
	{
		++HostImportCallCount;
		++DataBridgeMetrics.RejectedBuffers;
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	OutValue = HandleDataLaneSubmitImport(Bytes);
	return OutValue > 0
		? EAvidScriptVmTypedHostStatus::Succeeded
		: EAvidScriptVmTypedHostStatus::Rejected;
}

bool FAvidScriptWasmRuntimeInstance::InvokePreparedDynamicHost(
	void* Context,
	const TConstArrayView<uint64> Arguments,
	IAvidScriptVmGuestMemory& GuestMemory,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	FAvidScriptPreparedDynamicHostCall* Call =
		static_cast<FAvidScriptPreparedDynamicHostCall*>(Context);
	if (Call == nullptr || Call->Runtime == nullptr)
	{
		OutResult = FAvidScriptDynamicHostCallResult();
		OutResult.Details =
			TEXT("prepared_dynamic_context_invalid: the Runtime call context is unavailable.");
		return false;
	}
	return Call->Runtime->DispatchPreparedDynamicHost(
		*Call,
		Arguments,
		GuestMemory,
		OutResult);
}

bool FAvidScriptWasmRuntimeInstance::DispatchPreparedDynamicHost(
	FAvidScriptPreparedDynamicHostCall& Call,
	const TConstArrayView<uint64> Arguments,
	IAvidScriptVmGuestMemory& GuestMemory,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	bool bPreparedSucceeded = false;
	ON_SCOPE_EXIT
	{
		FAvidScriptBindingInvocationInstrumentation* Instrumentation =
			BindingInvocationContext.InvocationInstrumentation;
		if (Instrumentation != nullptr)
		{
			if (bPreparedSucceeded)
			{
				++Instrumentation->PreparedDynamicHitCount;
			}
			else
			{
				++Instrumentation->PreparedDynamicRejectCount;
			}
		}
	};
	const bool bCaptureTiming =
		HostContext.DynamicHostCallTimingPolicy
		== EAvidScriptDynamicHostCallTimingPolicy::PerCall;
	const double HostImportStartSeconds =
		bCaptureTiming ? FPlatformTime::Seconds() : 0.0;
	ON_SCOPE_EXIT
	{
		if (bCaptureTiming)
		{
			Metrics.HostImportCallMs += MeasureElapsedMs(HostImportStartSeconds);
			++Metrics.TimedDynamicHostCallCount;
		}
	};

	++HostImportCallCount;
	LastHostImportInput = static_cast<int32>(Call.Binding.BindingOrdinal);
	LastHostImportResult = 0;
	OutResult = FAvidScriptDynamicHostCallResult();
	if (!BindingPackage.IsValid()
		|| Call.Package.Get() != BindingPackage.Get()
		|| Call.Binding.ImmutableInvocationCell == nullptr
		|| Call.Binding.ExpectedClass == nullptr
		|| Call.Binding.Invoke == nullptr)
	{
		OutResult.Details =
			TEXT("prepared_dynamic_context_stale: the prepared binding package is no longer active.");
		return false;
	}
	if (Arguments.Num() != Call.Binding.ExpectedArgumentCount
		|| BindingInvocationScratch.Num()
			< Call.Binding.RequiredScratchSize)
	{
		OutResult.Details =
			TEXT("binding_frame_mismatch: arguments, guest memory, or scratch do not match the prepared call.");
		return false;
	}

	UObject* Receiver = nullptr;
	if (Call.Binding.bStatic)
	{
		Receiver = Call.Binding.ExpectedClass->GetDefaultObject();
	}
	else
	{
		if (Arguments.Num() < 2
			|| Arguments[0] > MAX_uint32
			|| Arguments[1] > MAX_uint32)
		{
			OutResult.Details =
				TEXT("binding_target_invalid: the prepared receiver handle is outside the 32-bit ABI.");
			return false;
		}
		const uint32 Slot = static_cast<uint32>(Arguments[0]);
		const uint32 Generation = static_cast<uint32>(Arguments[1]);
		const bool bCanUseSelfCache =
			Slot <= static_cast<uint32>(MAX_int32)
			&& Generation <= static_cast<uint32>(MAX_int32)
			&& HostContext.OwnerHandle.Slot == Slot
			&& HostContext.OwnerHandle.Generation == Generation;
		if (!bCanUseSelfCache
			|| !ResolveSelfCapability(
				static_cast<int32>(Slot),
				static_cast<int32>(Generation),
				Call.Binding.ExpectedClass,
				Receiver))
		{
			if (HostContext.ObjectRegistry == nullptr
				|| Slot == 0
				|| Generation == 0)
			{
				OutResult.Details =
					TEXT("binding_target_invalid: no object registry is available for the prepared receiver.");
				return false;
			}
			FAvidScriptObjectHandleResult ResolveResult;
			Receiver = HostContext.ObjectRegistry->ResolveObject(
				{ Slot, Generation },
				ResolveResult,
				false);
			if (Receiver == nullptr
				|| !Receiver->IsA(Call.Binding.ExpectedClass))
			{
				OutResult.Details = ResolveResult.ErrorMessage.IsEmpty()
					? FString(TEXT("binding_target_invalid: the prepared receiver does not match its owner class."))
					: MoveTemp(ResolveResult.ErrorMessage);
				return false;
			}
		}
	}
	if (Receiver == nullptr)
	{
		OutResult.Details =
			TEXT("binding_target_invalid: the prepared receiver is null.");
		return false;
	}

	const bool bSucceeded = Call.Binding.Invoke(
		Call.Binding.ImmutableInvocationCell,
		*Receiver,
		Arguments,
		&GuestMemory,
		BindingInvocationContext,
		BindingInvocationScratch,
		OutResult);
	bPreparedSucceeded = bSucceeded && OutResult.bSucceeded;
	LastHostImportResult = OutResult.ReturnValue;
	return bPreparedSucceeded;
}

bool FAvidScriptWasmRuntimeInstance::DispatchDynamicHostCall(
	const FAvidScriptDynamicHostCall& Call,
	FAvidScriptDynamicHostCallResult& OutResult)
{
	if (BindingInvocationContext.InvocationInstrumentation != nullptr)
	{
		++BindingInvocationContext.InvocationInstrumentation
			->PreparedDynamicFallbackCount;
	}
	const bool bCaptureTiming =
		HostContext.DynamicHostCallTimingPolicy
		== EAvidScriptDynamicHostCallTimingPolicy::PerCall;
	const double HostImportStartSeconds =
		bCaptureTiming ? FPlatformTime::Seconds() : 0.0;
	const auto RecordTiming = [this, bCaptureTiming, HostImportStartSeconds]()
	{
		if (bCaptureTiming)
		{
			Metrics.HostImportCallMs += MeasureElapsedMs(HostImportStartSeconds);
			++Metrics.TimedDynamicHostCallCount;
		}
	};
	++HostImportCallCount;
	LastHostImportInput = static_cast<int32>(Call.BindingOrdinal);
	LastHostImportResult = 0;
	if (!BindingPackage.IsValid())
	{
		OutResult = FAvidScriptDynamicHostCallResult();
		OutResult.Details = TEXT("No reflected binding package is attached to this Runtime instance.");
		RecordTiming();
		return false;
	}

	const bool bSucceeded = BindingPackage->Dispatch(
		Call,
		BindingInvocationContext,
		BindingInvocationScratch,
		OutResult);
	LastHostImportResult = OutResult.ReturnValue;
	RecordTiming();
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
	auto FinishI64 = [&Finish, &OutResult](int64 ReturnValue, bool bSucceeded)
	{
		OutResult.ReturnValueI64 = ReturnValue;
		return Finish(static_cast<int32>(ReturnValue), bSucceeded);
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
	case EAvidScriptHostBindingId::OwnerGetHandle:
	{
		const int64 Value = HandleOwnerGetHandleImport();
		return FinishI64(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::DataLaneGetEpoch:
	{
		const int64 Value = HandleDataLaneGetEpochImport();
		return FinishI64(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::DataLaneSubmit:
	{
		const int32 Value = HandleDataLaneSubmitImport(Call.InputBytes);
		return Finish(Value, Value > 0);
	}
	case EAvidScriptHostBindingId::ValueArrayLength:
	{
		const int32 Value = HandleValueArrayLengthImport(Call.IntArgs[0]);
		return Finish(Value, !bHasPendingHostImportFailure);
	}
	case EAvidScriptHostBindingId::ValueArrayLoad:
	{
		const int32 Value = HandleValueArrayLoadImport(
			Call.IntArgs[0],
			Call.IntArgs[1],
			Call.OutputBytes);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ValueArrayStore:
	{
		const int32 Value = HandleValueArrayStoreImport(
			Call.IntArgs[0],
			Call.IntArgs[1],
			Call.InputBytes);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ValueArrayReadRange:
	case EAvidScriptHostBindingId::ValueArrayWriteRange:
	{
		const int32 Value = HandleValueArrayRangeImport(
			Call.BindingId == EAvidScriptHostBindingId::ValueArrayReadRange,
			Call.IntArgs[0],
			Call.IntArgs[1],
			Call.GuestAddress,
			Call.IntArgs[2],
			Call.IntArgs[3]);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ValueRelease:
	{
		const int32 Value = HandleValueReleaseImport(Call.IntArgs[0]);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ValueTextToString:
	{
		const int32 Value = HandleValueTextToStringImport(Call.IntArgs[0]);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ValueContainerCount:
	{
		const int32 Value = HandleValueContainerCountImport(Call.IntArgs[0]);
		return Finish(Value, !bHasPendingHostImportFailure);
	}
	case EAvidScriptHostBindingId::ValueContainerRead:
	case EAvidScriptHostBindingId::ValueContainerWrite:
	{
		const int32 Value = HandleValueContainerAccessImport(
			Call.BindingId == EAvidScriptHostBindingId::ValueContainerRead,
			Call.IntArgs[0],
			Call.IntArgs[1],
			Call.IntArgs[2],
			Call.GuestAddress);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ValueContainerResize:
	{
		const int32 Value = HandleValueContainerResizeImport(
			Call.IntArgs[0],
			Call.IntArgs[1]);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ValueContainerClear:
	{
		const int32 Value = HandleValueContainerClearImport(Call.IntArgs[0]);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ValueContainerFind:
	{
		const int32 Value = HandleValueContainerFindImport(
			Call.IntArgs[0],
			Call.GuestAddress);
		return Finish(Value, !bHasPendingHostImportFailure);
	}
	case EAvidScriptHostBindingId::ValueContainerUpsert:
	{
		const int32 Value = HandleValueContainerUpsertImport(
			Call.IntArgs[0],
			static_cast<uint32>(Call.IntArgs[1]),
			Call.GuestAddress);
		return Finish(Value, !bHasPendingHostImportFailure);
	}
	case EAvidScriptHostBindingId::ValueContainerRemove:
	{
		const int32 Value = HandleValueContainerRemoveImport(
			Call.IntArgs[0],
			Call.GuestAddress);
		return Finish(Value, !bHasPendingHostImportFailure);
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
	case EAvidScriptHostBindingId::ContinuationDelay:
	{
		const int64 Value = HandleContinuationDelayImport(
			Call.FloatArgs[0],
			Call.IntArgs[0]);
		return FinishI64(Value, true);
	}
	case EAvidScriptHostBindingId::ContinuationCancel:
	{
		const int32 Value = HandleContinuationCancelImport(Call.Int64Args[0]);
		return Finish(Value, true);
	}
	case EAvidScriptHostBindingId::ContinuationLoadObject:
	{
		const int64 Value = HandleContinuationLoadObjectImport(
			Call.IntArgs[0],
			Call.IntArgs[1]);
		return FinishI64(Value, !bHasPendingHostImportFailure);
	}
	case EAvidScriptHostBindingId::ContinuationCancelSourceCreate:
	{
		const int64 Value = HandleContinuationCancelSourceCreateImport();
		return FinishI64(Value, true);
	}
	case EAvidScriptHostBindingId::ContinuationCancelSourceCancel:
	{
		const int32 Value = HandleContinuationCancelSourceCancelImport(
			Call.Int64Args[0]);
		return Finish(Value, true);
	}
	case EAvidScriptHostBindingId::ContinuationCancelSourceRelease:
	{
		const int32 Value = HandleContinuationCancelSourceReleaseImport(
			Call.Int64Args[0]);
		return Finish(Value, true);
	}
	case EAvidScriptHostBindingId::ContinuationBindCancel:
	{
		const int32 Value = HandleContinuationBindCancelImport(
			Call.Int64Args[0],
			Call.Int64Args[1]);
		return Finish(Value, true);
	}
	case EAvidScriptHostBindingId::ContinuationResultRead:
	{
		const int32 Value = HandleContinuationResultReadImport(
			Call.IntArgs[0],
			Call.IntArgs[1],
			Call.IntArgs[2],
			Call.OutputBytes);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::ContinuationStateStore:
	{
		const int32 Value = HandleContinuationStateStoreImport(
			Call.Int64Args[0],
			Call.InputBytes);
		return Finish(Value, true);
	}
	case EAvidScriptHostBindingId::ContinuationStateRead:
	{
		const int32 Value = HandleContinuationStateReadImport(
			Call.Int64Args[0],
			Call.OutputBytes);
		return Finish(Value, Value != 0);
	}
	case EAvidScriptHostBindingId::EventSubscribe:
	{
		const int64 Value = HandleEventSubscribeImport(
			Call.IntArgs[0],
			Call.IntArgs[1],
			Call.IntArgs[2]);
		return FinishI64(Value, !bHasPendingHostImportFailure);
	}
	case EAvidScriptHostBindingId::EventUnsubscribe:
	{
		const int32 Value = HandleEventUnsubscribeImport(Call.Int64Args[0]);
		return Finish(Value, true);
	}
	case EAvidScriptHostBindingId::DelegateOutputWrite:
	{
		const int32 Value = HandleDelegateOutputWriteImport(
			Call.IntArgs[0],
			Call.IntArgs[1],
			Call.GuestAddress);
		return Finish(Value, !bHasPendingHostImportFailure);
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
	ActiveDelegateOutputTransaction = nullptr;
	ActiveDelegateOutputToken = 0;
}

void FAvidScriptWasmRuntimeInstance::CopyHostImportStateToResult(FAvidScriptWasmSmokeResult& OutResult) const
{
	OutResult.HostImportCallCount = HostImportCallCount;
	OutResult.LastHostImportInput = LastHostImportInput;
	OutResult.LastHostImportResult = LastHostImportResult;
	OutResult.Metrics = Metrics;
	OutResult.DataBridgeMetrics = DataBridgeMetrics;
	OutResult.BindingInstrumentation =
		HostContext.BindingInvocationInstrumentation == nullptr
			? FAvidScriptBindingInvocationInstrumentation()
			: *HostContext.BindingInvocationInstrumentation;
}

void FAvidScriptWasmRuntimeInstance::CaptureSnapshot(
	FAvidScriptWasmSmokeResult& OutResult) const
{
	PrepareResult(OutResult, ModuleId, ActiveBackendInfo, Metrics);
	OutResult.bRuntimeInitialized = IsLoaded();
	OutResult.bModuleLoaded = IsLoaded();
	OutResult.bModuleInstantiated = IsLoaded();
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	OutResult.TickCallCount = TickCallCount;
	CopyObservableStateToResult(OutResult);
}

FAvidScriptWasmHotSnapshot
FAvidScriptWasmRuntimeInstance::GetHotSnapshot() const
{
	FAvidScriptWasmHotSnapshot Snapshot;
	Snapshot.bRuntimeLoaded = IsLoaded();
	Snapshot.bBeginPlayCalled = bHasBegunPlay;
	Snapshot.bEndPlayCalled = bHasEndedPlay;
	Snapshot.TickCallCount = TickCallCount;
	Snapshot.TimerCallbackCount = TimerCallbackCount;
	Snapshot.LastTimerCallbackId = LastTimerCallbackId;
	Snapshot.LastTimerHandle = LastTimerHandle;
	Snapshot.EventCallbackCount = EventCallbackCount;
	Snapshot.LastEventId = LastEventId;
	Snapshot.LastEventValue = LastEventValue;
	Snapshot.Metrics = Metrics;
	return Snapshot;
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
