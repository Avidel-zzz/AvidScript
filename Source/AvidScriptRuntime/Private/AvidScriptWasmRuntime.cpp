#include "AvidScriptWasmRuntime.h"

#include "AvidScriptSceneComponentBinding.h"

#ifndef AVIDSCRIPT_WITH_WAMR
#define AVIDSCRIPT_WITH_WAMR 0
#endif

#if AVIDSCRIPT_WITH_WAMR
extern "C"
{
#include "wasm_export.h"
}
#endif

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWasmRuntime, Log, All);

namespace
{
constexpr uint32 AvidScriptWasmStackSize = 64 * 1024;
constexpr uint32 AvidScriptWasmHeapSize = 64 * 1024;
constexpr uint32 AvidScriptWasmErrorBufferSize = 512;
constexpr double AvidScriptMinimumMeasuredMs = 0.0001;
constexpr int32 AvidScriptMaximumPendingTimers = 1024;

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

#if AVIDSCRIPT_WITH_WAMR
constexpr const char* AvidScriptHostImportModuleName = "avidscript";
constexpr const char* AvidScriptLdcDefaultImportModuleName = "env";
constexpr const char* AvidScriptHostAddI32Name = "host_add_i32";
constexpr const char* AvidScriptHostFailI32Name = "host_fail_i32";
constexpr const char* AvidScriptActorGetLocationName = "actor_get_location";
constexpr const char* AvidScriptActorSetLocationName = "actor_set_location";
constexpr const char* AvidScriptActorAddLocationOffsetName = "actor_add_location_offset";
constexpr const char* AvidScriptActorGetRotationName = "actor_get_rotation";
constexpr const char* AvidScriptActorSetRotationName = "actor_set_rotation";
constexpr const char* AvidScriptActorGetScaleName = "actor_get_scale";
constexpr const char* AvidScriptActorSetScaleName = "actor_set_scale";
constexpr const char* AvidScriptActorGetRootComponentName = "actor_get_root_component";
constexpr const char* AvidScriptSceneComponentGetWorldLocationName = "scene_component_get_world_location";
constexpr const char* AvidScriptSceneComponentSetWorldLocationName = "scene_component_set_world_location";
constexpr const char* AvidScriptOwnerGetSlotName = "owner_get_slot";
constexpr const char* AvidScriptOwnerGetGenerationName = "owner_get_generation";
constexpr const char* AvidScriptTimerSetOnceName = "timer_set_once";
constexpr const char* AvidScriptTimerCancelName = "timer_cancel";

int32_t AvidScriptHostAddI32(wasm_exec_env_t ExecEnv, int32_t Input);
int32_t AvidScriptHostFailI32(wasm_exec_env_t ExecEnv, int32_t Input);
int32_t AvidScriptActorGetLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutLocationPtr);
int32_t AvidScriptActorSetLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z);
int32_t AvidScriptActorAddLocationOffset(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z);
int32_t AvidScriptActorGetRotation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutRotationPtr);
int32_t AvidScriptActorSetRotation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float Pitch, float Yaw, float Roll);
int32_t AvidScriptActorGetScale(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutScalePtr);
int32_t AvidScriptActorSetScale(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z);
int32_t AvidScriptActorGetRootComponent(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutHandlePtr);
int32_t AvidScriptSceneComponentGetWorldLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutLocationPtr);
int32_t AvidScriptSceneComponentSetWorldLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z);
int32_t AvidScriptOwnerGetSlot(wasm_exec_env_t ExecEnv);
int32_t AvidScriptOwnerGetGeneration(wasm_exec_env_t ExecEnv);
int32_t AvidScriptTimerSetOnce(wasm_exec_env_t ExecEnv, float DelaySeconds, int32_t CallbackId);
int32_t AvidScriptTimerCancel(wasm_exec_env_t ExecEnv, int32_t TimerHandle);

NativeSymbol GAvidScriptNativeSymbols[] = {
	{ AvidScriptHostAddI32Name, reinterpret_cast<void*>(AvidScriptHostAddI32), "(i)i", nullptr },
	{ AvidScriptHostFailI32Name, reinterpret_cast<void*>(AvidScriptHostFailI32), "(i)i", nullptr },
	{ AvidScriptActorGetLocationName, reinterpret_cast<void*>(AvidScriptActorGetLocation), "(iii)i", nullptr },
	{ AvidScriptActorSetLocationName, reinterpret_cast<void*>(AvidScriptActorSetLocation), "(iifff)i", nullptr },
	{ AvidScriptActorAddLocationOffsetName, reinterpret_cast<void*>(AvidScriptActorAddLocationOffset), "(iifff)i", nullptr },
	{ AvidScriptActorGetRotationName, reinterpret_cast<void*>(AvidScriptActorGetRotation), "(iii)i", nullptr },
	{ AvidScriptActorSetRotationName, reinterpret_cast<void*>(AvidScriptActorSetRotation), "(iifff)i", nullptr },
	{ AvidScriptActorGetScaleName, reinterpret_cast<void*>(AvidScriptActorGetScale), "(iii)i", nullptr },
	{ AvidScriptActorSetScaleName, reinterpret_cast<void*>(AvidScriptActorSetScale), "(iifff)i", nullptr },
	{ AvidScriptActorGetRootComponentName, reinterpret_cast<void*>(AvidScriptActorGetRootComponent), "(iii)i", nullptr },
	{ AvidScriptSceneComponentGetWorldLocationName, reinterpret_cast<void*>(AvidScriptSceneComponentGetWorldLocation), "(iii)i", nullptr },
	{ AvidScriptSceneComponentSetWorldLocationName, reinterpret_cast<void*>(AvidScriptSceneComponentSetWorldLocation), "(iifff)i", nullptr },
	{ AvidScriptOwnerGetSlotName, reinterpret_cast<void*>(AvidScriptOwnerGetSlot), "()i", nullptr },
	{ AvidScriptOwnerGetGenerationName, reinterpret_cast<void*>(AvidScriptOwnerGetGeneration), "()i", nullptr },
	{ AvidScriptTimerSetOnceName, reinterpret_cast<void*>(AvidScriptTimerSetOnce), "(fi)i", nullptr },
	{ AvidScriptTimerCancelName, reinterpret_cast<void*>(AvidScriptTimerCancel), "(i)i", nullptr }
};

