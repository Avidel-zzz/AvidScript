#pragma once

#include "AvidScriptVmTypedHostImport.h"
#include "CoreMinimal.h"

enum class EAvidScriptVmBackendKind : uint8
{
	Wamr,
	Wasmtime
};

enum class EAvidScriptVmExecutionMode : uint8
{
	Auto,
	Interpreter,
	Aot,
	Jit
};

enum class EAvidScriptVmArtifactFormat : uint8
{
	WasmBytecode,
	WamrAot,
	WasmtimeSerialized
};

enum class EAvidScriptVmArtifactTrust : uint8
{
	Untrusted,
	VerifiedPackage
};

enum class EAvidScriptVmCapability : uint32
{
	None = 0,
	GuestMemory = 1 << 0,
	Interpreter = 1 << 1,
	Aot = 1 << 2,
	Jit = 1 << 3,
	PrecompiledArtifact = 1 << 4,
	StructuredStack = 1 << 5
};
ENUM_CLASS_FLAGS(EAvidScriptVmCapability);

struct FAvidScriptVmBackendInfo
{
	EAvidScriptVmBackendKind Kind = EAvidScriptVmBackendKind::Wamr;
	EAvidScriptVmExecutionMode ExecutionMode = EAvidScriptVmExecutionMode::Interpreter;
	EAvidScriptVmArtifactFormat ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	EAvidScriptVmCapability Capabilities = EAvidScriptVmCapability::None;
	FString StableBackendId;
	FString RuntimeVersion;
	FString RuntimeBuildIdentity;
	FString RuntimeArtifactSha256;
	FString TargetTriple;
};

struct FAvidScriptVmBackendSelection
{
	EAvidScriptVmBackendKind BackendKind = EAvidScriptVmBackendKind::Wamr;
	EAvidScriptVmExecutionMode ExecutionMode = EAvidScriptVmExecutionMode::Interpreter;
	EAvidScriptVmArtifactFormat ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	bool bAllowFallback = false;
};

struct FAvidScriptVmArtifactView
{
	TArrayView<const uint8> ExecutionBytes;
	EAvidScriptVmArtifactFormat ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	TArrayView<const uint8> CanonicalWasmBytes;
	FString ExecutionIdentity;
	FString CanonicalWasmIdentity;
	FString CompilerBuildIdentity;
	FString TargetTriple;
	EAvidScriptVmArtifactTrust Trust = EAvidScriptVmArtifactTrust::Untrusted;

	static FAvidScriptVmArtifactView FromWasmBytecode(
		TArrayView<const uint8> WasmBytes,
		const FString& WasmIdentity = FString())
	{
		FAvidScriptVmArtifactView View;
		View.ExecutionBytes = WasmBytes;
		View.CanonicalWasmBytes = WasmBytes;
		View.ExecutionIdentity = WasmIdentity;
		View.CanonicalWasmIdentity = WasmIdentity;
		return View;
	}

	static FAvidScriptVmArtifactView FromWasmtimeSerialized(
		TArrayView<const uint8> SerializedBytes,
		TArrayView<const uint8> WasmBytes,
		const FString& SerializedIdentity,
		const FString& WasmIdentity,
		const FString& InCompilerBuildIdentity,
		const FString& InTargetTriple,
		EAvidScriptVmArtifactTrust InTrust)
	{
		FAvidScriptVmArtifactView View;
		View.ExecutionBytes = SerializedBytes;
		View.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmtimeSerialized;
		View.CanonicalWasmBytes = WasmBytes;
		View.ExecutionIdentity = SerializedIdentity;
		View.CanonicalWasmIdentity = WasmIdentity;
		View.CompilerBuildIdentity = InCompilerBuildIdentity;
		View.TargetTriple = InTargetTriple;
		View.Trust = InTrust;
		return View;
	}
};

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
	TimerCancel,
	EventSubscribe,
	EventUnsubscribe,
	DataLaneGetEpoch,
	DataLaneSubmit,
	ValueArrayLength,
	ValueArrayLoad,
	ValueArrayStore,
	ValueArrayReadRange,
	ValueArrayWriteRange,
	ValueRelease
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
	uint64 BackendInstanceIdentity = 0;

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

struct FAvidScriptVmCallResult
{
	static constexpr uint32 MaxCells = 8;

	uint32 Cells[MaxCells] = {};
	uint32 CellCount = 0;
};

using FAvidScriptVmPreparedExportInvoke = bool (*)(
	void* Owner,
	void* Target,
	const FAvidScriptVmCallFrame& Frame,
	FAvidScriptVmError& OutError,
	FAvidScriptVmCallResult* OutResult);

