#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"

#include "Misc/AutomationTest.h"

#ifndef AVIDSCRIPT_WITH_WASMTIME
#define AVIDSCRIPT_WITH_WASMTIME 0
#endif

namespace
{
constexpr const TCHAR* WasmtimeDynamicImportName = TEXT("avid_ue_1111111111111111");

void AppendWasmtimeU32Leb(TArray<uint8>& Bytes, uint32 Value)
{
	do
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		if (Value != 0)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	} while (Value != 0);
}

void AppendWasmtimeI32Leb(TArray<uint8>& Bytes, int32 Value)
{
	bool bMore = true;
	while (bMore)
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		const bool bSignBitSet = (Byte & 0x40) != 0;
		bMore = !((Value == 0 && !bSignBitSet) || (Value == -1 && bSignBitSet));
		if (bMore)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	}
}

void AppendWasmtimeString(TArray<uint8>& Bytes, const char* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	AppendWasmtimeU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value[Index]));
	}
}

void AppendWasmtimeSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendWasmtimeU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendWasmtimeI32Const(TArray<uint8>& Body, int32 Value)
{
	Body.Add(0x41);
	AppendWasmtimeI32Leb(Body, Value);
}

TArray<uint8> MakeWasmtimeModule()
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));
	return Module;
}

TArray<uint8> BuildWasmtimeLifecycleFixture()
{
	TArray<uint8> Module = MakeWasmtimeModule();

	TArray<uint8> Types;
	AppendWasmtimeU32Leb(Types, 2);
	const uint8 EmptyType[] = { 0x60, 0x00, 0x00 };
	Types.Append(EmptyType, UE_ARRAY_COUNT(EmptyType));
	const uint8 TickType[] = { 0x60, 0x01, 0x7d, 0x00 };
	Types.Append(TickType, UE_ARRAY_COUNT(TickType));
	AppendWasmtimeSection(Module, 1, Types);

	TArray<uint8> Functions;
	AppendWasmtimeU32Leb(Functions, 3);
	AppendWasmtimeU32Leb(Functions, 0);
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeU32Leb(Functions, 0);
	AppendWasmtimeSection(Module, 3, Functions);

	TArray<uint8> Memory;
	AppendWasmtimeU32Leb(Memory, 1);
	Memory.Add(0x00);
	AppendWasmtimeU32Leb(Memory, 1);
	AppendWasmtimeSection(Module, 5, Memory);

	TArray<uint8> Exports;
	AppendWasmtimeU32Leb(Exports, 4);
	AppendWasmtimeString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 0);
	AppendWasmtimeString(Exports, "avid_on_tick");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 1);
	AppendWasmtimeString(Exports, "avid_trap");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 2);
	AppendWasmtimeString(Exports, "memory");
	Exports.Add(0x02);
	AppendWasmtimeU32Leb(Exports, 0);
	AppendWasmtimeSection(Module, 7, Exports);

	TArray<uint8> Code;
	AppendWasmtimeU32Leb(Code, 3);
	const TArray<uint8> EmptyBody = { 0x00, 0x0b };
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(EmptyBody.Num()));
	Code.Append(EmptyBody);
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(EmptyBody.Num()));
	Code.Append(EmptyBody);
	const TArray<uint8> TrapBody = { 0x00, 0x00, 0x0b };
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(TrapBody.Num()));
	Code.Append(TrapBody);
	AppendWasmtimeSection(Module, 10, Code);
	return Module;
}

TArray<uint8> BuildWasmtimeResultFixture(
	const uint8 ResultType,
	const int32 ResultCount)
{
	TArray<uint8> Module = MakeWasmtimeModule();

	TArray<uint8> Types;
	AppendWasmtimeU32Leb(Types, 1);
	Types.Add(0x60);
	Types.Add(0x00);
	AppendWasmtimeU32Leb(Types, static_cast<uint32>(ResultCount));
	for (int32 Index = 0; Index < ResultCount; ++Index)
	{
		Types.Add(ResultType);
	}
	AppendWasmtimeSection(Module, 1, Types);

	TArray<uint8> Functions;
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeU32Leb(Functions, 0);
	AppendWasmtimeSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendWasmtimeU32Leb(Exports, 1);
	AppendWasmtimeString(Exports, "result_test");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 0);
	AppendWasmtimeSection(Module, 7, Exports);

	TArray<uint8> Body;
	Body.Add(0x00);
	for (int32 Index = 0; Index < ResultCount; ++Index)
	{
		if (ResultType == 0x7f)
		{
			AppendWasmtimeI32Const(Body, Index + 7);
		}
		else
		{
			Body.Add(0xd0);
			Body.Add(ResultType);
		}
	}
	Body.Add(0x0b);
	TArray<uint8> Code;
	AppendWasmtimeU32Leb(Code, 1);
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(Body.Num()));
	Code.Append(Body);
	AppendWasmtimeSection(Module, 10, Code);
	return Module;
}