FCriticalSection GWamrRuntimeCriticalSection;
int32 GWamrRuntimeRefCount = 0;
bool bAvidScriptNativeSymbolsRegistered = false;
#endif

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
		TEXT("AvidScript WAMR error | backend=WAMR | module=%s | export=%s%s | category=%s | details=%s | next=%s"),
		ModuleId.IsEmpty() ? TEXT("<none>") : *ModuleId,
		ExportName.IsEmpty() ? TEXT("<none>") : *ExportName,
		*ImportText,
		*Category,
		*Details,
		*NextAction);

	UE_LOG(LogAvidScriptWasmRuntime, Warning, TEXT("%s"), *OutResult.ErrorMessage);
}

#if AVIDSCRIPT_WITH_WAMR
FAvidScriptWasmRuntimeInstance* GetRuntimeInstanceFromExecEnv(wasm_exec_env_t ExecEnv)
{
	return ExecEnv != nullptr
		? static_cast<FAvidScriptWasmRuntimeInstance*>(wasm_runtime_get_user_data(ExecEnv))
		: nullptr;
}

void SetHostImportException(wasm_exec_env_t ExecEnv, const char* Details)
{
	if (ExecEnv == nullptr)
	{
		return;
	}

	wasm_module_inst_t ModuleInstance = wasm_runtime_get_module_inst(ExecEnv);
	if (ModuleInstance != nullptr)
	{
		wasm_runtime_set_exception(ModuleInstance, Details);
	}
}

int32_t AvidScriptHostAddI32(wasm_exec_env_t ExecEnv, int32_t Input)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.host_add_i32");
		return 0;
	}

	return RuntimeInstance->HandleHostAddI32Import(static_cast<int32>(Input));
}

int32_t AvidScriptHostFailI32(wasm_exec_env_t ExecEnv, int32_t Input)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.host_fail_i32");
		return 0;
	}

	RuntimeInstance->HandleHostFailI32Import(static_cast<int32>(Input));
	SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.host_fail_i32 returned failure");
	return 0;
}

int32_t AvidScriptActorGetLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutLocationPtr)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.actor_get_location");
		return 0;
	}

	wasm_module_inst_t WamrModuleInstance = wasm_runtime_get_module_inst(ExecEnv);
	if (WamrModuleInstance == nullptr || OutLocationPtr <= 0 ||
		!wasm_runtime_validate_app_addr(WamrModuleInstance, static_cast<uint64_t>(OutLocationPtr), sizeof(float) * 3))
	{
		RuntimeInstance->SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_location"),
			FString::Printf(TEXT("Invalid output pointer %d for avidscript.actor_get_location"), OutLocationPtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_location received invalid output pointer");
		return 0;
	}

	FVector Location = FVector::ZeroVector;
	if (RuntimeInstance->HandleActorGetLocationImport(static_cast<int32>(Slot), static_cast<int32>(Generation), Location) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_location returned failure");
		return 0;
	}

	float* OutLocation = static_cast<float*>(wasm_runtime_addr_app_to_native(WamrModuleInstance, static_cast<uint64_t>(OutLocationPtr)));
	if (OutLocation == nullptr)
	{
		RuntimeInstance->SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_location"),
			FString::Printf(TEXT("Failed to translate output pointer %d for avidscript.actor_get_location"), OutLocationPtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_location pointer translation failed");
		return 0;
	}

	OutLocation[0] = static_cast<float>(Location.X);
	OutLocation[1] = static_cast<float>(Location.Y);
	OutLocation[2] = static_cast<float>(Location.Z);
	return 1;
}

int32_t AvidScriptActorSetLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.actor_set_location");
		return 0;
	}

	const FVector Location(static_cast<double>(X), static_cast<double>(Y), static_cast<double>(Z));
	if (RuntimeInstance->HandleActorSetLocationImport(static_cast<int32>(Slot), static_cast<int32>(Generation), Location) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_set_location returned failure");
		return 0;
	}

	return 1;
}

int32_t AvidScriptActorAddLocationOffset(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.actor_add_location_offset");
		return 0;
	}

	const FVector Offset(static_cast<double>(X), static_cast<double>(Y), static_cast<double>(Z));
	if (RuntimeInstance->HandleActorAddLocationOffsetImport(static_cast<int32>(Slot), static_cast<int32>(Generation), Offset) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_add_location_offset returned failure");
		return 0;
	}

	return 1;
}

int32_t AvidScriptActorGetRotation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutRotationPtr)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.actor_get_rotation");
		return 0;
	}

	wasm_module_inst_t WamrModuleInstance = wasm_runtime_get_module_inst(ExecEnv);
	if (WamrModuleInstance == nullptr || OutRotationPtr <= 0 ||
		!wasm_runtime_validate_app_addr(WamrModuleInstance, static_cast<uint64_t>(OutRotationPtr), sizeof(float) * 3))
	{
		RuntimeInstance->SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_rotation"),
			FString::Printf(TEXT("Invalid output pointer %d for avidscript.actor_get_rotation"), OutRotationPtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_rotation received invalid output pointer");
		return 0;
	}

	FRotator Rotation = FRotator::ZeroRotator;
	if (RuntimeInstance->HandleActorGetRotationImport(static_cast<int32>(Slot), static_cast<int32>(Generation), Rotation) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_rotation returned failure");
		return 0;
	}

	float* OutRotation = static_cast<float*>(wasm_runtime_addr_app_to_native(WamrModuleInstance, static_cast<uint64_t>(OutRotationPtr)));
	if (OutRotation == nullptr)
	{
		RuntimeInstance->SetPendingHostImportFailure(
			TEXT("avidscript"),
			TEXT("actor_get_rotation"),
			FString::Printf(TEXT("Failed to translate output pointer %d for avidscript.actor_get_rotation"), OutRotationPtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_rotation pointer translation failed");
		return 0;
	}

	OutRotation[0] = static_cast<float>(Rotation.Pitch);
	OutRotation[1] = static_cast<float>(Rotation.Yaw);
	OutRotation[2] = static_cast<float>(Rotation.Roll);
	return 1;
}

int32_t AvidScriptActorSetRotation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float Pitch, float Yaw, float Roll)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.actor_set_rotation");
		return 0;
	}

	const FRotator Rotation(static_cast<double>(Pitch), static_cast<double>(Yaw), static_cast<double>(Roll));
	if (RuntimeInstance->HandleActorSetRotationImport(static_cast<int32>(Slot), static_cast<int32>(Generation), Rotation) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_set_rotation returned failure");
		return 0;
	}

	return 1;
}