struct FAvidScriptVmPreparedExportCall
{
	void* Owner = nullptr;
	void* Target = nullptr;
	FAvidScriptVmPreparedExportInvoke InvokeFunction = nullptr;
	uint32 ParameterCellCount = 0;
	uint32 ResultCellCount = 0;

	bool IsValid() const
	{
		return Owner != nullptr
			&& Target != nullptr
			&& InvokeFunction != nullptr
			&& ParameterCellCount <= FAvidScriptVmCallFrame::MaxCells
			&& ResultCellCount <= FAvidScriptVmCallResult::MaxCells;
	}

	bool Call(
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult = nullptr) const
	{
		if (!IsValid())
		{
			OutError.Reset();
			OutError.Category = TEXT("prepared_export_invalid");
			OutError.Details =
				TEXT("The prepared VM export call is not initialized.");
			if (OutResult != nullptr)
			{
				*OutResult = FAvidScriptVmCallResult();
			}
			return false;
		}
		if (Frame.CellCount != ParameterCellCount)
		{
			OutError.Reset();
			OutError.Category = TEXT("invalid_arguments");
			OutError.Details =
				TEXT("The VM call frame does not match the prepared export signature.");
			if (OutResult != nullptr)
			{
				*OutResult = FAvidScriptVmCallResult();
			}
			return false;
		}
		return InvokeFunction(Owner, Target, Frame, OutError, OutResult);
	}
};

struct FAvidScriptHostCall
{
	EAvidScriptHostBindingId BindingId = EAvidScriptHostBindingId::Invalid;
	int32 IntArgs[4] = {};
	int64 Int64Args[2] = {};
	float FloatArgs[4] = {};
	uint32 GuestAddress = 0;

	// Views borrow validated guest memory and are valid only during synchronous dispatch.
	TConstArrayView<uint32> InputCells;
	TConstArrayView<uint8> InputBytes;
	TArrayView<uint8> OutputBytes;
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
	virtual bool BorrowReadOnlyBytes(
		uint32 GuestAddress,
		uint32 ByteCount,
		uint32 Alignment,
		TConstArrayView<uint8>& OutBytes,
		FString& OutError)
	{
		OutBytes = TConstArrayView<uint8>();
		OutError = TEXT("guest_memory_borrow_unavailable: this guest memory provider does not support synchronous borrowing.");
		return false;
	}
	virtual bool BorrowMutableBytes(
		uint32 GuestAddress,
		uint32 ByteCount,
		uint32 Alignment,
		TArrayView<uint8>& OutBytes,
		FString& OutError)
	{
		OutBytes = TArrayView<uint8>();
		OutError = TEXT("guest_memory_borrow_unavailable: this guest memory provider does not support synchronous borrowing.");
		return false;
	}
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
	int64 ReturnValueI64 = 0;
	FString Details;
};

using FAvidScriptVmPreparedDynamicHostInvoke =
	bool (*)(
		void* Context,
		TConstArrayView<uint64> Arguments,
		IAvidScriptVmGuestMemory& GuestMemory,
		FAvidScriptDynamicHostCallResult& OutResult);

struct FAvidScriptVmPreparedDynamicHostTarget
{
	void* Context = nullptr;
	FAvidScriptVmPreparedDynamicHostInvoke Invoke = nullptr;

	bool IsBound() const
	{
		return Context != nullptr && Invoke != nullptr;
	}

