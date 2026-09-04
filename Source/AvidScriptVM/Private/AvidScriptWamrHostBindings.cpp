#include "AvidScriptWamrHostBindings.h"
#include "AvidScriptVmStaticHostImports.h"

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
#if AVIDSCRIPT_WITH_WAMR
constexpr const char* CanonicalModuleName = "avidscript";
constexpr const char* CompatibilityModuleName = "env";
constexpr int32 MaxTransformBatchCount = 256;
constexpr uint32 TransformBatchInputCellsPerItem = 2;
constexpr uint32 TransformBatchOutputFloatsPerItem = 9;
constexpr int32 MaxContinuationResultBytes = 4096;
constexpr int32 MaxContinuationStateBytes = 4096;
constexpr int32 MaxDebugFrameBytes = 4096;

const char* StaticImportName(EAvidScriptHostBindingId BindingId)
{
	return GetAvidScriptVmStaticHostImport(BindingId).ImportName;
}

IAvidScriptWamrHostBridge* GetBridge(wasm_exec_env_t ExecEnv)
{
	return ExecEnv != nullptr
		? static_cast<IAvidScriptWamrHostBridge*>(wasm_runtime_get_user_data(ExecEnv))
		: nullptr;
}

void SetException(wasm_exec_env_t ExecEnv, const char* Details)
{
	if (ExecEnv == nullptr)
	{
		return;
	}

	if (wasm_module_inst_t ModuleInstance = wasm_runtime_get_module_inst(ExecEnv))
	{
		wasm_runtime_set_exception(ModuleInstance, Details);
	}
}

bool Fail(
	wasm_exec_env_t ExecEnv,
	IAvidScriptWamrHostBridge* Bridge,
	const char* ImportName,
	const FString& Details,
	const FString& Category = TEXT("host_import_failed"))
{
	if (Bridge != nullptr)
	{
		Bridge->RecordHostImportFailure(ImportName, Category, Details);
	}
	SetException(ExecEnv, "avidscript_host_import_failed");
	return false;
}

bool Dispatch(
	wasm_exec_env_t ExecEnv,
	const char* ImportName,
	FAvidScriptHostCall& Call,
	FAvidScriptHostCallResult& OutResult)
{
	IAvidScriptWamrHostBridge* Bridge = GetBridge(ExecEnv);
	if (Bridge == nullptr)
	{
		return Fail(ExecEnv, nullptr, ImportName, TEXT("The VM host bridge is unavailable."));
	}

	if (!Bridge->DispatchHostCall(Call, OutResult) || !OutResult.bSucceeded)
	{
		const FString Details = OutResult.Details.IsEmpty()
			? FString::Printf(TEXT("Host dispatcher rejected avidscript.%s."), ANSI_TO_TCHAR(ImportName))
			: OutResult.Details;
		return Fail(
			ExecEnv,
			Bridge,
			ImportName,
			Details,
			OutResult.ErrorCategory.IsEmpty()
				? FString(TEXT("host_import_failed"))
				: OutResult.ErrorCategory);
	}

	return true;
}

bool TranslateGuestRange(
	wasm_exec_env_t ExecEnv,
	const char* ImportName,
	const TCHAR* ArgumentName,
	int32 GuestAddress,
	uint32 ElementCount,
	uint32 ElementSize,
	uint32 Alignment,
	void*& OutNativeAddress)
{
	OutNativeAddress = nullptr;
	if (ElementCount == 0)
	{
		return true;
	}

	IAvidScriptWamrHostBridge* Bridge = GetBridge(ExecEnv);
	wasm_module_inst_t ModuleInstance = ExecEnv != nullptr ? wasm_runtime_get_module_inst(ExecEnv) : nullptr;
	const uint64 ByteCount64 = static_cast<uint64>(ElementCount) * ElementSize;
	if (ModuleInstance == nullptr || GuestAddress <= 0 || Alignment == 0 ||
		(static_cast<uint32>(GuestAddress) % Alignment) != 0 || ByteCount64 > MAX_uint32 ||
		!wasm_runtime_validate_app_addr(
			ModuleInstance,
			static_cast<uint64>(static_cast<uint32>(GuestAddress)),
			static_cast<uint64>(ByteCount64)))
	{
		return Fail(
			ExecEnv,
			Bridge,
			ImportName,
			FString::Printf(
				TEXT("Invalid %s range at %d for avidscript.%s."),
				ArgumentName,
				GuestAddress,
				ANSI_TO_TCHAR(ImportName)));
	}

	OutNativeAddress = wasm_runtime_addr_app_to_native(
		ModuleInstance,
		static_cast<uint64>(static_cast<uint32>(GuestAddress)));
	if (OutNativeAddress == nullptr)
	{
		return Fail(
			ExecEnv,
			Bridge,
			ImportName,
			FString::Printf(
				TEXT("Failed to translate %s address %d for avidscript.%s."),
				ArgumentName,
				GuestAddress,
				ANSI_TO_TCHAR(ImportName)));
	}
	return true;
}