int32_t AvidScriptActorGetScale(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutScalePtr)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.actor_get_scale");
		return 0;
	}

	wasm_module_inst_t WamrModuleInstance = wasm_runtime_get_module_inst(ExecEnv);
	if (WamrModuleInstance == nullptr || OutScalePtr <= 0 ||
		!wasm_runtime_validate_app_addr(WamrModuleInstance, static_cast<uint64_t>(OutScalePtr), sizeof(float) * 3))
	{
		RuntimeInstance->SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_scale"), FString::Printf(TEXT("Invalid output pointer %d for avidscript.actor_get_scale"), OutScalePtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_scale received invalid output pointer");
		return 0;
	}

	FVector Scale3D = FVector::ZeroVector;
	if (RuntimeInstance->HandleActorGetScaleImport(static_cast<int32>(Slot), static_cast<int32>(Generation), Scale3D) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_scale returned failure");
		return 0;
	}

	float* OutScale = static_cast<float*>(wasm_runtime_addr_app_to_native(WamrModuleInstance, static_cast<uint64_t>(OutScalePtr)));
	if (OutScale == nullptr)
	{
		RuntimeInstance->SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_scale"), FString::Printf(TEXT("Failed to translate output pointer %d for avidscript.actor_get_scale"), OutScalePtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_scale pointer translation failed");
		return 0;
	}

	OutScale[0] = static_cast<float>(Scale3D.X);
	OutScale[1] = static_cast<float>(Scale3D.Y);
	OutScale[2] = static_cast<float>(Scale3D.Z);
	return 1;
}

int32_t AvidScriptActorSetScale(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.actor_set_scale");
		return 0;
	}

	const FVector Scale3D(static_cast<double>(X), static_cast<double>(Y), static_cast<double>(Z));
	if (RuntimeInstance->HandleActorSetScaleImport(static_cast<int32>(Slot), static_cast<int32>(Generation), Scale3D) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_set_scale returned failure");
		return 0;
	}

	return 1;
}
int32_t AvidScriptActorGetRootComponent(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutHandlePtr)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.actor_get_root_component");
		return 0;
	}

	wasm_module_inst_t WamrModuleInstance = wasm_runtime_get_module_inst(ExecEnv);
	if (WamrModuleInstance == nullptr || OutHandlePtr <= 0 || !wasm_runtime_validate_app_addr(WamrModuleInstance, static_cast<uint64_t>(OutHandlePtr), sizeof(uint32) * 2))
	{
		RuntimeInstance->SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_root_component"), FString::Printf(TEXT("Invalid output pointer %d for avidscript.actor_get_root_component"), OutHandlePtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_root_component received invalid output pointer");
		return 0;
	}

	FAvidScriptObjectHandle ComponentHandle;
	if (RuntimeInstance->HandleActorGetRootComponentImport(static_cast<int32>(Slot), static_cast<int32>(Generation), ComponentHandle) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_root_component returned failure");
		return 0;
	}

	uint32* OutHandle = static_cast<uint32*>(wasm_runtime_addr_app_to_native(WamrModuleInstance, static_cast<uint64_t>(OutHandlePtr)));
	if (OutHandle == nullptr)
	{
		RuntimeInstance->SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_get_root_component"), FString::Printf(TEXT("Failed to translate output pointer %d for avidscript.actor_get_root_component"), OutHandlePtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.actor_get_root_component pointer translation failed");
		return 0;
	}

	OutHandle[0] = ComponentHandle.Slot;
	OutHandle[1] = ComponentHandle.Generation;
	return 1;
}

int32_t AvidScriptSceneComponentGetWorldLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, int32_t OutLocationPtr)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.scene_component_get_world_location");
		return 0;
	}

	wasm_module_inst_t WamrModuleInstance = wasm_runtime_get_module_inst(ExecEnv);
	if (WamrModuleInstance == nullptr || OutLocationPtr <= 0 || !wasm_runtime_validate_app_addr(WamrModuleInstance, static_cast<uint64_t>(OutLocationPtr), sizeof(float) * 3))
	{
		RuntimeInstance->SetPendingHostImportFailure(TEXT("avidscript"), TEXT("scene_component_get_world_location"), FString::Printf(TEXT("Invalid output pointer %d for avidscript.scene_component_get_world_location"), OutLocationPtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.scene_component_get_world_location received invalid output pointer");
		return 0;
	}

	FVector WorldLocation = FVector::ZeroVector;
	if (RuntimeInstance->HandleSceneComponentGetWorldLocationImport(static_cast<int32>(Slot), static_cast<int32>(Generation), WorldLocation) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.scene_component_get_world_location returned failure");
		return 0;
	}

	float* OutLocation = static_cast<float*>(wasm_runtime_addr_app_to_native(WamrModuleInstance, static_cast<uint64_t>(OutLocationPtr)));
	if (OutLocation == nullptr)
	{
		RuntimeInstance->SetPendingHostImportFailure(TEXT("avidscript"), TEXT("scene_component_get_world_location"), FString::Printf(TEXT("Failed to translate output pointer %d for avidscript.scene_component_get_world_location"), OutLocationPtr));
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.scene_component_get_world_location pointer translation failed");
		return 0;
	}

	OutLocation[0] = static_cast<float>(WorldLocation.X);
	OutLocation[1] = static_cast<float>(WorldLocation.Y);
	OutLocation[2] = static_cast<float>(WorldLocation.Z);
	return 1;
}

int32_t AvidScriptSceneComponentSetWorldLocation(wasm_exec_env_t ExecEnv, int32_t Slot, int32_t Generation, float X, float Y, float Z)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.scene_component_set_world_location");
		return 0;
	}

	const FVector WorldLocation(static_cast<double>(X), static_cast<double>(Y), static_cast<double>(Z));
	if (RuntimeInstance->HandleSceneComponentSetWorldLocationImport(static_cast<int32>(Slot), static_cast<int32>(Generation), WorldLocation) == 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.scene_component_set_world_location returned failure");
		return 0;
	}
	return 1;
}

