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
	ActorGetTransformBatch,
	ActorGetRootComponent,
	SceneComponentGetWorldLocation,
	SceneComponentSetWorldLocation,
	OwnerGetSlot,
	OwnerGetGeneration,
	OwnerGetHandle,
	TimerSetOnce,
	TimerCancel
};

struct FAvidScriptVmStackFrame
{
	uint32 FunctionIndex = MAX_uint32;
	uint32 FunctionOffset = 0;
	FString RawFunctionToken;
};

struct FAvidScriptVmError
{
	FString Category;
	FString Details;
	FString ImportModuleName;
	FString ImportName;
	TArray<FAvidScriptVmStackFrame> StackFrames;

	void Reset()
	{
		Category.Reset();
		Details.Reset();
		ImportModuleName.Reset();
		ImportName.Reset();
		StackFrames.Reset();
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

	// Views borrow validated guest memory and are valid only during synchronous dispatch.
	TConstArrayView<uint32> InputCells;
	TArrayView<float> OutputFloats;
};

struct FAvidScriptHostCallResult
{
	bool bSucceeded = false;
	int32 ReturnValue = 0;
	int64 ReturnValueI64 = 0;
	uint32 IntValues[2] = {};
	float FloatValues[3] = {};
	FString Details;
};

class AVIDSCRIPTVM_API IAvidScriptVmGuestMemory
{
public:
	IAvidScriptVmGuestMemory();
	virtual ~IAvidScriptVmGuestMemory();
	virtual bool ReadBytes(
		uint32 GuestAddress,
		TArrayView<uint8> OutBytes,
		FString& OutError) = 0;
	virtual bool WriteBytes(
		uint32 GuestAddress,
		TConstArrayView<uint8> Bytes,
		FString& OutError) = 0;
};

struct FAvidScriptDynamicHostCall
{
	uint32 BindingOrdinal = MAX_uint32;
	TConstArrayView<uint64> Arguments;
	IAvidScriptVmGuestMemory* GuestMemory = nullptr;
};

struct FAvidScriptDynamicHostCallResult
{
	bool bSucceeded = false;
	int32 ReturnValue = 0;
	FString Details;
};

class AVIDSCRIPTVM_API IAvidScriptHostDispatcher
{
public:
	virtual ~IAvidScriptHostDispatcher() = default;
	virtual bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) = 0;
	virtual bool DispatchDynamicHostCall(
		const FAvidScriptDynamicHostCall& Call,
		FAvidScriptDynamicHostCallResult& OutResult)
	{
		OutResult = FAvidScriptDynamicHostCallResult();
		OutResult.Details = TEXT("Dynamic host calls are not supported by this dispatcher.");
		return false;
	}
};

struct FAvidScriptVmLoadMetrics
{
	double RuntimeInitMs = 0.0;
	double ModuleLoadMs = 0.0;
	double ModuleInstantiateMs = 0.0;
	double ExecEnvCreateMs = 0.0;
};

struct FAvidScriptVmDynamicImport
{
	FString StableId;
	uint32 Ordinal = MAX_uint32;
	FString ModuleName;
	FString ImportName;
	FString Signature;
};

struct FAvidScriptVmBindingPackage
{
	FString PackageName;
	FString PackageHash;
	TArray<FAvidScriptVmDynamicImport> Imports;
};

AVIDSCRIPTVM_API bool IsAvidScriptVmStaticHostImport(
	const FString& ModuleName,
	const FString& ImportName);

struct FAvidScriptVmLoadConfig
{
	uint32 StackSize = 64 * 1024;
	uint32 HeapSize = 64 * 1024;
	IAvidScriptHostDispatcher* HostDispatcher = nullptr;
	const FAvidScriptVmBindingPackage* BindingPackage = nullptr;
};

AVIDSCRIPTVM_API TUniquePtr<class IAvidScriptVmBackend> CreateAvidScriptWamrBackend();

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
	virtual IAvidScriptVmGuestMemory* GetGuestMemory() { return nullptr; }
	virtual uint32 GetExportLookupCount() const = 0;
	virtual const FAvidScriptVmLoadMetrics& GetLoadMetrics() const = 0;
};