TArray<uint8> BuildWasmtimeI32ImportFixture(const char* ImportName, int32 Input)
{
	TArray<uint8> Module = MakeWasmtimeModule();

	TArray<uint8> Types;
	AppendWasmtimeU32Leb(Types, 2);
	const uint8 ImportType[] = { 0x60, 0x01, 0x7f, 0x01, 0x7f };
	Types.Append(ImportType, UE_ARRAY_COUNT(ImportType));
	const uint8 EmptyType[] = { 0x60, 0x00, 0x00 };
	Types.Append(EmptyType, UE_ARRAY_COUNT(EmptyType));
	AppendWasmtimeSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendWasmtimeU32Leb(Imports, 1);
	AppendWasmtimeString(Imports, "avidscript");
	AppendWasmtimeString(Imports, ImportName);
	Imports.Add(0x00);
	AppendWasmtimeU32Leb(Imports, 0);
	AppendWasmtimeSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendWasmtimeU32Leb(Exports, 1);
	AppendWasmtimeString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 1);
	AppendWasmtimeSection(Module, 7, Exports);

	TArray<uint8> Body;
	Body.Add(0x00);
	AppendWasmtimeI32Const(Body, Input);
	Body.Add(0x10);
	AppendWasmtimeU32Leb(Body, 0);
	Body.Add(0x1a);
	Body.Add(0x0b);
	TArray<uint8> Code;
	AppendWasmtimeU32Leb(Code, 1);
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(Body.Num()));
	Code.Append(Body);
	AppendWasmtimeSection(Module, 10, Code);
	return Module;
}

FAvidScriptVmBindingPackage MakeWasmtimeDynamicPackage(uint32 TargetOrdinal, TCHAR HashCharacter)
{
	FAvidScriptVmBindingPackage Package;
	Package.PackageName = TEXT("avidscript.phase54.wasmtime_dynamic");
	Package.PackageHash = FString::ChrN(64, HashCharacter);
	if (TargetOrdinal > 0)
	{
		FAvidScriptVmDynamicImport Padding;
		Padding.StableId = TEXT("2222222222222222222222222222222222222222222222222222222222222222");
		Padding.Ordinal = 0;
		Padding.ModuleName = TEXT("avidscript");
		Padding.ImportName = TEXT("avid_ue_2222222222222222");
		Padding.Signature = TEXT("(i)i");
		Package.Imports.Add(MoveTemp(Padding));
	}

	FAvidScriptVmDynamicImport Import;
	Import.StableId = TEXT("1111111111111111111111111111111111111111111111111111111111111111");
	Import.Ordinal = TargetOrdinal;
	Import.ModuleName = TEXT("avidscript");
	Import.ImportName = WasmtimeDynamicImportName;
	Import.Signature = TEXT("(i)i");
	Package.Imports.Add(MoveTemp(Import));
	return Package;
}

TArray<uint8> BuildWasmtimeVectorFixture()
{
	TArray<uint8> Module = MakeWasmtimeModule();

	TArray<uint8> Types;
	AppendWasmtimeU32Leb(Types, 3);
	const uint8 VectorReadType[] = { 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f };
	Types.Append(VectorReadType, UE_ARRAY_COUNT(VectorReadType));
	const uint8 I32Type[] = { 0x60, 0x01, 0x7f, 0x01, 0x7f };
	Types.Append(I32Type, UE_ARRAY_COUNT(I32Type));
	const uint8 EmptyType[] = { 0x60, 0x00, 0x00 };
	Types.Append(EmptyType, UE_ARRAY_COUNT(EmptyType));
	AppendWasmtimeSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendWasmtimeU32Leb(Imports, 2);
	AppendWasmtimeString(Imports, "avidscript");
	AppendWasmtimeString(Imports, "actor_get_location");
	Imports.Add(0x00);
	AppendWasmtimeU32Leb(Imports, 0);
	AppendWasmtimeString(Imports, "env");
	AppendWasmtimeString(Imports, "host_add_i32");
	Imports.Add(0x00);
	AppendWasmtimeU32Leb(Imports, 1);
	AppendWasmtimeSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeU32Leb(Functions, 2);
	AppendWasmtimeSection(Module, 3, Functions);

	TArray<uint8> Memory;
	AppendWasmtimeU32Leb(Memory, 1);
	Memory.Add(0x00);
	AppendWasmtimeU32Leb(Memory, 1);
	AppendWasmtimeSection(Module, 5, Memory);

	TArray<uint8> Exports;
	AppendWasmtimeU32Leb(Exports, 2);
	AppendWasmtimeString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 2);
	AppendWasmtimeString(Exports, "memory");
	Exports.Add(0x02);
	AppendWasmtimeU32Leb(Exports, 0);
	AppendWasmtimeSection(Module, 7, Exports);

	TArray<uint8> Body;
	Body.Add(0x00);
	AppendWasmtimeI32Const(Body, 7);
	AppendWasmtimeI32Const(Body, 11);
	AppendWasmtimeI32Const(Body, 64);
	Body.Add(0x10);
	AppendWasmtimeU32Leb(Body, 0);
	Body.Add(0x1a);
	AppendWasmtimeI32Const(Body, 64);
	Body.Add(0x28);
	AppendWasmtimeU32Leb(Body, 2);
	AppendWasmtimeU32Leb(Body, 0);
	Body.Add(0x10);
	AppendWasmtimeU32Leb(Body, 1);
	Body.Add(0x1a);
	Body.Add(0x0b);
	TArray<uint8> Code;
	AppendWasmtimeU32Leb(Code, 1);
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(Body.Num()));
	Code.Append(Body);
	AppendWasmtimeSection(Module, 10, Code);
	return Module;
}