int32_t AvidScriptOwnerGetSlot(wasm_exec_env_t ExecEnv)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.owner_get_slot");
		return 0;
	}

	const int32 Slot = RuntimeInstance->HandleOwnerGetSlotImport();
	if (Slot <= 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.owner_get_slot returned failure");
		return 0;
	}

	return Slot;
}

int32_t AvidScriptOwnerGetGeneration(wasm_exec_env_t ExecEnv)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.owner_get_generation");
		return 0;
	}

	const int32 Generation = RuntimeInstance->HandleOwnerGetGenerationImport();
	if (Generation <= 0)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: avidscript.owner_get_generation returned failure");
		return 0;
	}

	return Generation;
}

int32_t AvidScriptTimerSetOnce(wasm_exec_env_t ExecEnv, float DelaySeconds, int32_t CallbackId)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.timer_set_once");
		return 0;
	}

	return RuntimeInstance->HandleTimerSetOnceImport(DelaySeconds, static_cast<int32>(CallbackId));
}

int32_t AvidScriptTimerCancel(wasm_exec_env_t ExecEnv, int32_t TimerHandle)
{
	FAvidScriptWasmRuntimeInstance* RuntimeInstance = GetRuntimeInstanceFromExecEnv(ExecEnv);
	if (RuntimeInstance == nullptr)
	{
		SetHostImportException(ExecEnv, "avidscript_host_import_failed: missing runtime instance for avidscript.timer_cancel");
		return 0;
	}

	return RuntimeInstance->HandleTimerCancelImport(static_cast<int32>(TimerHandle));
}

bool RegisterAvidScriptHostImports(const FString& ModuleId, FAvidScriptWasmSmokeResult& OutResult)
{
	if (bAvidScriptNativeSymbolsRegistered)
	{
		return true;
	}

	constexpr const char* ModuleNames[] = {
		AvidScriptHostImportModuleName,
		AvidScriptLdcDefaultImportModuleName
	};

	for (const char* ModuleName : ModuleNames)
	{
		if (!wasm_runtime_register_natives(
			ModuleName,
			GAvidScriptNativeSymbols,
			static_cast<uint32_t>(UE_ARRAY_COUNT(GAvidScriptNativeSymbols))))
		{
			for (const char* RegisteredModuleName : ModuleNames)
			{
				if (RegisteredModuleName == ModuleName)
				{
					break;
				}

				wasm_runtime_unregister_natives(RegisteredModuleName, GAvidScriptNativeSymbols);
			}

			SetFailure(
				OutResult,
				ModuleId,
				TEXT("<runtime>"),
				TEXT("host_import_registration_failed"),
				FString::Printf(TEXT("wasm_runtime_register_natives returned false for AvidScript host imports module '%s'"), ANSI_TO_TCHAR(ModuleName)),
				TEXT("verify WAMR native-symbol support and AvidScript import signatures"));
			return false;
		}
	}

	bAvidScriptNativeSymbolsRegistered = true;
	return true;
}

bool AcquireWamrRuntime(const FString& ModuleId, FAvidScriptWasmSmokeResult& OutResult)
{
	FScopeLock Lock(&GWamrRuntimeCriticalSection);

	if (GWamrRuntimeRefCount == 0)
	{
		if (!wasm_runtime_init())
		{
			SetFailure(
				OutResult,
				ModuleId,
				TEXT("<runtime>"),
				TEXT("runtime_init_failed"),
				TEXT("wasm_runtime_init returned false"),
				TEXT("verify WAMR build artifacts and platform initialization"));
			return false;
		}

		if (!RegisterAvidScriptHostImports(ModuleId, OutResult))
		{
			wasm_runtime_destroy();
			return false;
		}
	}

	++GWamrRuntimeRefCount;
	return true;
}

void ReleaseWamrRuntime()
{
	FScopeLock Lock(&GWamrRuntimeCriticalSection);

	if (GWamrRuntimeRefCount <= 0)
	{
		GWamrRuntimeRefCount = 0;
		return;
	}

	--GWamrRuntimeRefCount;
	if (GWamrRuntimeRefCount == 0)
	{
		if (bAvidScriptNativeSymbolsRegistered)
		{
			wasm_runtime_unregister_natives(AvidScriptLdcDefaultImportModuleName, GAvidScriptNativeSymbols);
			wasm_runtime_unregister_natives(AvidScriptHostImportModuleName, GAvidScriptNativeSymbols);
			bAvidScriptNativeSymbolsRegistered = false;
		}

		wasm_runtime_destroy();
	}
}

FString GetWamrException(wasm_module_inst_t InModuleInstance)
{
	if (InModuleInstance == nullptr)
	{
		return TEXT("No WAMR module instance is available.");
	}

	const char* Exception = wasm_runtime_get_exception(InModuleInstance);
	return Exception != nullptr ? UTF8_TO_TCHAR(Exception) : TEXT("WAMR did not report an exception.");
}

bool ValidateLinkedImports(
	wasm_module_t InModule,
	const FString& ModuleId,
	FAvidScriptWasmSmokeResult& OutResult)
{
	const int32 ImportCount = static_cast<int32>(wasm_runtime_get_import_count(InModule));
	if (ImportCount < 0)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<import>"),
			TEXT("import_inspection_failed"),
			TEXT("wasm_runtime_get_import_count returned a negative value"),
			TEXT("reject this script module and inspect the guest import section"));
		return false;
	}

	for (int32 ImportIndex = 0; ImportIndex < ImportCount; ++ImportIndex)
	{
		wasm_import_t ImportInfo = {};
		wasm_runtime_get_import_type(InModule, ImportIndex, &ImportInfo);

		if (ImportInfo.kind != WASM_IMPORT_EXPORT_KIND_FUNC)
		{
			continue;
		}

		const FString ImportModuleName = ImportInfo.module_name != nullptr ? FString(UTF8_TO_TCHAR(ImportInfo.module_name)) : FString();
		const FString ImportName = ImportInfo.name != nullptr ? FString(UTF8_TO_TCHAR(ImportInfo.name)) : FString();
		const bool bLinked = ImportInfo.linked;
		if (!bLinked)
		{
			SetFailure(
				OutResult,
				ModuleId,
				TEXT("<import>"),
				TEXT("missing_import"),
				FString::Printf(TEXT("Required host import '%s.%s' was not registered"), *ImportModuleName, *ImportName),
				TEXT("reject this script module and keep the previous live runtime"),
				ImportModuleName,
				ImportName);
			return false;
		}
	}

	return true;
}

