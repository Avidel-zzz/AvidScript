#include "AvidScriptWamrHostBindings.h"

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

bool Fail(wasm_exec_env_t ExecEnv, IAvidScriptWamrHostBridge* Bridge, const char* ImportName, const FString& Details)
{
	if (Bridge != nullptr)
	{
		Bridge->RecordHostImportFailure(ImportName, Details);
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
		return Fail(ExecEnv, Bridge, ImportName, Details);
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
	return DispatchI32(ExecEnv, EAvidScriptHostBindingId::HostAddI32, "host_add_i32", Input);
}

int32_t HostFailI32(wasm_exec_env_t ExecEnv, int32_t Input)
{
	return DispatchI32(ExecEnv, EAvidScriptHostBindingId::HostFailI32, "host_fail_i32", Input);
}

int32_t ActorGetLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutAddress)
{
	return DispatchVectorRead(ExecEnv, EAvidScriptHostBindingId::ActorGetLocation, "actor_get_location", Slot, Generation, OutAddress);
}

int32_t ActorSetLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::ActorSetLocation, "actor_set_location", Slot, Generation, X, Y, Z);
}

int32_t ActorAddLocationOffset(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::ActorAddLocationOffset, "actor_add_location_offset", Slot, Generation, X, Y, Z);
}

int32_t ActorGetRotation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutAddress)
{
	return DispatchVectorRead(ExecEnv, EAvidScriptHostBindingId::ActorGetRotation, "actor_get_rotation", Slot, Generation, OutAddress);
}

int32_t ActorSetRotation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float Pitch, float Yaw, float Roll)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::ActorSetRotation, "actor_set_rotation", Slot, Generation, Pitch, Yaw, Roll);
}

int32_t ActorGetScale(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutAddress)
{
	return DispatchVectorRead(ExecEnv, EAvidScriptHostBindingId::ActorGetScale, "actor_get_scale", Slot, Generation, OutAddress);
}

int32_t ActorSetScale(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::ActorSetScale, "actor_set_scale", Slot, Generation, X, Y, Z);
}

int32_t ActorGetTransformBatch(
	wasm_exec_env_t ExecEnv,
	int32_t InputAddress,
	int32_t Count,
	int32_t OutputAddress)
{
	constexpr const char* ImportName = "actor_get_transform_batch";
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
	if (!Dispatch(ExecEnv, "actor_get_root_component", Call, Result))
	{
		return 0;
	}

	const uint32 HandleValues[2] = {
		Result.IntValues[0],
		Result.IntValues[1]
	};
	return WriteGuestBytes(ExecEnv, "actor_get_root_component", OutAddress, HandleValues, sizeof(HandleValues))
		? Result.ReturnValue
		: 0;
}

int32_t SceneComponentGetWorldLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutAddress)
{
	return DispatchVectorRead(ExecEnv, EAvidScriptHostBindingId::SceneComponentGetWorldLocation, "scene_component_get_world_location", Slot, Generation, OutAddress);
}

int32_t SceneComponentSetWorldLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	return DispatchActorVectorWrite(ExecEnv, EAvidScriptHostBindingId::SceneComponentSetWorldLocation, "scene_component_set_world_location", Slot, Generation, X, Y, Z);
}

int32_t OwnerGetSlot(wasm_exec_env_t ExecEnv)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::OwnerGetSlot;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, "owner_get_slot", Call, Result) ? Result.ReturnValue : 0;
}

int32_t OwnerGetGeneration(wasm_exec_env_t ExecEnv)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::OwnerGetGeneration;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, "owner_get_generation", Call, Result) ? Result.ReturnValue : 0;
}

int32_t TimerSetOnce(wasm_exec_env_t ExecEnv, float DelaySeconds, int32_t CallbackId)
{
	FAvidScriptHostCall Call;
	Call.BindingId = EAvidScriptHostBindingId::TimerSetOnce;
	Call.FloatArgs[0] = DelaySeconds;
	Call.IntArgs[0] = CallbackId;
	FAvidScriptHostCallResult Result;
	return Dispatch(ExecEnv, "timer_set_once", Call, Result) ? Result.ReturnValue : 0;
}

int32_t TimerCancel(wasm_exec_env_t ExecEnv, int32_t TimerHandle)
{
	return DispatchI32(ExecEnv, EAvidScriptHostBindingId::TimerCancel, "timer_cancel", TimerHandle);
}

NativeSymbol GNativeSymbols[] = {
	{ "host_add_i32", reinterpret_cast<void*>(HostAddI32), "(i)i", nullptr },
	{ "host_fail_i32", reinterpret_cast<void*>(HostFailI32), "(i)i", nullptr },
	{ "actor_get_location", reinterpret_cast<void*>(ActorGetLocation), "(iii)i", nullptr },
	{ "actor_set_location", reinterpret_cast<void*>(ActorSetLocation), "(iifff)i", nullptr },
	{ "actor_add_location_offset", reinterpret_cast<void*>(ActorAddLocationOffset), "(iifff)i", nullptr },
	{ "actor_get_rotation", reinterpret_cast<void*>(ActorGetRotation), "(iii)i", nullptr },
	{ "actor_set_rotation", reinterpret_cast<void*>(ActorSetRotation), "(iifff)i", nullptr },
	{ "actor_get_scale", reinterpret_cast<void*>(ActorGetScale), "(iii)i", nullptr },
	{ "actor_set_scale", reinterpret_cast<void*>(ActorSetScale), "(iifff)i", nullptr },
	{ "actor_get_transform_batch", reinterpret_cast<void*>(ActorGetTransformBatch), "(iii)i", nullptr },
	{ "actor_get_root_component", reinterpret_cast<void*>(ActorGetRootComponent), "(iii)i", nullptr },
	{ "scene_component_get_world_location", reinterpret_cast<void*>(SceneComponentGetWorldLocation), "(iii)i", nullptr },
	{ "scene_component_set_world_location", reinterpret_cast<void*>(SceneComponentSetWorldLocation), "(iifff)i", nullptr },
	{ "owner_get_slot", reinterpret_cast<void*>(OwnerGetSlot), "()i", nullptr },
	{ "owner_get_generation", reinterpret_cast<void*>(OwnerGetGeneration), "()i", nullptr },
	{ "timer_set_once", reinterpret_cast<void*>(TimerSetOnce), "(fi)i", nullptr },
	{ "timer_cancel", reinterpret_cast<void*>(TimerCancel), "(i)i", nullptr }
};
#endif
}

bool RegisterAvidScriptWamrHostBindings()
{
#if !AVIDSCRIPT_WITH_WAMR
	return false;
#else
	if (!wasm_runtime_register_natives(CanonicalModuleName, GNativeSymbols, UE_ARRAY_COUNT(GNativeSymbols)))
	{
		return false;
	}

	if (!wasm_runtime_register_natives(CompatibilityModuleName, GNativeSymbols, UE_ARRAY_COUNT(GNativeSymbols)))
	{
		wasm_runtime_unregister_natives(CanonicalModuleName, GNativeSymbols);
		return false;
	}

	return true;
#endif
}

void UnregisterAvidScriptWamrHostBindings()
{
#if AVIDSCRIPT_WITH_WAMR
	wasm_runtime_unregister_natives(CompatibilityModuleName, GNativeSymbols);
	wasm_runtime_unregister_natives(CanonicalModuleName, GNativeSymbols);
#endif
}