	bool IsEmpty() const
	{
		return Context == nullptr && Invoke == nullptr;
	}
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

enum class EAvidScriptVmValueKind : uint8
{
	I32,
	I64,
	F32,
	F64
};

struct FAvidScriptVmAbiSignature
{
	TArray<EAvidScriptVmValueKind> Parameters;
	bool bHasResult = false;
	EAvidScriptVmValueKind Result = EAvidScriptVmValueKind::I32;
};

AVIDSCRIPTVM_API bool ParseAvidScriptVmAbiSignature(
	const FString& CompactSignature,
	FAvidScriptVmAbiSignature& OutSignature,
	FString& OutError);

struct FAvidScriptVmDynamicImport
{
	FString StableId;
	uint32 Ordinal = MAX_uint32;
	FString ModuleName;
	FString ImportName;
	FString Signature;
	FAvidScriptVmPreparedDynamicHostTarget PreparedTarget;
};

struct FAvidScriptVmBindingPackage
{
	FString PackageName;
	FString PackageHash;
	TArray<FAvidScriptVmDynamicImport> Imports;
};

struct FAvidScriptVmExpectedImport
{
	FString ModuleName;
	FString ImportName;
};

struct FAvidScriptWasmModuleLayout;

AVIDSCRIPTVM_API bool IsAvidScriptVmStaticHostImport(
	const FString& ModuleName,
	const FString& ImportName);

AVIDSCRIPTVM_API bool ValidateAvidScriptVmImportContract(
	const FAvidScriptWasmModuleLayout& ActualLayout,
	const FAvidScriptVmBindingPackage* BindingPackage,
	TConstArrayView<FAvidScriptVmExpectedImport> ExpectedImports,
	bool bEnforceExpectedImports,
	FAvidScriptVmError& OutError);

struct FAvidScriptVmLoadConfig
{
	uint32 StackSize = 64 * 1024;
	uint32 HeapSize = 64 * 1024;
	IAvidScriptHostDispatcher* HostDispatcher = nullptr;
	const FAvidScriptVmBindingPackage* BindingPackage = nullptr;
	IAvidScriptVmTypedHostDispatcher* TypedHostDispatcher = nullptr;
	TConstArrayView<FAvidScriptVmTypedHostImport> TypedHostImports;
};

AVIDSCRIPTVM_API TUniquePtr<class IAvidScriptVmBackend> CreateAvidScriptWamrBackend();
AVIDSCRIPTVM_API TUniquePtr<class IAvidScriptVmBackend> CreateAvidScriptWasmtimeBackend(
	EAvidScriptVmArtifactFormat ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode);
AVIDSCRIPTVM_API TUniquePtr<class IAvidScriptVmBackend> CreateAvidScriptVmBackend(
	const FAvidScriptVmBackendSelection& Selection,
	FAvidScriptVmError& OutError);

class AVIDSCRIPTVM_API IAvidScriptVmBackend
{
public:
	virtual ~IAvidScriptVmBackend() = default;

	virtual const FAvidScriptVmBackendInfo& GetBackendInfo() const = 0;
	virtual bool Load(
		TArrayView<const uint8> Bytecode,
		const FString& ModuleId,
		const FAvidScriptVmLoadConfig& Config,
		FAvidScriptVmError& OutError) = 0;
	virtual bool LoadArtifact(
		const FAvidScriptVmArtifactView& Artifact,
		const FString& ModuleId,
		const FAvidScriptVmLoadConfig& Config,
		FAvidScriptVmError& OutError)
	{
		if (Artifact.ArtifactFormat != EAvidScriptVmArtifactFormat::WasmBytecode)
		{
			OutError.Reset();
			OutError.Category = TEXT("artifact_format_unavailable");
			OutError.Details = TEXT("This VM backend does not support the requested precompiled artifact format.");
			return false;
		}
		if (Artifact.ExecutionBytes.Num() != Artifact.CanonicalWasmBytes.Num()
			|| (Artifact.ExecutionBytes.Num() > 0
				&& FMemory::Memcmp(
					Artifact.ExecutionBytes.GetData(),
					Artifact.CanonicalWasmBytes.GetData(),
					Artifact.ExecutionBytes.Num()) != 0))
		{
			OutError.Reset();
			OutError.Category = TEXT("artifact_identity_mismatch");
			OutError.Details = TEXT("WASM execution bytes must match the canonical bytes that were validated.");
			return false;
		}
		return Load(Artifact.CanonicalWasmBytes, ModuleId, Config, OutError);
	}
	virtual bool ResolveExport(
		const FString& ExportName,
		FAvidScriptVmExportHandle& OutHandle,
		FAvidScriptVmError& OutError) = 0;
	virtual bool PrepareExportCall(
		const FAvidScriptVmExportHandle& Handle,
		FAvidScriptVmPreparedExportCall& OutCall,
		FAvidScriptVmError& OutError) = 0;
	virtual bool Call(
		const FAvidScriptVmExportHandle& Handle,
		const FAvidScriptVmCallFrame& Frame,
		FAvidScriptVmError& OutError,
		FAvidScriptVmCallResult* OutResult = nullptr) = 0;
	virtual void Unload() = 0;
	virtual bool IsLoaded() const = 0;
	virtual IAvidScriptVmGuestMemory* GetGuestMemory() { return nullptr; }
	virtual uint32 GetExportLookupCount() const = 0;
	virtual const FAvidScriptVmLoadMetrics& GetLoadMetrics() const = 0;
};