bool CallWamrExport(
	wasm_module_inst_t InModuleInstance,
	wasm_exec_env_t InExecEnv,
	FAvidScriptWasmRuntimeInstance* RuntimeInstance,
	const FString& ModuleId,
	const char* ExportName,
	uint32 ArgCount,
	uint32* Args,
	FAvidScriptWasmSmokeResult& OutResult)
{
	const FString ExportNameText(UTF8_TO_TCHAR(ExportName));
	wasm_function_inst_t Function = wasm_runtime_lookup_function(InModuleInstance, ExportName);
	if (Function == nullptr)
	{
		SetFailure(
			OutResult,
			ModuleId,
			ExportNameText,
			TEXT("missing_export"),
			FString::Printf(TEXT("Required export '%s' was not found"), *ExportNameText),
			TEXT("skip this script instance and report the guest ABI mismatch"));
		return false;
	}

	if (!wasm_runtime_call_wasm(InExecEnv, Function, ArgCount, Args))
	{
		FString ImportModuleName;
		FString ImportName;
		FString Details;
		if (RuntimeInstance != nullptr && RuntimeInstance->ConsumePendingHostImportFailure(ImportModuleName, ImportName, Details))
		{
			SetFailure(
				OutResult,
				ModuleId,
				ExportNameText,
				TEXT("host_import_failed"),
				Details,
				TEXT("stop this script instance and surface the host import failure"),
				ImportModuleName,
				ImportName);
			return false;
		}

		SetFailure(
			OutResult,
			ModuleId,
			ExportNameText,
			TEXT("trap"),
			GetWamrException(InModuleInstance),
			TEXT("stop ticking this script instance and surface the trap to UE logs"));
		return false;
	}

	return true;
}
#endif
} // namespace

FAvidScriptWasmRuntimeInstance::~FAvidScriptWasmRuntimeInstance()
{
	Unload();
}

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
	Unload();
	Metrics = FAvidScriptWasmRuntimeMetrics();
	ResetHostImportState();
	ModuleId = InModuleId;
	PrepareResult(OutResult, ModuleId, Metrics);
	CopyHostImportStateToResult(OutResult);

#if !AVIDSCRIPT_WITH_WAMR
	SetFailure(
		OutResult,
		ModuleId,
		TEXT("<runtime>"),
		TEXT("backend_unavailable"),
		TEXT("WAMR backend is not available. Build WAMR and verify AVIDSCRIPT_WITH_WAMR=1."),
		TEXT("build the ThirdParty WAMR static library before running scripts"));
	return false;
#else
	if (Bytecode == nullptr || BytecodeSize <= 0)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<module>"),
			TEXT("invalid_bytecode"),
			TEXT("No WASM bytecode was provided"),
			TEXT("provide a non-empty WASM module buffer"));
		return false;
	}

	const double RuntimeInitStartSeconds = FPlatformTime::Seconds();
	if (!AcquireWamrRuntime(ModuleId, OutResult))
	{
		CopyHostImportStateToResult(OutResult);
		return false;
	}
	Metrics.RuntimeInitMs = MeasureElapsedMs(RuntimeInitStartSeconds);

	bOwnsRuntimeLease = true;
	OutResult.bRuntimeInitialized = true;
	OutResult.Metrics = Metrics;
	CopyHostImportStateToResult(OutResult);

	ModuleBuffer.Reset(BytecodeSize);
	ModuleBuffer.Append(Bytecode, BytecodeSize);

	char ErrorBuffer[AvidScriptWasmErrorBufferSize] = {};

	const double ModuleLoadStartSeconds = FPlatformTime::Seconds();
	Module = wasm_runtime_load(ModuleBuffer.GetData(), static_cast<uint32>(ModuleBuffer.Num()), ErrorBuffer, sizeof(ErrorBuffer));
	Metrics.ModuleLoadMs = MeasureElapsedMs(ModuleLoadStartSeconds);
	OutResult.Metrics = Metrics;
	CopyHostImportStateToResult(OutResult);
	if (Module == nullptr)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<module>"),
			TEXT("load_failed"),
			UTF8_TO_TCHAR(ErrorBuffer),
			TEXT("reject this script module and keep the runtime alive"));
		Unload();
		return false;
	}

	OutResult.bModuleLoaded = true;
	if (!ValidateLinkedImports(static_cast<wasm_module_t>(Module), ModuleId, OutResult))
	{
		OutResult.bRuntimeInitialized = bOwnsRuntimeLease;
		OutResult.bModuleLoaded = true;
		OutResult.Metrics = Metrics;
		CopyHostImportStateToResult(OutResult);
		Unload();
		return false;
	}

	const double ModuleInstantiateStartSeconds = FPlatformTime::Seconds();
	ModuleInstance = wasm_runtime_instantiate(
		static_cast<wasm_module_t>(Module),
		AvidScriptWasmStackSize,
		AvidScriptWasmHeapSize,
		ErrorBuffer,
		sizeof(ErrorBuffer));
	Metrics.ModuleInstantiateMs = MeasureElapsedMs(ModuleInstantiateStartSeconds);
	OutResult.Metrics = Metrics;
	CopyHostImportStateToResult(OutResult);
	if (ModuleInstance == nullptr)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<module>"),
			TEXT("instantiate_failed"),
			UTF8_TO_TCHAR(ErrorBuffer),
			TEXT("reject this script module and inspect stack/heap limits"));
		Unload();
		return false;
	}

	OutResult.bModuleInstantiated = true;

	const double ExecEnvCreateStartSeconds = FPlatformTime::Seconds();
	ExecEnv = wasm_runtime_create_exec_env(static_cast<wasm_module_inst_t>(ModuleInstance), AvidScriptWasmStackSize);
	Metrics.ExecEnvCreateMs = MeasureElapsedMs(ExecEnvCreateStartSeconds);
	OutResult.Metrics = Metrics;
	CopyHostImportStateToResult(OutResult);
	if (ExecEnv == nullptr)
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<exec_env>"),
			TEXT("exec_env_failed"),
			TEXT("wasm_runtime_create_exec_env returned null"),
			TEXT("reject this script module and inspect stack size"));
		Unload();
		return false;
	}

	wasm_runtime_set_user_data(static_cast<wasm_exec_env_t>(ExecEnv), this);
	FAvidScriptLifecycleTransitionResult LifecycleResult;
	if (!LifecycleState.TryTransition(EAvidScriptLifecycleState::Loaded, LifecycleResult))
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<lifecycle>"),
			TEXT("invalid_state"),
			TEXT("The runtime lifecycle rejected the Loaded transition"),
			TEXT("unload the session and create a fresh runtime instance"));
		Unload();
		return false;
	}
	return true;