TArray<uint8> BuildWasmtimeBatchFixture()
{
	TArray<uint8> Module = MakeWasmtimeModule();

	TArray<uint8> Types;
	AppendWasmtimeU32Leb(Types, 3);
	const uint8 BatchType[] = { 0x60, 0x03, 0x7f, 0x7f, 0x7f, 0x01, 0x7f };
	Types.Append(BatchType, UE_ARRAY_COUNT(BatchType));
	const uint8 I32Type[] = { 0x60, 0x01, 0x7f, 0x01, 0x7f };
	Types.Append(I32Type, UE_ARRAY_COUNT(I32Type));
	const uint8 EmptyType[] = { 0x60, 0x00, 0x00 };
	Types.Append(EmptyType, UE_ARRAY_COUNT(EmptyType));
	AppendWasmtimeSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendWasmtimeU32Leb(Imports, 2);
	AppendWasmtimeString(Imports, "avidscript");
	AppendWasmtimeString(Imports, "actor_get_transform_batch");
	Imports.Add(0x00);
	AppendWasmtimeU32Leb(Imports, 0);
	AppendWasmtimeString(Imports, "avidscript");
	AppendWasmtimeString(Imports, "host_add_i32");
	Imports.Add(0x00);
	AppendWasmtimeU32Leb(Imports, 1);
	AppendWasmtimeSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeU32Leb(Functions, 2);
	AppendWasmtimeSection(Module, 3, Functions);

	TArray<uint8> Memory;
	AppendWasmtimeU32Leb(Memory, 1);
	Memory.Add(0x00);
	AppendWasmtimeU32Leb(Memory, 1);
	AppendWasmtimeSection(Module, 5, Memory);

	TArray<uint8> Exports;
	AppendWasmtimeU32Leb(Exports, 2);
	AppendWasmtimeString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 2);
	AppendWasmtimeString(Exports, "memory");
	Exports.Add(0x02);
	AppendWasmtimeU32Leb(Exports, 0);
	AppendWasmtimeSection(Module, 7, Exports);

	TArray<uint8> Body;
	Body.Add(0x00);
	AppendWasmtimeI32Const(Body, 64);
	AppendWasmtimeI32Const(Body, 2);
	AppendWasmtimeI32Const(Body, 128);
	Body.Add(0x10);
	AppendWasmtimeU32Leb(Body, 0);
	Body.Add(0x1a);
	AppendWasmtimeI32Const(Body, 128);
	Body.Add(0x28);
	AppendWasmtimeU32Leb(Body, 2);
	AppendWasmtimeU32Leb(Body, 0);
	Body.Add(0x10);
	AppendWasmtimeU32Leb(Body, 1);
	Body.Add(0x1a);
	Body.Add(0x0b);
	TArray<uint8> Code;
	AppendWasmtimeU32Leb(Code, 1);
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(Body.Num()));
	Code.Append(Body);
	AppendWasmtimeSection(Module, 10, Code);

	const uint32 InputCells[] = { 7u, 11u, 13u, 17u };
	TArray<uint8> Data;
	AppendWasmtimeU32Leb(Data, 1);
	Data.Add(0x00);
	AppendWasmtimeI32Const(Data, 64);
	Data.Add(0x0b);
	AppendWasmtimeU32Leb(Data, sizeof(InputCells));
	Data.Append(reinterpret_cast<const uint8*>(InputCells), sizeof(InputCells));
	AppendWasmtimeSection(Module, 11, Data);
	return Module;
}