bool WriteGuestBytes(
	wasm_exec_env_t ExecEnv,
	const char* ImportName,
	int32 GuestAddress,
	const void* Source,
	uint32 ByteCount)
{
	IAvidScriptWamrHostBridge* Bridge = GetBridge(ExecEnv);
	wasm_module_inst_t ModuleInstance = ExecEnv != nullptr ? wasm_runtime_get_module_inst(ExecEnv) : nullptr;
	if (ModuleInstance == nullptr || GuestAddress <= 0 ||
		!wasm_runtime_validate_app_addr(ModuleInstance, static_cast<uint64>(GuestAddress), ByteCount))
	{
		return Fail(
			ExecEnv,
			Bridge,
			ImportName,
			FString::Printf(TEXT("Invalid output pointer %d for avidscript.%s."), GuestAddress, ANSI_TO_TCHAR(ImportName)));
	}

	void* Destination = wasm_runtime_addr_app_to_native(ModuleInstance, static_cast<uint64>(GuestAddress));
	if (Destination == nullptr)
	{
		return Fail(
			ExecEnv,
			Bridge,
			ImportName,
			FString::Printf(TEXT("Failed to translate output pointer %d for avidscript.%s."), GuestAddress, ANSI_TO_TCHAR(ImportName)));
	}

	FMemory::Memcpy(Destination, Source, ByteCount);
	return true;
}

int32_t DispatchI32(wasm_exec_env_t ExecEnv, EAvidScriptHostBindingId BindingId, const char* ImportName, int32 Arg)
{
	FAvidScriptHostCall Call;
	Call.BindingId = BindingId;
	Call.IntArgs[0] = Arg;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result) ? Result.ReturnValue : 0;
}

int32_t DispatchActorVectorWrite(
	wasm_exec_env_t ExecEnv,
	EAvidScriptHostBindingId BindingId,
	const char* ImportName,
	int32 Slot,
	int32 Generation,
	float X,
	float Y,
	float Z)
{
	FAvidScriptHostCall Call;
	Call.BindingId = BindingId;
	Call.IntArgs[0] = Slot;
	Call.IntArgs[1] = Generation;
	Call.FloatArgs[0] = X;
	Call.FloatArgs[1] = Y;
	Call.FloatArgs[2] = Z;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result) ? Result.ReturnValue : 0;
}

int32_t DispatchVectorRead(
	wasm_exec_env_t ExecEnv,
	EAvidScriptHostBindingId BindingId,
	const char* ImportName,
	int32 Slot,
	int32 Generation,
	int32 OutAddress)
{
	FAvidScriptHostCall Call;
	Call.BindingId = BindingId;
	Call.IntArgs[0] = Slot;
	Call.IntArgs[1] = Generation;
	Call.GuestAddress = static_cast<uint32>(OutAddress);
	FAvidScriptHostCallResult Result;
	if (!Dispatch(ExecEnv, ImportName, Call, Result))
	{
		return 0;
	}

	return WriteGuestBytes(ExecEnv, ImportName, OutAddress, Result.FloatValues, sizeof(Result.FloatValues))
		? Result.ReturnValue
		: 0;
}

int32_t HostAddI32(wasm_exec_env_t ExecEnv, int32_t Input)
{
	return DispatchI32(ExecEnv, EAvidScriptHostBindingId::HostAddI32, StaticImportName(EAvidScriptHostBindingId::HostAddI32), Input);
}

int32_t HostFailI32(wasm_exec_env_t ExecEnv, int32_t Input)
{
	return DispatchI32(ExecEnv, EAvidScriptHostBindingId::HostFailI32, StaticImportName(EAvidScriptHostBindingId::HostFailI32), Input);
}

int32_t ActorGetLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutAddress)
{
	return DispatchVectorRead(ExecEnv, EAvidScriptHostBindingId::ActorGetLocation, StaticImportName(EAvidScriptHostBindingId::ActorGetLocation), Slot, Generation, OutAddress);
}

int32_t ActorSetLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::ActorSetLocation, StaticImportName(EAvidScriptHostBindingId::ActorSetLocation), Slot, Generation, X, Y, Z);
}

int32_t ActorAddLocationOffset(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::ActorAddLocationOffset, StaticImportName(EAvidScriptHostBindingId::ActorAddLocationOffset), Slot, Generation, X, Y, Z);
}

int32_t ActorGetRotation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutAddress)
{
	return DispatchVectorRead(ExecEnv, EAvidScriptHostBindingId::ActorGetRotation, StaticImportName(EAvidScriptHostBindingId::ActorGetRotation), Slot, Generation, OutAddress);
}

int32_t ActorSetRotation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float Pitch, float Yaw, float Roll)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::ActorSetRotation, StaticImportName(EAvidScriptHostBindingId::ActorSetRotation), Slot, Generation, Pitch, Yaw, Roll);
}

int32_t ActorGetScale(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutAddress)
{
	return DispatchVectorRead(ExecEnv, EAvidScriptHostBindingId::ActorGetScale, StaticImportName(EAvidScriptHostBindingId::ActorGetScale), Slot, Generation, OutAddress);
}

int32_t ActorSetScale(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::ActorSetScale, StaticImportName(EAvidScriptHostBindingId::ActorSetScale), Slot, Generation, X, Y, Z);
}

int32_t ActorGetTransformBatch(
	wasm_exec_env_t ExecEnv,
	int32_t InputAddress,
	int32_t Count,
	int32_t OutputAddress)
{
	const char* ImportName = StaticImportName(EAvidScriptHostBindingId::ActorGetTransformBatch);
	IAvidScriptWamrHostBridge* Bridge = GetBridge(ExecEnv);
	if (Count < 0 || Count > MaxTransformBatchCount)
	{
		Fail(
			ExecEnv,
			Bridge,
			ImportName,
			FString::Printf(TEXT("Batch count %d is outside the supported range 0..%d."), Count, MaxTransformBatchCount));
		return 0;
	}

	const uint32 ItemCount = static_cast<uint32>(Count);
	const uint32 InputCellCount = ItemCount * TransformBatchInputCellsPerItem;
	const uint32 OutputFloatCount = ItemCount * TransformBatchOutputFloatsPerItem;
	void* InputNativeAddress = nullptr;
	void* OutputNativeAddress = nullptr;
	if (!TranslateGuestRange(
			ExecEnv,
			ImportName,
			TEXT("input"),
			InputAddress,
			InputCellCount,
			sizeof(uint32),
			alignof(uint32),
			InputNativeAddress) ||
		!TranslateGuestRange(
			ExecEnv,
			ImportName,
			TEXT("output"),
			OutputAddress,
			OutputFloatCount,
			sizeof(float),
			alignof(float),
			OutputNativeAddress))
	{
		return 0;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ActorGetTransformBatch;
	Call.IntArgs[0] = Count;
	if (InputCellCount > 0)
	{
		Call.InputCells = MakeArrayView(static_cast<const uint32*>(InputNativeAddress), static_cast<int32>(InputCellCount));
		Call.OutputFloats = MakeArrayView(static_cast<float*>(OutputNativeAddress), static_cast<int32>(OutputFloatCount));
	}
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result) ? Result.ReturnValue : 0;
}

