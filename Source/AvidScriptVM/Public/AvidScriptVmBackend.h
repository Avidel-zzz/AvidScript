#pragma once

#include "CoreMinimal.h"

enum class EAvidScriptHostBindingId : uint16
{
	Invalid = 0,
	HostAddI32,
	HostFailI32,
	ActorGetLocation,
	ActorSetLocation,
	ActorAddLocationOffset,
	ActorGetRotation,
	ActorSetRotation,
	ActorGetScale,
	ActorSetScale,
	ActorGetRootComponent,
	SceneComponentGetWorldLocation,
	SceneComponentSetWorldLocation,
	OwnerGetSlot,
	OwnerGetGeneration,
	TimerSetOnce,
	TimerCancel
};

struct FAvidScriptVmError
{
	FString Category;
	FString Details;
	FString ImportModuleName;
	FString ImportName;

	void Reset()
	{
		Category.Reset();
		Details.Reset();
		ImportModuleName.Reset();
		ImportName.Reset();
	}
};

struct FAvidScriptVmExportHandle
{
	uint32 Slot = 0;
	uint32 Generation = 0;

	bool IsValid() const
	{
		return Slot != 0 && Generation != 0;
	}
};

struct FAvidScriptVmCallFrame
{
	static constexpr uint32 MaxCells = 8;

	uint32 Cells[MaxCells] = {};
	uint32 CellCount = 0;
};

struct FAvidScriptHostCall
{
	EAvidScriptHostBindingId BindingId = EAvidScriptHostBindingId::Invalid;
	int32 IntArgs[4] = {};
	float FloatArgs[4] = {};
	uint32 GuestAddress = 0;
};

struct FAvidScriptHostCallResult
{
	bool bSucceeded = false;
	int32 ReturnValue = 0;
	int32 IntValues[2] = {};
	float FloatValues[3] = {};
	FString Details;
};

class AVIDSCRIPTVM_API IAvidScriptHostDispatcher
{
public:
	virtual ~IAvidScriptHostDispatcher() = default;
	virtual bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) = 0;
};

struct FAvidScriptVmLoadConfig
{
	uint32 StackSize = 64 * 1024;
	uint32 HeapSize = 64 * 1024;
	IAvidScriptHostDispatcher* HostDispatcher = nullptr;
};

class AVIDSCRIPTVM_API IAvidScriptVmBackend
{
public:
	virtual ~IAvidScriptVmBackend() = default;

	virtual bool Load(
		TArrayView<const uint8> Bytecode,
		const FString& ModuleId,
		const FAvidScriptVmLoadConfig& Config,
		FAvidScriptVmError& OutError) = 0;
	virtual bool ResolveExport(
		const FString& ExportName,
		FAvidScriptVmExportHandle& OutHandle,
		FAvidScriptVmError& OutError) = 0;
	virtual bool Call(
		const FAvidScriptVmExportHandle& Handle,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError) = 0;
	virtual void Unload() = 0;
	virtual bool IsLoaded() const = 0;
	virtual uint32 GetExportLookupCount() const = 0;
};