#endif
}

bool FAvidScriptWasmRuntimeInstance::ValidateRequiredExports(
	const TArray<FString>& RequiredExports,
	FAvidScriptWasmSmokeResult& OutResult) const
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = bOwnsRuntimeLease;
	OutResult.bModuleLoaded = Module != nullptr;
	OutResult.bModuleInstantiated = ModuleInstance != nullptr;
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	OutResult.TickCallCount = TickCallCount;
	CopyHostImportStateToResult(OutResult);

#if !AVIDSCRIPT_WITH_WAMR
	SetFailure(
		OutResult,
		ModuleId,
		TEXT("<runtime>"),
		TEXT("backend_unavailable"),
		TEXT("WAMR backend is not available"),
		TEXT("build the ThirdParty WAMR static library before validating script exports"));
	return false;
#else
	if (!IsLoaded())
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("<module>"),
			TEXT("invalid_state"),
			TEXT("No WASM module is loaded"),
			TEXT("load a module before validating required exports"));
		return false;
	}

	for (const FString& RequiredExport : RequiredExports)
	{
		if (RequiredExport.IsEmpty())
		{
			SetFailure(
				OutResult,
				ModuleId,
				TEXT("<manifest>"),
				TEXT("missing_export"),
				TEXT("Required export name is empty"),
				TEXT("fix the reload manifest before activating this script"));
			return false;
		}

		FTCHARToUTF8 RequiredExportUtf8(*RequiredExport);
		wasm_function_inst_t Function = wasm_runtime_lookup_function(
			static_cast<wasm_module_inst_t>(ModuleInstance),
			RequiredExportUtf8.Get());
		if (Function == nullptr)
		{
			SetFailure(
				OutResult,
				ModuleId,
				RequiredExport,
				TEXT("missing_export"),
				FString::Printf(TEXT("Required export '%s' was not found"), *RequiredExport),
				TEXT("reject this script module and keep the previous live runtime"));
			return false;
		}
	}

	return true;
#endif
}