int32_t ActorGetRootComponent(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutAddress)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ActorGetRootComponent;
	Call.IntArgs[0] = Slot;
	Call.IntArgs[1] = Generation;
	Call.GuestAddress = static_cast<uint32>(OutAddress);
	FAvidScriptHostCallResult Result;
	const char* ImportName = StaticImportName(EAvidScriptHostBindingId::ActorGetRootComponent);
	if (!Dispatch(ExecEnv, ImportName, Call, Result))
	{
		return 0;
	}

	const uint32 HandleValues[2] = {
		Result.IntValues[0],
		Result.IntValues[1]
	};
	return WriteGuestBytes(ExecEnv, ImportName, OutAddress, HandleValues, sizeof(HandleValues))
		? Result.ReturnValue
		: 0;
}

int32_t SceneComponentGetWorldLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutAddress)
{
	return DispatchVectorRead(ExecEnv, EAvidScriptHostBindingId::SceneComponentGetWorldLocation, StaticImportName(EAvidScriptHostBindingId::SceneComponentGetWorldLocation), Slot, Generation, OutAddress);
}

int32_t SceneComponentSetWorldLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::SceneComponentSetWorldLocation, StaticImportName(EAvidScriptHostBindingId::SceneComponentSetWorldLocation), Slot, Generation, X, Y, Z);
}

int32_t OwnerGetSlot(wasm_exec_env_t ExecEnv)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::OwnerGetSlot;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, StaticImportName(EAvidScriptHostBindingId::OwnerGetSlot), Call, Result) ? Result.ReturnValue : 0;
}

int32_t OwnerGetGeneration(wasm_exec_env_t ExecEnv)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::OwnerGetGeneration;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, StaticImportName(EAvidScriptHostBindingId::OwnerGetGeneration), Call, Result) ? Result.ReturnValue : 0;
}

int64_t OwnerGetHandle(wasm_exec_env_t ExecEnv)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::OwnerGetHandle;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, StaticImportName(EAvidScriptHostBindingId::OwnerGetHandle), Call, Result) ? Result.ReturnValueI64 : 0;
}

int64_t DataLaneGetEpoch(wasm_exec_env_t ExecEnv)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::DataLaneGetEpoch;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, StaticImportName(EAvidScriptHostBindingId::DataLaneGetEpoch), Call, Result) ? Result.ReturnValueI64 : 0;
}

int32_t DataLaneSubmit(wasm_exec_env_t ExecEnv, int32_t GuestAddress, int32_t ByteCount)
{
	const char* ImportName = StaticImportName(EAvidScriptHostBindingId::DataLaneSubmit);
	if (ByteCount <= 0)
	{
		Fail(
			ExecEnv,
			GetBridge(ExecEnv),
			ImportName,
			TEXT("The command buffer byte count must be positive."));
		return 0;
	}

	void* NativeAddress = nullptr;
	if (!TranslateGuestRange(
			ExecEnv,
			ImportName,
			TEXT("command buffer"),
			GuestAddress,
			static_cast<uint32>(ByteCount),
			1,
			alignof(uint32),
			NativeAddress))
	{
		return 0;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::DataLaneSubmit;
	Call.GuestAddress = static_cast<uint32>(GuestAddress);
	Call.IntArgs[0] = ByteCount;
	Call.InputBytes = MakeArrayView(
		static_cast<const uint8*>(NativeAddress),
		ByteCount);
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result) ? Result.ReturnValue : 0;
}

int32_t ValueArrayLength(wasm_exec_env_t ExecEnv, int32_t Token)
{
	return DispatchI32(
		ExecEnv,
		EAvidScriptHostBindingId::ValueArrayLength,
		StaticImportName(EAvidScriptHostBindingId::ValueArrayLength),
		Token);
}

int32_t DispatchValueArrayAccess(
	wasm_exec_env_t ExecEnv,
	const EAvidScriptHostBindingId BindingId,
	const int32_t Token,
	const int32_t ElementIndex,
	const int32_t GuestAddress,
	const int32_t ByteCount)
{
	const char* ImportName = StaticImportName(BindingId);
	if (ElementIndex < 0 || ByteCount <= 0 || ByteCount > 4096)
	{
		Fail(
			ExecEnv,
			GetBridge(ExecEnv),
			ImportName,
			TEXT("The array element index or byte count is outside the supported range."));
		return 0;
	}

	void* NativeAddress = nullptr;
	if (!TranslateGuestRange(
			ExecEnv,
			ImportName,
			TEXT("array element"),
			GuestAddress,
			static_cast<uint32>(ByteCount),
			1,
			1,
			NativeAddress))
	{
		return 0;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = BindingId;
	Call.GuestAddress = static_cast<uint32>(GuestAddress);
	Call.IntArgs[0] = Token;
	Call.IntArgs[1] = ElementIndex;
	Call.IntArgs[2] = ByteCount;
	if (BindingId == EAvidScriptHostBindingId::ValueArrayLoad)
	{
		Call.OutputBytes = MakeArrayView(
			static_cast<uint8*>(NativeAddress),
			ByteCount);
	}
	else
	{
		Call.InputBytes = MakeArrayView(
			static_cast<const uint8*>(NativeAddress),
			ByteCount);
	}
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result)
		? Result.ReturnValue
		: 0;
}