TUniquePtr<IAvidScriptVmBackend> CreateWasmtimeBackendForTest(FAvidScriptVmError& OutError)
{
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	return CreateAvidScriptVmBackend(Selection, OutError);
}

bool LoadWasmtimeTestModule(
	FAutomationTestBase& Test,
	IAvidScriptVmBackend& Backend,
	TConstArrayView<uint8> Bytecode,
	const FAvidScriptVmLoadConfig& Config,
	FAvidScriptVmError& OutError)
{
	if (!Test.TestTrue(TEXT("Wasmtime fixture loads"), Backend.Load(Bytecode, TEXT("wasmtime_test"), Config, OutError)))
	{
		Test.AddError(OutError.Category + TEXT(": ") + OutError.Details);
		return false;
	}
	return true;
}

class FAvidScriptWasmtimeTestDispatcher final : public IAvidScriptHostDispatcher
{
public:
	bool DispatchHostCall(const FAvidScriptHostCall& Call, FAvidScriptHostCallResult& OutResult) override
	{
		++CallCount;
		LastBindingId = Call.BindingId;
		if (Call.BindingId == EAvidScriptHostBindingId::HostAddI32)
		{
			LastI32 = Call.IntArgs[0];
			OutResult.bSucceeded = true;
			OutResult.ReturnValue = Call.IntArgs[0] + 1;
		}
		else if (Call.BindingId == EAvidScriptHostBindingId::HostFailI32)
		{
			OutResult.Details = TEXT("wasmtime host failure sentinel");
		}
		else if (Call.BindingId == EAvidScriptHostBindingId::ActorGetLocation)
		{
			OutResult.bSucceeded = true;
			OutResult.ReturnValue = 1;
			OutResult.FloatValues[0] = 42.0f;
			OutResult.FloatValues[1] = 43.0f;
			OutResult.FloatValues[2] = 44.0f;
		}
		else if (Call.BindingId == EAvidScriptHostBindingId::ActorGetTransformBatch)
		{
			CapturedInputCells.Reset();
			CapturedInputCells.Append(Call.InputCells.GetData(), Call.InputCells.Num());
			CapturedOutputFloatCount = Call.OutputFloats.Num();
			if (!Call.OutputFloats.IsEmpty())
			{
				Call.OutputFloats[0] = 84.0f;
			}
			OutResult.bSucceeded = true;
			OutResult.ReturnValue = Call.IntArgs[0];
		}
		else
		{
			OutResult.Details = TEXT("unexpected Wasmtime test binding");
		}

		if (BackendToUnload != nullptr)
		{
			IAvidScriptVmBackend* Backend = BackendToUnload;
			BackendToUnload = nullptr;
			Backend->Unload();
		}
		return OutResult.bSucceeded;
	}

	bool DispatchDynamicHostCall(
		const FAvidScriptDynamicHostCall& Call,
		FAvidScriptDynamicHostCallResult& OutResult) override
	{
		++DynamicCallCount;
		LastDynamicOrdinal = Call.BindingOrdinal;
		LastDynamicArgumentCount = Call.Arguments.Num();
		LastDynamicInput = Call.Arguments.IsEmpty() ? 0 : static_cast<int32>(Call.Arguments[0]);
		bSawDynamicGuestMemory = Call.GuestMemory != nullptr;
		OutResult.bSucceeded = !bFailDynamicCall;
		OutResult.ReturnValue = LastDynamicInput + 1;
		if (bFailDynamicCall)
		{
			OutResult.Details = TEXT("wasmtime dynamic failure sentinel");
			return false;
		}
		return true;
	}