bool FAvidScriptWasmRuntimeInstance::BeginPlay(FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = bOwnsRuntimeLease;
	OutResult.bModuleLoaded = Module != nullptr;
	OutResult.bModuleInstantiated = ModuleInstance != nullptr;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	CopyHostImportStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
	CopyEventStateToResult(OutResult);

#if !AVIDSCRIPT_WITH_WAMR
	SetFailure(
		OutResult,
		ModuleId,
		TEXT("avid_on_begin_play"),
		TEXT("backend_unavailable"),
		TEXT("WAMR backend is not available"),
		TEXT("build the ThirdParty WAMR static library before running scripts"));
	return false;
#else
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
	if (!CallWamrExport(
		static_cast<wasm_module_inst_t>(ModuleInstance),
		static_cast<wasm_exec_env_t>(ExecEnv),
		this,
		ModuleId,
		"avid_on_begin_play",
		0,
		nullptr,
		OutResult))
	{
		Metrics.BeginPlayCallMs = MeasureElapsedMs(BeginPlayStartSeconds);
		OutResult.Metrics = Metrics;
		CopyHostImportStateToResult(OutResult);
		CopyTimerStateToResult(OutResult);
		CopyEventStateToResult(OutResult);
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
	CopyHostImportStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
	CopyEventStateToResult(OutResult);
	return true;
#endif
}

bool FAvidScriptWasmRuntimeInstance::Tick(float DeltaSeconds, FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = bOwnsRuntimeLease;
	OutResult.bModuleLoaded = Module != nullptr;
	OutResult.bModuleInstantiated = ModuleInstance != nullptr;
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	CopyHostImportStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
	CopyEventStateToResult(OutResult);

#if !AVIDSCRIPT_WITH_WAMR
	SetFailure(
		OutResult,
		ModuleId,
		TEXT("avid_on_tick"),
		TEXT("backend_unavailable"),
		TEXT("WAMR backend is not available"),
		TEXT("build the ThirdParty WAMR static library before running scripts"));
	return false;
#else
	if (!IsLoaded())
	{
		SetFailure(
			OutResult,
			ModuleId,
			TEXT("avid_on_tick"),
			TEXT("invalid_state"),
			TEXT("No WASM module is loaded"),
			TEXT("load a module before ticking"));
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
		return false;
	}

	TArray<int32> DueTimerHandles;
	CollectDueTimerHandles(DeltaSeconds, DueTimerHandles);
	Metrics.TimerCallbackCallMs = 0.0;

	uint32 TickArgs[1] = {};
	static_assert(sizeof(TickArgs[0]) == sizeof(DeltaSeconds), "WAMR f32 argument must fit in one cell.");
	FMemory::Memcpy(&TickArgs[0], &DeltaSeconds, sizeof(DeltaSeconds));

	const double TickStartSeconds = FPlatformTime::Seconds();
	if (!CallWamrExport(
		static_cast<wasm_module_inst_t>(ModuleInstance),
		static_cast<wasm_exec_env_t>(ExecEnv),
		this,
		ModuleId,
		"avid_on_tick",
		UE_ARRAY_COUNT(TickArgs),
		TickArgs,
		OutResult))
	{
		Metrics.TickCallMs = MeasureElapsedMs(TickStartSeconds);
		OutResult.Metrics = Metrics;
		CopyHostImportStateToResult(OutResult);
		CopyTimerStateToResult(OutResult);
		CopyEventStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	Metrics.TickCallMs = MeasureElapsedMs(TickStartSeconds);
	++TickCallCount;
	OutResult.Metrics = Metrics;
	OutResult.bTickCalled = true;
	OutResult.TickCallCount = TickCallCount;
	CopyHostImportStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
	CopyEventStateToResult(OutResult);

	if (!ExecuteDueTimerCallbacks(DueTimerHandles, OutResult))
	{
		OutResult.Metrics = Metrics;
		OutResult.bTickCalled = true;
		OutResult.TickCallCount = TickCallCount;
		CopyHostImportStateToResult(OutResult);
		CopyTimerStateToResult(OutResult);
		CopyEventStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	OutResult.Metrics = Metrics;
	CopyHostImportStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
	CopyEventStateToResult(OutResult);
	return true;
#endif
}

bool FAvidScriptWasmRuntimeInstance::DispatchEvent(
	int32 EventId,
	float Value,
	FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = bOwnsRuntimeLease;
	OutResult.bModuleLoaded = Module != nullptr;
	OutResult.bModuleInstantiated = ModuleInstance != nullptr;
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	OutResult.TickCallCount = TickCallCount;
	CopyHostImportStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
	CopyEventStateToResult(OutResult);

#if !AVIDSCRIPT_WITH_WAMR
	SetFailure(
		OutResult,
		ModuleId,
		TEXT("avid_on_event"),
		TEXT("backend_unavailable"),
		TEXT("WAMR backend is not available"),
		TEXT("build the ThirdParty WAMR static library before dispatching events"));
	return false;
#else
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
	static_assert(sizeof(EventArgs[1]) == sizeof(Value), "WAMR f32 argument must fit in one cell.");
	FMemory::Memcpy(&EventArgs[1], &Value, sizeof(Value));

	const double EventStartSeconds = FPlatformTime::Seconds();
	if (!CallWamrExport(
		static_cast<wasm_module_inst_t>(ModuleInstance),
		static_cast<wasm_exec_env_t>(ExecEnv),
		this,
		ModuleId,
		"avid_on_event",
		UE_ARRAY_COUNT(EventArgs),
		EventArgs,
		OutResult))
	{
		Metrics.EventCallbackCallMs = MeasureElapsedMs(EventStartSeconds);
		OutResult.Metrics = Metrics;
		CopyHostImportStateToResult(OutResult);
		CopyTimerStateToResult(OutResult);
		CopyEventStateToResult(OutResult);
		FAvidScriptLifecycleTransitionResult LifecycleResult;
		LifecycleState.MarkFaulted(LifecycleResult);
		return false;
	}

	Metrics.EventCallbackCallMs = MeasureElapsedMs(EventStartSeconds);
	++EventCallbackCount;
	LastEventId = EventId;
	LastEventValue = Value;
	OutResult.Metrics = Metrics;
	CopyHostImportStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
	CopyEventStateToResult(OutResult);
	return true;
#endif
}

bool FAvidScriptWasmRuntimeInstance::EndPlay(FAvidScriptWasmSmokeResult& OutResult)
{
	PrepareResult(OutResult, ModuleId, Metrics);
	OutResult.bRuntimeInitialized = bOwnsRuntimeLease;
	OutResult.bModuleLoaded = Module != nullptr;
	OutResult.bModuleInstantiated = ModuleInstance != nullptr;
	OutResult.bBeginPlayCalled = bHasBegunPlay;
	OutResult.bEndPlayCalled = bHasEndedPlay;
	OutResult.TickCallCount = TickCallCount;
	CopyHostImportStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
	CopyEventStateToResult(OutResult);

#if !AVIDSCRIPT_WITH_WAMR
	SetFailure(
		OutResult,
		ModuleId,
		TEXT("avid_on_end_play"),
		TEXT("backend_unavailable"),
		TEXT("WAMR backend is not available"),
		TEXT("build the ThirdParty WAMR static library before running scripts"));
	return false;
#else
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
	wasm_function_inst_t Function = wasm_runtime_lookup_function(
		static_cast<wasm_module_inst_t>(ModuleInstance),
		"avid_on_end_play");
	if (Function == nullptr)
	{
		Metrics.EndPlayCallMs = 0.0;
		OutResult.Metrics = Metrics;
		CopyHostImportStateToResult(OutResult);
		CopyTimerStateToResult(OutResult);
		CopyEventStateToResult(OutResult);
		bEndPlaySucceeded = true;
		LifecycleState.TryTransition(EAvidScriptLifecycleState::Stopped, LifecycleResult);
		CachedEndPlayResult = OutResult;
		return true;
	}

	const double EndPlayStartSeconds = FPlatformTime::Seconds();
	if (!CallWamrExport(
		static_cast<wasm_module_inst_t>(ModuleInstance),
		static_cast<wasm_exec_env_t>(ExecEnv),
		this,
		ModuleId,
		"avid_on_end_play",
		0,
		nullptr,
		OutResult))
	{
		Metrics.EndPlayCallMs = MeasureElapsedMs(EndPlayStartSeconds);
		OutResult.Metrics = Metrics;
		OutResult.bBeginPlayCalled = bHasBegunPlay;
		OutResult.bEndPlayCalled = false;
		OutResult.TickCallCount = TickCallCount;
		CopyHostImportStateToResult(OutResult);
		CopyTimerStateToResult(OutResult);
		CopyEventStateToResult(OutResult);
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
	CopyHostImportStateToResult(OutResult);
	CopyTimerStateToResult(OutResult);
	CopyEventStateToResult(OutResult);
	CachedEndPlayResult = OutResult;
	return true;
#endif
}

void FAvidScriptWasmRuntimeInstance::Unload()
{
	FAvidScriptWasmSmokeResult IgnoredResult;
	Unload(IgnoredResult);
}

void FAvidScriptWasmRuntimeInstance::Unload(FAvidScriptWasmSmokeResult& OutResult)
{
	const FString PreviousModuleId = ModuleId;
	const bool bWasRuntimeInitialized = bOwnsRuntimeLease;
	const bool bWasModuleLoaded = Module != nullptr;
	const bool bWasModuleInstantiated = ModuleInstance != nullptr;
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
	const bool bHadResources = bWasRuntimeInitialized || bWasModuleLoaded || bWasModuleInstantiated || ExecEnv != nullptr;
	const double UnloadStartSeconds = FPlatformTime::Seconds();

#if AVIDSCRIPT_WITH_WAMR
	if (ExecEnv != nullptr)
	{
		wasm_runtime_set_user_data(static_cast<wasm_exec_env_t>(ExecEnv), nullptr);
		wasm_runtime_destroy_exec_env(static_cast<wasm_exec_env_t>(ExecEnv));
		ExecEnv = nullptr;
	}

	if (ModuleInstance != nullptr)
	{
		wasm_runtime_deinstantiate(static_cast<wasm_module_inst_t>(ModuleInstance));
		ModuleInstance = nullptr;
	}

	if (Module != nullptr)
	{
		wasm_runtime_unload(static_cast<wasm_module_t>(Module));
		Module = nullptr;
	}

	if (bOwnsRuntimeLease)
	{
		ReleaseWamrRuntime();
		bOwnsRuntimeLease = false;
	}
#endif

	Module = nullptr;
	ModuleInstance = nullptr;
	ExecEnv = nullptr;
	ModuleBuffer.Empty();
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
	return Module != nullptr && ModuleInstance != nullptr && ExecEnv != nullptr;
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
	if (!FAvidScriptActorBinding::SetActorLocation(*HostContext.ObjectRegistry, ActorHandle, Location, HostContext.ActorWritePolicy, BindingResult))
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
	if (!FAvidScriptActorBinding::AddActorLocationOffset(*HostContext.ObjectRegistry, ActorHandle, Offset, HostContext.ActorWritePolicy, BindingResult))
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
	if (!FAvidScriptActorBinding::SetActorRotation(*HostContext.ObjectRegistry, ActorHandle, Rotation, HostContext.ActorWritePolicy, BindingResult))
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
	if (!FAvidScriptActorBinding::SetActorScale3D(*HostContext.ObjectRegistry, ActorHandle, Scale3D, HostContext.ActorWritePolicy, BindingResult))
	{
		SetPendingHostImportFailure(TEXT("avidscript"), TEXT("actor_set_scale"), BindingResult.ErrorMessage.IsEmpty() ? FString::Printf(TEXT("Actor scale write failed | slot=%d | generation=%d"), Slot, Generation) : BindingResult.ErrorMessage);
		Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
		return 0;
	}

	LastHostImportResult = 1;
	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return 1;
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
	if (!FAvidScriptSceneComponentBinding::SetWorldLocation(*HostContext.ObjectRegistry, ComponentHandle, WorldLocation, HostContext.ActorWritePolicy, BindingResult))
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
		|| PendingTimers.Num() >= AvidScriptMaximumPendingTimers)
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

	FAvidScriptWasmTimerEntry& Timer = PendingTimers.AddDefaulted_GetRef();
	Timer.Handle = TimerHandle;
	Timer.CallbackId = CallbackId;
	Timer.RemainingSeconds = DelaySeconds;
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

	const int32 TimerIndex = PendingTimers.IndexOfByPredicate(
		[TimerHandle](const FAvidScriptWasmTimerEntry& Timer)
		{
			return Timer.Handle == TimerHandle;
		});
	if (TimerIndex != INDEX_NONE)
	{
		PendingTimers.RemoveAtSwap(TimerIndex, 1, EAllowShrinking::No);
		LastHostImportResult = 1;
	}

	Metrics.HostImportCallMs = MeasureElapsedMs(HostImportStartSeconds);
	return LastHostImportResult;
}

void FAvidScriptWasmRuntimeInstance::CollectDueTimerHandles(float DeltaSeconds, TArray<int32>& OutDueTimerHandles)
{
	OutDueTimerHandles.Reset();
	const float SafeDeltaSeconds = FMath::IsFinite(DeltaSeconds) && DeltaSeconds > 0.0f
		? DeltaSeconds
		: 0.0f;
	for (FAvidScriptWasmTimerEntry& Timer : PendingTimers)
	{
		Timer.RemainingSeconds -= SafeDeltaSeconds;
		if (Timer.RemainingSeconds <= 0.0f)
		{
			OutDueTimerHandles.Add(Timer.Handle);
		}
	}
	OutDueTimerHandles.Sort();
}

bool FAvidScriptWasmRuntimeInstance::ExecuteDueTimerCallbacks(
	const TArray<int32>& DueTimerHandles,
	FAvidScriptWasmSmokeResult& OutResult)
{
#if !AVIDSCRIPT_WITH_WAMR
	return DueTimerHandles.IsEmpty();
#else
	for (const int32 TimerHandle : DueTimerHandles)
	{
		const int32 TimerIndex = PendingTimers.IndexOfByPredicate(
			[TimerHandle](const FAvidScriptWasmTimerEntry& Timer)
			{
				return Timer.Handle == TimerHandle;
			});
		if (TimerIndex == INDEX_NONE)
		{
			continue;
		}

		const FAvidScriptWasmTimerEntry Timer = PendingTimers[TimerIndex];
		PendingTimers.RemoveAtSwap(TimerIndex, 1, EAllowShrinking::No);

		uint32 TimerArgs[2] = {
			static_cast<uint32>(Timer.CallbackId),
			static_cast<uint32>(Timer.Handle)
		};
		const double CallbackStartSeconds = FPlatformTime::Seconds();
		if (!CallWamrExport(
			static_cast<wasm_module_inst_t>(ModuleInstance),
			static_cast<wasm_exec_env_t>(ExecEnv),
			this,
			ModuleId,
			"avid_on_timer",
			UE_ARRAY_COUNT(TimerArgs),
			TimerArgs,
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
#endif
}

int32 FAvidScriptWasmRuntimeInstance::AllocateTimerHandle()
{
	for (int32 Attempt = 0; Attempt <= AvidScriptMaximumPendingTimers; ++Attempt)
	{
		const int32 Candidate = NextTimerHandle;
		NextTimerHandle = NextTimerHandle == MAX_int32 ? 1 : NextTimerHandle + 1;
		const bool bAlreadyUsed = PendingTimers.ContainsByPredicate(
			[Candidate](const FAvidScriptWasmTimerEntry& Timer)
			{
				return Timer.Handle == Candidate;
			});
		if (Candidate > 0 && !bAlreadyUsed)
		{
			return Candidate;
		}
	}
	return 0;
}

void FAvidScriptWasmRuntimeInstance::ResetTimerState()
{
	PendingTimers.Reset();
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