int32_t ValueArrayLoad(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t ElementIndex,
	int32_t GuestAddress,
	int32_t ByteCount)
{
	return DispatchValueArrayAccess(
		ExecEnv,
		EAvidScriptHostBindingId::ValueArrayLoad,
		Token,
		ElementIndex,
		GuestAddress,
		ByteCount);
}

int32_t ValueArrayStore(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t ElementIndex,
	int32_t GuestAddress,
	int32_t ByteCount)
{
	return DispatchValueArrayAccess(
		ExecEnv,
		EAvidScriptHostBindingId::ValueArrayStore,
		Token,
		ElementIndex,
		GuestAddress,
		ByteCount);
}

int32_t DispatchValueArrayRange(
	wasm_exec_env_t ExecEnv,
	const EAvidScriptHostBindingId BindingId,
	const int32_t Token,
	const int32_t CapabilityIndex,
	const int32_t GuestArrayReference,
	const int32_t GuestIndex,
	const int32_t ElementCount)
{
	const char* ImportName = StaticImportName(BindingId);
	if (CapabilityIndex < 0
		|| GuestArrayReference <= 0
		|| GuestIndex < 0
		|| ElementCount < 0
		|| ElementCount > 4096)
	{
		Fail(
			ExecEnv,
			GetBridge(ExecEnv),
			ImportName,
			TEXT("An array range argument is outside the supported range."));
		return 0;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = BindingId;
	Call.IntArgs[0] = Token;
	Call.IntArgs[1] = CapabilityIndex;
	Call.GuestAddress = static_cast<uint32>(GuestArrayReference);
	Call.IntArgs[2] = GuestIndex;
	Call.IntArgs[3] = ElementCount;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result)
		? Result.ReturnValue
		: 0;
}

int32_t ValueArrayReadRange(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t CapabilityIndex,
	int32_t GuestArrayReference,
	int32_t GuestIndex,
	int32_t ElementCount)
{
	return DispatchValueArrayRange(
		ExecEnv,
		EAvidScriptHostBindingId::ValueArrayReadRange,
		Token,
		CapabilityIndex,
		GuestArrayReference,
		GuestIndex,
		ElementCount);
}

int32_t ValueArrayWriteRange(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t CapabilityIndex,
	int32_t GuestArrayReference,
	int32_t GuestIndex,
	int32_t ElementCount)
{
	return DispatchValueArrayRange(
		ExecEnv,
		EAvidScriptHostBindingId::ValueArrayWriteRange,
		Token,
		CapabilityIndex,
		GuestArrayReference,
		GuestIndex,
		ElementCount);
}

int32_t ValueRelease(wasm_exec_env_t ExecEnv, int32_t Token)
{
	return DispatchI32(
		ExecEnv,
		EAvidScriptHostBindingId::ValueRelease,
		StaticImportName(EAvidScriptHostBindingId::ValueRelease),
		Token);
}

int32_t ValueTextToString(wasm_exec_env_t ExecEnv, int32_t Token)
{
	return DispatchI32(
		ExecEnv,
		EAvidScriptHostBindingId::ValueTextToString,
		StaticImportName(EAvidScriptHostBindingId::ValueTextToString),
		Token);
}

int32_t ValueContainerCount(wasm_exec_env_t ExecEnv, int32_t Token)
{
	return DispatchI32(
		ExecEnv,
		EAvidScriptHostBindingId::ValueContainerCount,
		StaticImportName(EAvidScriptHostBindingId::ValueContainerCount),
		Token);
}

int32_t DispatchValueContainerAccess(
	wasm_exec_env_t ExecEnv,
	const EAvidScriptHostBindingId BindingId,
	const int32_t Token,
	const int32_t Index,
	const int32_t Lane,
	const int32_t GuestAddress)
{
	FAvidScriptHostCall Call;
	Call.BindingId = BindingId;
	Call.IntArgs[0] = Token;
	Call.IntArgs[1] = Index;
	Call.IntArgs[2] = Lane;
	Call.GuestAddress = static_cast<uint32>(GuestAddress);
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, StaticImportName(BindingId), Call, Result)
		? Result.ReturnValue
		: 0;
}

int32_t ValueContainerRead(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t Index,
	int32_t Lane,
	int32_t GuestAddress)
{
	return DispatchValueContainerAccess(
		ExecEnv,
		EAvidScriptHostBindingId::ValueContainerRead,
		Token,
		Index,
		Lane,
		GuestAddress);
}

int32_t ValueContainerWrite(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t Index,
	int32_t Lane,
	int32_t GuestAddress)
{
	return DispatchValueContainerAccess(
		ExecEnv,
		EAvidScriptHostBindingId::ValueContainerWrite,
		Token,
		Index,
		Lane,
		GuestAddress);
}

int32_t ValueContainerResize(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t NewCount)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ValueContainerResize;
	Call.IntArgs[0] = Token;
	Call.IntArgs[1] = NewCount;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ValueContainerResize),
		Call,
		Result)
		? Result.ReturnValue
		: 0;
}