	IAvidScriptVmBackend* BackendToUnload = nullptr;
	int32 CallCount = 0;
	int32 LastI32 = 0;
	int32 CapturedOutputFloatCount = 0;
	EAvidScriptHostBindingId LastBindingId = EAvidScriptHostBindingId::Invalid;
	TArray<uint32> CapturedInputCells;
	int32 DynamicCallCount = 0;
	uint32 LastDynamicOrdinal = MAX_uint32;
	int32 LastDynamicArgumentCount = 0;
	int32 LastDynamicInput = 0;
	bool bSawDynamicGuestMemory = false;
	bool bFailDynamicCall = false;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimeLifecycleTest,
	"AvidScript.VM.Wasmtime.Lifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeLifecycleTest::RunTest(const FString& Parameters)
{
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateWasmtimeBackendForTest(Error);
#if !AVIDSCRIPT_WITH_WASMTIME
	TestNull(TEXT("Wasmtime is unavailable without its managed dependency"), Backend.Get());
	TestEqual(TEXT("unavailable category"), Error.Category, FString(TEXT("backend_unavailable")));
	return true;
#else
	if (!TestNotNull(TEXT("Wasmtime backend is created"), Backend.Get()))
	{
		return false;
	}
	TestEqual(TEXT("stable backend id"), Backend->GetBackendInfo().StableBackendId, FString(TEXT("wasmtime.cranelift.jit")));
	TestEqual(TEXT("locked runtime identity"), Backend->GetBackendInfo().RuntimeVersion, FString(TEXT("45.0.0")));
	TestEqual(TEXT("target triple"), Backend->GetBackendInfo().TargetTriple, FString(TEXT("x86_64-pc-windows-msvc")));

	const TArray<uint8> Bytecode = BuildWasmtimeLifecycleFixture();
	FAvidScriptVmLoadConfig Config;
	if (!LoadWasmtimeTestModule(*this, *Backend, Bytecode, Config, Error))
	{
		return false;
	}
	const FAvidScriptVmBackendInfo& LoadedInfo = Backend->GetBackendInfo();
	TestEqual(
		TEXT("loaded runtime artifact identity is SHA-256"),
		LoadedInfo.RuntimeArtifactSha256.Len(),
		64);
	TestEqual(
		TEXT("runtime build identity binds the loaded DLL"),
		LoadedInfo.RuntimeBuildIdentity,
		FString::Printf(
			TEXT("wasmtime-v45.0.0;cranelift=1;dll_sha256=%s"),
			*LoadedInfo.RuntimeArtifactSha256));
	TestTrue(TEXT("RuntimeInitMs is measured"), Backend->GetLoadMetrics().RuntimeInitMs > 0.0);
	TestTrue(TEXT("ModuleLoadMs is measured"), Backend->GetLoadMetrics().ModuleLoadMs > 0.0);
	TestTrue(TEXT("ModuleInstantiateMs is measured"), Backend->GetLoadMetrics().ModuleInstantiateMs > 0.0);
	TestTrue(TEXT("ExecEnvCreateMs is measured"), Backend->GetLoadMetrics().ExecEnvCreateMs > 0.0);

	FAvidScriptVmExportHandle BeginHandle;
	FAvidScriptVmExportHandle TickHandle;
	TestTrue(TEXT("BeginPlay resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginHandle, Error));
	TestTrue(TEXT("Tick resolves"), Backend->ResolveExport(TEXT("avid_on_tick"), TickHandle, Error));
	FAvidScriptVmExportHandle CachedTickHandle;
	TestTrue(TEXT("Tick resolves from cache"), Backend->ResolveExport(TEXT("avid_on_tick"), CachedTickHandle, Error));
	TestEqual(TEXT("two unique export lookups"), Backend->GetExportLookupCount(), 2u);
	TestEqual(TEXT("cached export slot"), CachedTickHandle.Slot, TickHandle.Slot);

	TestTrue(TEXT("BeginPlay calls"), Backend->Call(BeginHandle, FAvidScriptVmCallFrame(), Error));
	FAvidScriptVmCallFrame TickFrame;
	TickFrame.CellCount = 1;
	const float DeltaSeconds = 1.0f / 60.0f;
	FMemory::Memcpy(TickFrame.Cells, &DeltaSeconds, sizeof(float));
	TestTrue(TEXT("Tick calls"), Backend->Call(TickHandle, TickFrame, Error));

	FAvidScriptVmCallFrame WrongFrame;
	WrongFrame.CellCount = 2;
	TestFalse(TEXT("cached invocation shape rejects wrong arity"), Backend->Call(TickHandle, WrongFrame, Error));
	TestEqual(TEXT("wrong arity category"), Error.Category, FString(TEXT("invalid_arguments")));

	IAvidScriptVmGuestMemory* GuestMemory = Backend->GetGuestMemory();
	if (!TestNotNull(TEXT("guest memory is exposed"), GuestMemory))
	{
		return false;
	}
	const TArray<uint8> Written = { 0x11, 0x22, 0x33, 0x44 };
	TArray<uint8> Read;
	Read.SetNumZeroed(Written.Num());
	FString MemoryError;
	TestTrue(TEXT("guest write succeeds"), GuestMemory->WriteBytes(16, Written, MemoryError));
	TestTrue(TEXT("guest read succeeds"), GuestMemory->ReadBytes(16, Read, MemoryError));
	TestEqual(TEXT("guest bytes round trip"), Read, Written);
	TestFalse(TEXT("guest range wraparound is rejected"), GuestMemory->ReadBytes(MAX_uint32 - 1, Read, MemoryError));
	TestTrue(TEXT("guest range failure is stable"), MemoryError.Contains(TEXT("guest_memory_invalid")));

	FAvidScriptVmExportHandle MissingHandle;
	TestFalse(TEXT("missing export rejects"), Backend->ResolveExport(TEXT("missing"), MissingHandle, Error));
	TestEqual(TEXT("missing export category"), Error.Category, FString(TEXT("missing_export")));

	TUniquePtr<IAvidScriptVmBackend> ForeignBackend = CreateWasmtimeBackendForTest(Error);
	TestTrue(TEXT("foreign backend loads"), ForeignBackend->Load(Bytecode, TEXT("wasmtime_foreign"), Config, Error));
	TestFalse(TEXT("foreign backend rejects handle"), ForeignBackend->Call(BeginHandle, FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("foreign category"), Error.Category, FString(TEXT("foreign_export")));

	Backend->Unload();
	TestFalse(TEXT("backend unloads"), Backend->IsLoaded());
	TestFalse(TEXT("stale handle rejects"), Backend->Call(BeginHandle, FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("stale category"), Error.Category, FString(TEXT("stale_export")));
	Backend->Unload();

	const uint8 Malformed[] = { 0x00, 0x61, 0x73, 0x6d };
	TestFalse(TEXT("malformed WASM rejects"), Backend->Load(MakeArrayView(Malformed), TEXT("wasmtime_malformed"), Config, Error));
	TestFalse(TEXT("malformed WASM reports details"), Error.Details.IsEmpty());
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimeResultAbiTest,
	"AvidScript.VM.Wasmtime.ResultAbi",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeResultAbiTest::RunTest(const FString& Parameters)
{
#if !AVIDSCRIPT_WITH_WASMTIME
	return true;
#else
	FAvidScriptVmError Error;
	FAvidScriptVmLoadConfig Config;
	TUniquePtr<IAvidScriptVmBackend> Backend =
		CreateWasmtimeBackendForTest(Error);

	const TArray<uint8> VoidFixture = BuildWasmtimeResultFixture(0x7f, 0);
	if (!LoadWasmtimeTestModule(*this, *Backend, VoidFixture, Config, Error))
	{
		return false;
	}
	FAvidScriptVmExportHandle Handle;
	TestTrue(
		TEXT("void result export resolves"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	FAvidScriptVmCallResult Result;
	TestTrue(
		TEXT("void result export calls"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error, &Result));
	TestEqual(TEXT("void result has zero cells"), Result.CellCount, 0u);
	TestTrue(
		TEXT("legacy call keeps default null result compatibility"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error));

	const TArray<uint8> I32Fixture = BuildWasmtimeResultFixture(0x7f, 1);
	if (!LoadWasmtimeTestModule(*this, *Backend, I32Fixture, Config, Error))
	{
		return false;
	}
	TestTrue(
		TEXT("i32 result export resolves"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	TestTrue(
		TEXT("i32 result export calls"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error, &Result));
	TestEqual(TEXT("i32 result has one cell"), Result.CellCount, 1u);
	TestEqual(TEXT("i32 result value is preserved"), Result.Cells[0], 7u);

	const TArray<uint8> OversizeFixture = BuildWasmtimeResultFixture(
		0x7f,
		FAvidScriptVmCallResult::MaxCells + 1);
	if (!LoadWasmtimeTestModule(*this, *Backend, OversizeFixture, Config, Error))
	{
		return false;
	}
	TestFalse(
		TEXT("oversize result ABI rejects at resolve"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	TestEqual(
		TEXT("oversize result category"),
		Error.Category,
		FString(TEXT("invalid_export")));

	const TArray<uint8> UnsupportedFixture =
		BuildWasmtimeResultFixture(0x6f, 1);
	if (!LoadWasmtimeTestModule(
			*this,
			*Backend,
			UnsupportedFixture,
			Config,
			Error))
	{
		return false;
	}
	TestFalse(
		TEXT("unsupported result ABI rejects at resolve"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	TestEqual(
		TEXT("unsupported result category"),
		Error.Category,
		FString(TEXT("invalid_arguments")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimeStaticImportsTest,
	"AvidScript.VM.Wasmtime.StaticImports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeStaticImportsTest::RunTest(const FString& Parameters)
{
#if !AVIDSCRIPT_WITH_WASMTIME
	return true;
#else
	FAvidScriptWasmtimeTestDispatcher Dispatcher;
	FAvidScriptVmLoadConfig Config;
	Config.HostDispatcher = &Dispatcher;
	FAvidScriptVmError Error;

	TUniquePtr<IAvidScriptVmBackend> Backend = CreateWasmtimeBackendForTest(Error);
	const TArray<uint8> AddFixture = BuildWasmtimeI32ImportFixture("host_add_i32", 41);
	if (!LoadWasmtimeTestModule(*this, *Backend, AddFixture, Config, Error))
	{
		return false;
	}
	FAvidScriptVmExportHandle BeginHandle;
	TestTrue(TEXT("add BeginPlay resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginHandle, Error));
	TestTrue(TEXT("host_add_i32 succeeds"), Backend->Call(BeginHandle, FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("host_add_i32 binding id"), Dispatcher.LastBindingId, EAvidScriptHostBindingId::HostAddI32);
	TestEqual(TEXT("host_add_i32 input"), Dispatcher.LastI32, 41);

	const TArray<uint8> FailFixture = BuildWasmtimeI32ImportFixture("host_fail_i32", 7);
	TestTrue(TEXT("failure fixture loads"), Backend->Load(FailFixture, TEXT("wasmtime_host_fail"), Config, Error));
	TestTrue(TEXT("failure BeginPlay resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginHandle, Error));
	TestFalse(TEXT("host_fail_i32 traps"), Backend->Call(BeginHandle, FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("host failure category"), Error.Category, FString(TEXT("host_import_failed")));
	TestEqual(TEXT("host failure import"), Error.ImportName, FString(TEXT("host_fail_i32")));
	TestEqual(TEXT("host failure details"), Error.Details, FString(TEXT("wasmtime host failure sentinel")));

	const TArray<uint8> VectorFixture = BuildWasmtimeVectorFixture();
	TestTrue(TEXT("vector fixture loads"), Backend->Load(VectorFixture, TEXT("wasmtime_vector"), Config, Error));
	TestTrue(TEXT("vector BeginPlay resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginHandle, Error));
	TestTrue(TEXT("vector output import succeeds"), Backend->Call(BeginHandle, FAvidScriptVmCallFrame(), Error));
	uint32 ExpectedVectorBits = 0;
	const float ExpectedVector = 42.0f;
	FMemory::Memcpy(&ExpectedVectorBits, &ExpectedVector, sizeof(float));
	TestEqual(TEXT("guest observes vector output memory"), Dispatcher.LastI32, static_cast<int32>(ExpectedVectorBits));

	const TArray<uint8> BatchFixture = BuildWasmtimeBatchFixture();
	TestTrue(TEXT("batch fixture loads"), Backend->Load(BatchFixture, TEXT("wasmtime_batch"), Config, Error));
	TestTrue(TEXT("batch BeginPlay resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginHandle, Error));
	TestTrue(TEXT("transform batch succeeds"), Backend->Call(BeginHandle, FAvidScriptVmCallFrame(), Error));
	const TArray<uint32> ExpectedCells = { 7u, 11u, 13u, 17u };
	TestTrue(TEXT("batch input span is preserved"), Dispatcher.CapturedInputCells == ExpectedCells);
	TestEqual(TEXT("batch output span is preserved"), Dispatcher.CapturedOutputFloatCount, 18);
	uint32 ExpectedBatchBits = 0;
	const float ExpectedBatch = 84.0f;
	FMemory::Memcpy(&ExpectedBatchBits, &ExpectedBatch, sizeof(float));
	TestEqual(TEXT("guest observes batch output memory"), Dispatcher.LastI32, static_cast<int32>(ExpectedBatchBits));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimeDynamicImportsTest,
	"AvidScript.VM.Wasmtime.DynamicImports",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeDynamicImportsTest::RunTest(const FString& Parameters)
{
#if !AVIDSCRIPT_WITH_WASMTIME
	return true;
#else
	const TArray<uint8> Fixture = BuildWasmtimeI32ImportFixture("avid_ue_1111111111111111", 41);
	FAvidScriptVmBindingPackage FirstPackage = MakeWasmtimeDynamicPackage(0, TEXT('a'));
	FAvidScriptVmBindingPackage SecondPackage = MakeWasmtimeDynamicPackage(1, TEXT('b'));
	FAvidScriptWasmtimeTestDispatcher FirstDispatcher;
	FAvidScriptWasmtimeTestDispatcher SecondDispatcher;
	FAvidScriptVmLoadConfig FirstConfig;
	FirstConfig.HostDispatcher = &FirstDispatcher;
	FirstConfig.BindingPackage = &FirstPackage;
	FAvidScriptVmLoadConfig SecondConfig;
	SecondConfig.HostDispatcher = &SecondDispatcher;
	SecondConfig.BindingPackage = &SecondPackage;

	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> FirstBackend = CreateWasmtimeBackendForTest(Error);
	TUniquePtr<IAvidScriptVmBackend> SecondBackend = CreateWasmtimeBackendForTest(Error);
	if (!TestTrue(TEXT("first per-instance package loads"), FirstBackend->Load(
			Fixture,
			TEXT("wasmtime_dynamic_first"),
			FirstConfig,
			Error))
		|| !TestTrue(TEXT("second per-instance package loads"), SecondBackend->Load(
			Fixture,
			TEXT("wasmtime_dynamic_second"),
			SecondConfig,
			Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}

	FAvidScriptVmExportHandle FirstBeginPlay;
	FAvidScriptVmExportHandle SecondBeginPlay;
	TestTrue(TEXT("first dynamic BeginPlay resolves"),
		FirstBackend->ResolveExport(TEXT("avid_on_begin_play"), FirstBeginPlay, Error));
	TestTrue(TEXT("second dynamic BeginPlay resolves"),
		SecondBackend->ResolveExport(TEXT("avid_on_begin_play"), SecondBeginPlay, Error));
	TestTrue(TEXT("first dynamic callback succeeds"),
		FirstBackend->Call(FirstBeginPlay, FAvidScriptVmCallFrame(), Error));
	TestTrue(TEXT("second dynamic callback succeeds"),
		SecondBackend->Call(SecondBeginPlay, FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("first metadata keeps local ordinal zero"), FirstDispatcher.LastDynamicOrdinal, 0u);
	TestEqual(TEXT("second metadata keeps local ordinal one"), SecondDispatcher.LastDynamicOrdinal, 1u);
	TestEqual(TEXT("dynamic callback receives one canonical cell"), SecondDispatcher.LastDynamicArgumentCount, 1);
	TestEqual(TEXT("dynamic callback preserves i32 cell bits"), SecondDispatcher.LastDynamicInput, 41);
	TestTrue(TEXT("dynamic callback exposes active guest memory"), SecondDispatcher.bSawDynamicGuestMemory);

	SecondDispatcher.bFailDynamicCall = true;
	TestFalse(TEXT("dynamic dispatcher failure traps"),
		SecondBackend->Call(SecondBeginPlay, FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("dynamic failure category"), Error.Category, FString(TEXT("host_import_failed")));
	TestEqual(TEXT("dynamic failure module"), Error.ImportModuleName, FString(TEXT("avidscript")));
	TestEqual(TEXT("dynamic failure import"), Error.ImportName, FString(WasmtimeDynamicImportName));
	TestEqual(TEXT("dynamic failure details"), Error.Details, FString(TEXT("wasmtime dynamic failure sentinel")));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimeTrapAndReentrantUnloadTest,
	"AvidScript.VM.Wasmtime.TrapAndReentrantUnload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeTrapAndReentrantUnloadTest::RunTest(const FString& Parameters)
{
#if !AVIDSCRIPT_WITH_WASMTIME
	return true;
#else
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateWasmtimeBackendForTest(Error);
	FAvidScriptVmLoadConfig Config;
	const TArray<uint8> LifecycleFixture = BuildWasmtimeLifecycleFixture();
	if (!LoadWasmtimeTestModule(*this, *Backend, LifecycleFixture, Config, Error))
	{
		return false;
	}
	FAvidScriptVmExportHandle TrapHandle;
	TestTrue(TEXT("trap export resolves"), Backend->ResolveExport(TEXT("avid_trap"), TrapHandle, Error));
	TestFalse(TEXT("guest trap fails"), Backend->Call(TrapHandle, FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("guest trap category"), Error.Category, FString(TEXT("trap")));
	TestFalse(TEXT("guest trap details are nonempty"), Error.Details.IsEmpty());
	TestTrue(TEXT("guest trap has structured frames"), !Error.StackFrames.IsEmpty());

	FAvidScriptWasmtimeTestDispatcher Dispatcher;
	Config.HostDispatcher = &Dispatcher;
	const TArray<uint8> AddFixture = BuildWasmtimeI32ImportFixture("host_add_i32", 3);
	TestTrue(TEXT("reentrant fixture loads"), Backend->Load(AddFixture, TEXT("wasmtime_reentrant"), Config, Error));
	FAvidScriptVmExportHandle BeginHandle;
	TestTrue(TEXT("reentrant BeginPlay resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginHandle, Error));
	Dispatcher.BackendToUnload = Backend.Get();
	TestFalse(TEXT("reentrant unload fails active call safely"), Backend->Call(BeginHandle, FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("reentrant unload category"), Error.Category, FString(TEXT("reentrant_unload")));
	TestFalse(TEXT("deferred unload completes"), Backend->IsLoaded());
	return true;
#endif
}

#endif