int32_t ValueContainerClear(wasm_exec_env_t ExecEnv, int32_t Token)
{
	return DispatchI32(
		ExecEnv,
		EAvidScriptHostBindingId::ValueContainerClear,
		StaticImportName(EAvidScriptHostBindingId::ValueContainerClear),
		Token);
}

int32_t ValueContainerFind(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t GuestAddress)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ValueContainerFind;
	Call.IntArgs[0] = Token;
	Call.GuestAddress = static_cast<uint32>(GuestAddress);
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ValueContainerFind),
		Call,
		Result)
		? Result.ReturnValue
		: -1;
}

int32_t ValueContainerUpsert(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t KeyAddress,
	int32_t ValueAddress)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ValueContainerUpsert;
	Call.IntArgs[0] = Token;
	Call.IntArgs[1] = KeyAddress;
	Call.GuestAddress = static_cast<uint32>(ValueAddress);
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ValueContainerUpsert),
		Call,
		Result)
		? Result.ReturnValue
		: -1;
}

int32_t ValueContainerRemove(
	wasm_exec_env_t ExecEnv,
	int32_t Token,
	int32_t KeyAddress)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ValueContainerRemove;
	Call.IntArgs[0] = Token;
	Call.GuestAddress = static_cast<uint32>(KeyAddress);
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ValueContainerRemove),
		Call,
		Result)
		? Result.ReturnValue
		: -1;
}

int32_t TimerSetOnce(wasm_exec_env_t ExecEnv, float DelaySeconds, int32_t CallbackId)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::TimerSetOnce;
	Call.FloatArgs[0] = DelaySeconds;
	Call.IntArgs[0] = CallbackId;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, StaticImportName(EAvidScriptHostBindingId::TimerSetOnce), Call, Result) ? Result.ReturnValue : 0;
}

int32_t TimerCancel(wasm_exec_env_t ExecEnv, int32_t TimerHandle)
{
	return DispatchI32(ExecEnv, EAvidScriptHostBindingId::TimerCancel, StaticImportName(EAvidScriptHostBindingId::TimerCancel), TimerHandle);
}

int64_t ContinuationDelay(wasm_exec_env_t ExecEnv, float DelaySeconds, int32_t CallbackId)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationDelay;
	Call.FloatArgs[0] = DelaySeconds;
	Call.IntArgs[0] = CallbackId;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ContinuationDelay),
		Call,
		Result)
		? Result.ReturnValueI64
		: 0;
}

int32_t ContinuationCancel(wasm_exec_env_t ExecEnv, int64_t ContinuationToken)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationCancel;
	Call.Int64Args[0] = ContinuationToken;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ContinuationCancel),
		Call,
		Result)
		? Result.ReturnValue
		: 0;
}

int64_t ContinuationLoadObject(wasm_exec_env_t ExecEnv, int32_t ObjectPathId, int32_t CallbackId)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationLoadObject;
	Call.IntArgs[0] = ObjectPathId;
	Call.IntArgs[1] = CallbackId;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ContinuationLoadObject),
		Call,
		Result)
		? Result.ReturnValueI64
		: 0;
}

int64_t ContinuationCancelSourceCreate(wasm_exec_env_t ExecEnv)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationCancelSourceCreate;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ContinuationCancelSourceCreate),
		Call,
		Result)
		? Result.ReturnValueI64
		: 0;
}

int32_t ContinuationCancelSourceCancel(
	wasm_exec_env_t ExecEnv,
	int64_t SourceToken)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationCancelSourceCancel;
	Call.Int64Args[0] = SourceToken;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ContinuationCancelSourceCancel),
		Call,
		Result)
		? Result.ReturnValue
		: 0;
}

int32_t ContinuationCancelSourceRelease(
	wasm_exec_env_t ExecEnv,
	int64_t SourceToken)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationCancelSourceRelease;
	Call.Int64Args[0] = SourceToken;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ContinuationCancelSourceRelease),
		Call,
		Result)
		? Result.ReturnValue
		: 0;
}

int32_t ContinuationBindCancel(
	wasm_exec_env_t ExecEnv,
	int64_t SourceToken,
	int64_t ContinuationToken)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationBindCancel;
	Call.Int64Args[0] = SourceToken;
	Call.Int64Args[1] = ContinuationToken;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::ContinuationBindCancel),
		Call,
		Result)
		? Result.ReturnValue
		: 0;
}

int32_t ContinuationResultRead(
	wasm_exec_env_t ExecEnv,
	int32_t BindingOrdinal,
	int32_t ResultSlot,
	int32_t ResultGeneration,
	int32_t OutputAddress,
	int32_t ByteCount)
{
	const char* ImportName = StaticImportName(
		EAvidScriptHostBindingId::ContinuationResultRead);
	IAvidScriptWamrHostBridge* Bridge = GetBridge(ExecEnv);
	if (ByteCount <= 0 || ByteCount > MaxContinuationResultBytes)
	{
		Fail(
			ExecEnv,
			Bridge,
			ImportName,
			TEXT("The continuation result byte count is outside the supported range."));
		return 0;
	}

	void* NativeOutput = nullptr;
	if (!TranslateGuestRange(
			ExecEnv,
			ImportName,
			TEXT("continuation result output"),
			OutputAddress,
			static_cast<uint32>(ByteCount),
			1,
			1,
			NativeOutput))
	{
		return 0;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationResultRead;
	Call.IntArgs[0] = BindingOrdinal;
	Call.IntArgs[1] = ResultSlot;
	Call.IntArgs[2] = ResultGeneration;
	Call.IntArgs[3] = ByteCount;
	Call.GuestAddress = static_cast<uint32>(OutputAddress);
	Call.OutputBytes = MakeArrayView(
		static_cast<uint8*>(NativeOutput),
		ByteCount);
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result)
		? Result.ReturnValue
		: 0;
}

int32_t ContinuationStateStore(
	wasm_exec_env_t ExecEnv,
	int64_t ContinuationToken,
	int32_t InputAddress,
	int32_t ByteCount)
{
	const char* ImportName = StaticImportName(
		EAvidScriptHostBindingId::ContinuationStateStore);
	IAvidScriptWamrHostBridge* Bridge = GetBridge(ExecEnv);
	if (ByteCount <= 0
		|| ByteCount > MaxContinuationStateBytes)
	{
		Fail(
			ExecEnv,
			Bridge,
			ImportName,
			TEXT("The continuation state arguments are outside the supported range."));
		return 0;
	}

	void* NativeInput = nullptr;
	if (!TranslateGuestRange(
			ExecEnv,
			ImportName,
			TEXT("continuation state input"),
			InputAddress,
			static_cast<uint32>(ByteCount),
			1,
			1,
			NativeInput))
	{
		return 0;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationStateStore;
	Call.Int64Args[0] = ContinuationToken;
	Call.IntArgs[0] = ByteCount;
	Call.GuestAddress = static_cast<uint32>(InputAddress);
	Call.InputBytes = MakeArrayView(
		static_cast<const uint8*>(NativeInput),
		ByteCount);
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result)
		? Result.ReturnValue
		: 0;
}

int32_t ContinuationStateRead(
	wasm_exec_env_t ExecEnv,
	int64_t ContinuationToken,
	int32_t OutputAddress,
	int32_t ByteCount)
{
	const char* ImportName = StaticImportName(
		EAvidScriptHostBindingId::ContinuationStateRead);
	IAvidScriptWamrHostBridge* Bridge = GetBridge(ExecEnv);
	if (ByteCount <= 0
		|| ByteCount > MaxContinuationStateBytes)
	{
		Fail(
			ExecEnv,
			Bridge,
			ImportName,
			TEXT("The continuation state arguments are outside the supported range."));
		return 0;
	}

	void* NativeOutput = nullptr;
	if (!TranslateGuestRange(
			ExecEnv,
			ImportName,
			TEXT("continuation state output"),
			OutputAddress,
			static_cast<uint32>(ByteCount),
			1,
			1,
			NativeOutput))
	{
		return 0;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::ContinuationStateRead;
	Call.Int64Args[0] = ContinuationToken;
	Call.IntArgs[0] = ByteCount;
	Call.GuestAddress = static_cast<uint32>(OutputAddress);
	Call.OutputBytes = MakeArrayView(
		static_cast<uint8*>(NativeOutput),
		ByteCount);
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result)
		? Result.ReturnValue
		: 0;
}

int64_t EventSubscribe(
	wasm_exec_env_t ExecEnv,
	int32_t Slot,
	int32_t Generation,
	int32_t EventOrdinal)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::EventSubscribe;
	Call.IntArgs[0] = Slot;
	Call.IntArgs[1] = Generation;
	Call.IntArgs[2] = EventOrdinal;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::EventSubscribe),
		Call,
		Result)
		? Result.ReturnValueI64
		: 0;
}

int32_t EventIsCurrentSource(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::EventIsCurrentSource;
	Call.IntArgs[0] = Slot;
	Call.IntArgs[1] = Generation;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::EventIsCurrentSource),
		Call,
		Result)
		? Result.ReturnValue
		: 0;
}

int32_t EventUnsubscribe(wasm_exec_env_t ExecEnv, int64_t SubscriptionToken)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::EventUnsubscribe;
	Call.Int64Args[0] = SubscriptionToken;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::EventUnsubscribe),
		Call,
		Result)
		? Result.ReturnValue
		: 0;
}

int32_t DelegateOutputWrite(
	wasm_exec_env_t ExecEnv,
	int32_t TransactionToken,
	int32_t OutputOrdinal,
	int32_t GuestAddress)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::DelegateOutputWrite;
	Call.IntArgs[0] = TransactionToken;
	Call.IntArgs[1] = OutputOrdinal;
	Call.GuestAddress = static_cast<uint32>(GuestAddress);
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::DelegateOutputWrite),
		Call,
		Result)
		? Result.ReturnValue
		: 0;
}

int32_t DebugProbe(wasm_exec_env_t ExecEnv, int64_t ProbeId)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::DebugProbe;
	Call.Int64Args[0] = ProbeId;
	FAvidScriptHostCallResult Result;
	return Dispatch(
		ExecEnv,
		StaticImportName(EAvidScriptHostBindingId::DebugProbe),
		Call,
		Result)
		? Result.ReturnValue
		: static_cast<int32>(EAvidScriptDebugProbeAction::Abort);
}

int64_t DebugSuspend(
	wasm_exec_env_t ExecEnv,
	int64_t ProbeId,
	int32_t ResumeRoute,
	int32_t InputAddress,
	int32_t ByteCount)
{
	const char* ImportName = StaticImportName(EAvidScriptHostBindingId::DebugSuspend);
	IAvidScriptWamrHostBridge* Bridge = GetBridge(ExecEnv);
	if (ResumeRoute <= 0 || ByteCount <= 0 || ByteCount > MaxDebugFrameBytes)
	{
		Fail(
			ExecEnv,
			Bridge,
			ImportName,
			TEXT("The debug suspension frame arguments are outside the supported range."));
		return 0;
	}

	void* NativeInput = nullptr;
	if (!TranslateGuestRange(
			ExecEnv,
			ImportName,
			TEXT("debug suspension frame input"),
			InputAddress,
			static_cast<uint32>(ByteCount),
			1,
			1,
			NativeInput))
	{
		return 0;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::DebugSuspend;
	Call.Int64Args[0] = ProbeId;
	Call.IntArgs[0] = ResumeRoute;
	Call.IntArgs[1] = ByteCount;
	Call.GuestAddress = static_cast<uint32>(InputAddress);
	Call.InputBytes = MakeArrayView(
		static_cast<const uint8*>(NativeInput),
		ByteCount);
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result)
		? Result.ReturnValueI64
		: 0;
}

int32_t DebugFrameRead(
	wasm_exec_env_t ExecEnv,
	int64_t SuspensionToken,
	int32_t OutputAddress,
	int32_t ByteCount)
{
	const char* ImportName = StaticImportName(EAvidScriptHostBindingId::DebugFrameRead);
	IAvidScriptWamrHostBridge* Bridge = GetBridge(ExecEnv);
	if (SuspensionToken <= 0 || ByteCount <= 0 || ByteCount > MaxDebugFrameBytes)
	{
		Fail(
			ExecEnv,
			Bridge,
			ImportName,
			TEXT("The debug resume frame arguments are outside the supported range."));
		return 0;
	}

	void* NativeOutput = nullptr;
	if (!TranslateGuestRange(
			ExecEnv,
			ImportName,
			TEXT("debug suspension frame output"),
			OutputAddress,
			static_cast<uint32>(ByteCount),
			1,
			1,
			NativeOutput))
	{
		return 0;
	}

	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::DebugFrameRead;
	Call.Int64Args[0] = SuspensionToken;
	Call.IntArgs[1] = ByteCount;
	Call.GuestAddress = static_cast<uint32>(OutputAddress);
	Call.OutputBytes = MakeArrayView(
		static_cast<uint8*>(NativeOutput),
		ByteCount);
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, ImportName, Call, Result)
		? Result.ReturnValue
		: 0;
}

void* GetWamrStaticHostFunction(EAvidScriptHostBindingId BindingId)
{
	switch (BindingId)
	{
	case EAvidScriptHostBindingId::HostAddI32: return reinterpret_cast<void*>(HostAddI32);
	case EAvidScriptHostBindingId::HostFailI32: return reinterpret_cast<void*>(HostFailI32);
	case EAvidScriptHostBindingId::ActorGetLocation: return reinterpret_cast<void*>(ActorGetLocation);
	case EAvidScriptHostBindingId::ActorSetLocation: return reinterpret_cast<void*>(ActorSetLocation);
	case EAvidScriptHostBindingId::ActorAddLocationOffset: return reinterpret_cast<void*>(ActorAddLocationOffset);
	case EAvidScriptHostBindingId::ActorGetRotation: return reinterpret_cast<void*>(ActorGetRotation);
	case EAvidScriptHostBindingId::ActorSetRotation: return reinterpret_cast<void*>(ActorSetRotation);
	case EAvidScriptHostBindingId::ActorGetScale: return reinterpret_cast<void*>(ActorGetScale);
	case EAvidScriptHostBindingId::ActorSetScale: return reinterpret_cast<void*>(ActorSetScale);
	case EAvidScriptHostBindingId::ActorGetTransformBatch: return reinterpret_cast<void*>(ActorGetTransformBatch);
	case EAvidScriptHostBindingId::ActorGetRootComponent: return reinterpret_cast<void*>(ActorGetRootComponent);
	case EAvidScriptHostBindingId::SceneComponentGetWorldLocation: return reinterpret_cast<void*>(SceneComponentGetWorldLocation);
	case EAvidScriptHostBindingId::SceneComponentSetWorldLocation: return reinterpret_cast<void*>(SceneComponentSetWorldLocation);
	case EAvidScriptHostBindingId::OwnerGetSlot: return reinterpret_cast<void*>(OwnerGetSlot);
	case EAvidScriptHostBindingId::OwnerGetGeneration: return reinterpret_cast<void*>(OwnerGetGeneration);
	case EAvidScriptHostBindingId::OwnerGetHandle: return reinterpret_cast<void*>(OwnerGetHandle);
	case EAvidScriptHostBindingId::TimerSetOnce: return reinterpret_cast<void*>(TimerSetOnce);
	case EAvidScriptHostBindingId::TimerCancel: return reinterpret_cast<void*>(TimerCancel);
	case EAvidScriptHostBindingId::ContinuationDelay: return reinterpret_cast<void*>(ContinuationDelay);
	case EAvidScriptHostBindingId::ContinuationCancel: return reinterpret_cast<void*>(ContinuationCancel);
	case EAvidScriptHostBindingId::ContinuationLoadObject: return reinterpret_cast<void*>(ContinuationLoadObject);
	case EAvidScriptHostBindingId::ContinuationCancelSourceCreate: return reinterpret_cast<void*>(ContinuationCancelSourceCreate);
	case EAvidScriptHostBindingId::ContinuationCancelSourceCancel: return reinterpret_cast<void*>(ContinuationCancelSourceCancel);
	case EAvidScriptHostBindingId::ContinuationCancelSourceRelease: return reinterpret_cast<void*>(ContinuationCancelSourceRelease);
	case EAvidScriptHostBindingId::ContinuationBindCancel: return reinterpret_cast<void*>(ContinuationBindCancel);
	case EAvidScriptHostBindingId::ContinuationResultRead: return reinterpret_cast<void*>(ContinuationResultRead);
	case EAvidScriptHostBindingId::ContinuationStateStore: return reinterpret_cast<void*>(ContinuationStateStore);
	case EAvidScriptHostBindingId::ContinuationStateRead: return reinterpret_cast<void*>(ContinuationStateRead);
	case EAvidScriptHostBindingId::EventSubscribe: return reinterpret_cast<void*>(EventSubscribe);
	case EAvidScriptHostBindingId::EventUnsubscribe: return reinterpret_cast<void*>(EventUnsubscribe);
	case EAvidScriptHostBindingId::EventIsCurrentSource: return reinterpret_cast<void*>(EventIsCurrentSource);
	case EAvidScriptHostBindingId::DelegateOutputWrite: return reinterpret_cast<void*>(DelegateOutputWrite);
	case EAvidScriptHostBindingId::DataLaneGetEpoch: return reinterpret_cast<void*>(DataLaneGetEpoch);
	case EAvidScriptHostBindingId::DataLaneSubmit: return reinterpret_cast<void*>(DataLaneSubmit);
	case EAvidScriptHostBindingId::ValueArrayLength: return reinterpret_cast<void*>(ValueArrayLength);
	case EAvidScriptHostBindingId::ValueArrayLoad: return reinterpret_cast<void*>(ValueArrayLoad);
	case EAvidScriptHostBindingId::ValueArrayStore: return reinterpret_cast<void*>(ValueArrayStore);
	case EAvidScriptHostBindingId::ValueArrayReadRange: return reinterpret_cast<void*>(ValueArrayReadRange);
	case EAvidScriptHostBindingId::ValueArrayWriteRange: return reinterpret_cast<void*>(ValueArrayWriteRange);
	case EAvidScriptHostBindingId::ValueRelease: return reinterpret_cast<void*>(ValueRelease);
	case EAvidScriptHostBindingId::ValueTextToString: return reinterpret_cast<void*>(ValueTextToString);
	case EAvidScriptHostBindingId::ValueContainerCount: return reinterpret_cast<void*>(ValueContainerCount);
	case EAvidScriptHostBindingId::ValueContainerRead: return reinterpret_cast<void*>(ValueContainerRead);
	case EAvidScriptHostBindingId::ValueContainerWrite: return reinterpret_cast<void*>(ValueContainerWrite);
	case EAvidScriptHostBindingId::ValueContainerResize: return reinterpret_cast<void*>(ValueContainerResize);
	case EAvidScriptHostBindingId::ValueContainerClear: return reinterpret_cast<void*>(ValueContainerClear);
	case EAvidScriptHostBindingId::ValueContainerFind: return reinterpret_cast<void*>(ValueContainerFind);
	case EAvidScriptHostBindingId::ValueContainerUpsert: return reinterpret_cast<void*>(ValueContainerUpsert);
	case EAvidScriptHostBindingId::ValueContainerRemove: return reinterpret_cast<void*>(ValueContainerRemove);
	case EAvidScriptHostBindingId::DebugProbe: return reinterpret_cast<void*>(DebugProbe);
	case EAvidScriptHostBindingId::DebugSuspend: return reinterpret_cast<void*>(DebugSuspend);
	case EAvidScriptHostBindingId::DebugFrameRead: return reinterpret_cast<void*>(DebugFrameRead);
	default: return nullptr;
	}
}

TArray<NativeSymbol> GNativeSymbols;
TArray<NativeSymbol> GCompatibilityNativeSymbols;

void BuildWamrStaticHostSymbolTables()
{
	if (!GNativeSymbols.IsEmpty())
	{
		return;
	}
	const TConstArrayView<FAvidScriptVmStaticHostImport> Imports = GetAvidScriptVmStaticHostImports();
	GNativeSymbols.Reserve(Imports.Num());
	GCompatibilityNativeSymbols.Reserve(Imports.Num());
	for (const FAvidScriptVmStaticHostImport& Import : Imports)
	{
		NativeSymbol Symbol = {
			Import.ImportName,
			GetWamrStaticHostFunction(Import.BindingId),
			Import.Signature,
			nullptr
		};
		check(Symbol.func_ptr != nullptr);
		GNativeSymbols.Add(Symbol);
		if (Import.bSupportsEnvCompatibility)
		{
			GCompatibilityNativeSymbols.Add(Symbol);
		}
	}
}
#endif
}

bool IsAvidScriptVmStaticHostImport(const FString& ModuleName, const FString& ImportName)
{
	if (ModuleName != TEXT("avidscript") && ModuleName != TEXT("env"))
	{
		return false;
	}
	for (const FAvidScriptVmStaticHostImport& Import : GetAvidScriptVmStaticHostImports())
	{
		if (ImportName == UTF8_TO_TCHAR(Import.ImportName)
			&& (ModuleName == TEXT("avidscript") || Import.bSupportsEnvCompatibility))
		{
			return true;
		}
	}
	return false;
}

bool RegisterAvidScriptWamrHostBindings()
{
#if !AVIDSCRIPT_WITH_WAMR
	return false;
#else
	BuildWamrStaticHostSymbolTables();
	if (!wasm_runtime_register_natives(CanonicalModuleName, GNativeSymbols.GetData(), GNativeSymbols.Num()))
	{
		return false;
	}

	if (!wasm_runtime_register_natives(
		CompatibilityModuleName,
		GCompatibilityNativeSymbols.GetData(),
		GCompatibilityNativeSymbols.Num()))
	{
		wasm_runtime_unregister_natives(CanonicalModuleName, GNativeSymbols.GetData());
		return false;
	}

	return true;
#endif
}

void UnregisterAvidScriptWamrHostBindings()
{
#if AVIDSCRIPT_WITH_WAMR
	wasm_runtime_unregister_natives(CompatibilityModuleName, GCompatibilityNativeSymbols.GetData());
	wasm_runtime_unregister_natives(CanonicalModuleName, GNativeSymbols.GetData());
#endif
}
