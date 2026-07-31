#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptHash.h"
#include "AvidScriptVmArtifact.h"
#include "AvidScriptVmBackend.h"
#include "AvidScriptVmResultFixtureBuilder.h"
#include "AvidScriptWasmtimeApi.h"

#include "Misc/AutomationTest.h"

#ifndef AVIDSCRIPT_WITH_WASMTIME
#define AVIDSCRIPT_WITH_WASMTIME 0
#endif

namespace
{
constexpr const TCHAR* WasmtimeDynamicImportName = TEXT("avid_ue_1111111111111111");
constexpr const TCHAR* WasmtimeTypedGuestResultImportName =
	TEXT("avid_s1_1111111111111111");
constexpr const TCHAR* WasmtimeTypedGuestResultStableId =
	TEXT("1111111111111111111111111111111111111111111111111111111111111111");

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
	AppendWasmtimeU32Leb(Types, 3);
	const uint8 EmptyType[] = { 0x60, 0x00, 0x00 };
	Types.Append(EmptyType, UE_ARRAY_COUNT(EmptyType));
	const uint8 TickType[] = { 0x60, 0x01, 0x7d, 0x00 };
	Types.Append(TickType, UE_ARRAY_COUNT(TickType));
	const uint8 GrowType[] = { 0x60, 0x00, 0x01, 0x7f };
	Types.Append(GrowType, UE_ARRAY_COUNT(GrowType));
	AppendWasmtimeSection(Module, 1, Types);

	TArray<uint8> Functions;
	AppendWasmtimeU32Leb(Functions, 4);
	AppendWasmtimeU32Leb(Functions, 0);
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeU32Leb(Functions, 0);
	AppendWasmtimeU32Leb(Functions, 2);
	AppendWasmtimeSection(Module, 3, Functions);

	TArray<uint8> Memory;
	AppendWasmtimeU32Leb(Memory, 1);
	Memory.Add(0x00);
	AppendWasmtimeU32Leb(Memory, 1);
	AppendWasmtimeSection(Module, 5, Memory);

	TArray<uint8> Exports;
	AppendWasmtimeU32Leb(Exports, 5);
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
	AppendWasmtimeString(Exports, "avid_grow_memory");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 3);
	AppendWasmtimeSection(Module, 7, Exports);

	TArray<uint8> Code;
	AppendWasmtimeU32Leb(Code, 4);
	const TArray<uint8> EmptyBody = { 0x00, 0x0b };
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(EmptyBody.Num()));
	Code.Append(EmptyBody);
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(EmptyBody.Num()));
	Code.Append(EmptyBody);
	const TArray<uint8> TrapBody = { 0x00, 0x00, 0x0b };
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(TrapBody.Num()));
	Code.Append(TrapBody);
	const TArray<uint8> GrowBody = { 0x00, 0x41, 0x01, 0x40, 0x00, 0x0b };
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(GrowBody.Num()));
	Code.Append(GrowBody);
	AppendWasmtimeSection(Module, 10, Code);
	return Module;
}

TArray<uint8> BuildWasmtimeWideParameterFixture()
{
	TArray<uint8> Module = MakeWasmtimeModule();

	TArray<uint8> Types;
	AppendWasmtimeU32Leb(Types, 1);
	Types.Add(0x60);
	AppendWasmtimeU32Leb(
		Types,
		FAvidScriptVmCallFrame::MaxCells + 1);
	for (uint32 Index = 0;
		Index < FAvidScriptVmCallFrame::MaxCells + 1;
		++Index)
	{
		Types.Add(0x7f);
	}
	Types.Add(0x00);
	AppendWasmtimeSection(Module, 1, Types);

	TArray<uint8> Functions;
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeU32Leb(Functions, 0);
	AppendWasmtimeSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendWasmtimeU32Leb(Exports, 1);
	AppendWasmtimeString(Exports, "wide");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 0);
	AppendWasmtimeSection(Module, 7, Exports);

	TArray<uint8> Code;
	AppendWasmtimeU32Leb(Code, 1);
	const TArray<uint8> EmptyBody = { 0x00, 0x0b };
	AppendWasmtimeU32Leb(Code, static_cast<uint32>(EmptyBody.Num()));
	Code.Append(EmptyBody);
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

TArray<uint8> BuildWasmtimeSelfI32PairGuestResultFixture()
{
	TArray<uint8> Module = MakeWasmtimeModule();

	TArray<uint8> Types;
	AppendWasmtimeU32Leb(Types, 2);
	const uint8 ImportType[] = {
		0x60,
		0x05,
		0x7f,
		0x7f,
		0x7f,
		0x7f,
		0x7f,
		0x01,
		0x7f
	};
	Types.Append(ImportType, UE_ARRAY_COUNT(ImportType));
	const uint8 ExportType[] = { 0x60, 0x00, 0x01, 0x7f };
	Types.Append(ExportType, UE_ARRAY_COUNT(ExportType));
	AppendWasmtimeSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendWasmtimeU32Leb(Imports, 1);
	AppendWasmtimeString(Imports, "avidscript");
	AppendWasmtimeString(Imports, "avid_s1_1111111111111111");
	Imports.Add(0x00);
	AppendWasmtimeU32Leb(Imports, 0);
	AppendWasmtimeSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeU32Leb(Functions, 1);
	AppendWasmtimeSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendWasmtimeU32Leb(Exports, 1);
	AppendWasmtimeString(Exports, "run");
	Exports.Add(0x00);
	AppendWasmtimeU32Leb(Exports, 1);
	AppendWasmtimeSection(Module, 7, Exports);

	TArray<uint8> Body;
	Body.Add(0x00);
	AppendWasmtimeI32Const(Body, 7);
	AppendWasmtimeI32Const(Body, 11);
	AppendWasmtimeI32Const(Body, 13);
	AppendWasmtimeI32Const(Body, 17);
	AppendWasmtimeI32Const(Body, 64);
	Body.Add(0x10);
	AppendWasmtimeU32Leb(Body, 0);
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

FAvidScriptVmBindingPackage MakeWasmtimeTypedGuestResultPackage()
{
	FAvidScriptVmBindingPackage Package;
	Package.PackageName = TEXT("avidscript.phase57.typed_guest_result");
	Package.PackageHash = FString::ChrN(64, TEXT('5'));

	FAvidScriptVmDynamicImport Import;
	Import.StableId = WasmtimeTypedGuestResultStableId;
	Import.Ordinal = 0;
	Import.ModuleName = TEXT("avidscript");
	Import.ImportName = WasmtimeTypedGuestResultImportName;
	Import.Signature = TEXT("(iiiii)i");
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

TUniquePtr<IAvidScriptVmBackend> CreateWasmtimePrecompiledBackendForTest(
	FAvidScriptVmError& OutError)
{
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Aot;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	return CreateAvidScriptVmBackend(Selection, OutError);
}

#if AVIDSCRIPT_WITH_WASMTIME
FString ConsumeWasmtimeTestFailure(AvidScriptWasmtimeFailure* Failure)
{
	if (Failure == nullptr)
	{
		return FString();
	}
	size_t MessageSize = 0;
	const char* Message = avidscript_wasmtime_failure_message(
		Failure,
		&MessageSize);
	const FUTF8ToTCHAR Converted(
		Message,
		static_cast<int32>(FMath::Min<size_t>(MessageSize, MAX_int32)));
	const FString Details(Converted.Length(), Converted.Get());
	avidscript_wasmtime_failure_delete(Failure);
	return Details;
}

bool SerializeWasmtimeFixture(
	TConstArrayView<uint8> Bytecode,
	TArray<uint8>& OutSerialized,
	FString& OutError)
{
	OutSerialized.Reset();
	OutError.Reset();
	AvidScriptWasmtimeEngine* Engine = avidscript_wasmtime_engine_new();
	if (Engine == nullptr)
	{
		OutError = TEXT("Wasmtime test engine allocation failed.");
		return false;
	}

	AvidScriptWasmtimeModule* Module = nullptr;
	AvidScriptWasmtimeFailure* Failure = avidscript_wasmtime_module_new(
		Engine,
		Bytecode.GetData(),
		static_cast<size_t>(Bytecode.Num()),
		&Module);
	if (Failure != nullptr || Module == nullptr)
	{
		OutError = Failure != nullptr
			? ConsumeWasmtimeTestFailure(Failure)
			: TEXT("Wasmtime test module allocation failed.");
		avidscript_wasmtime_engine_delete(Engine);
		return false;
	}

	uint8_t* SerializedBytes = nullptr;
	size_t SerializedSize = 0;
	Failure = avidscript_wasmtime_module_serialize(
		Module,
		&SerializedBytes,
		&SerializedSize);
	if (Failure != nullptr
		|| SerializedBytes == nullptr
		|| SerializedSize == 0
		|| SerializedSize > static_cast<size_t>(MAX_int32))
	{
		OutError = Failure != nullptr
			? ConsumeWasmtimeTestFailure(Failure)
			: TEXT("Wasmtime produced an invalid serialized module.");
		avidscript_wasmtime_serialized_bytes_delete(SerializedBytes);
		avidscript_wasmtime_module_delete(Module);
		avidscript_wasmtime_engine_delete(Engine);
		return false;
	}

	OutSerialized.Append(SerializedBytes, static_cast<int32>(SerializedSize));
	avidscript_wasmtime_serialized_bytes_delete(SerializedBytes);
	avidscript_wasmtime_module_delete(Module);
	avidscript_wasmtime_engine_delete(Engine);
	return true;
}
#endif

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

class FAvidScriptWasmtimeTypedBridgeDispatcher final
	: public IAvidScriptVmTypedHostDispatcher
{
public:
	EAvidScriptVmTypedHostStatus DispatchEmptyI32(
		uint32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	EAvidScriptVmTypedHostStatus DispatchI32PairToI32(
		uint32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	EAvidScriptVmTypedHostStatus DispatchSelfI32PairToI32(
		uint32,
		int32,
		int32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	EAvidScriptVmTypedHostStatus DispatchSelfPropertyI32GetSet(
		uint32,
		int32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	EAvidScriptVmTypedHostStatus DispatchSelfVectorValue(
		uint32,
		int32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	EAvidScriptVmTypedHostStatus DispatchStableObjectRoundtrip(
		uint32,
		int32,
		int32,
		int32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}

	EAvidScriptVmTypedHostStatus DispatchCommandBufferSubmit(
		uint32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
};

struct FAvidScriptWasmtimeTypedGuestResultContext
{
	EAvidScriptVmTypedHostStatus Status =
		EAvidScriptVmTypedHostStatus::Succeeded;
	int32 OutStatus = 1;
	int32 CallCount = 0;
	int32 SelfSlot = 0;
	int32 SelfGeneration = 0;
	int32 Left = 0;
	int32 Right = 0;
	int32 GuestAddress = 0;
};

EAvidScriptVmTypedHostStatus InvokeWasmtimeTypedGuestResultForTest(
	void* Context,
	const int32 SelfSlot,
	const int32 SelfGeneration,
	const int32 Left,
	const int32 Right,
	const int32 GuestAddress,
	int32& OutStatus)
{
	FAvidScriptWasmtimeTypedGuestResultContext* TargetContext =
		static_cast<FAvidScriptWasmtimeTypedGuestResultContext*>(Context);
	if (TargetContext == nullptr)
	{
		return EAvidScriptVmTypedHostStatus::Rejected;
	}
	++TargetContext->CallCount;
	TargetContext->SelfSlot = SelfSlot;
	TargetContext->SelfGeneration = SelfGeneration;
	TargetContext->Left = Left;
	TargetContext->Right = Right;
	TargetContext->GuestAddress = GuestAddress;
	OutStatus = TargetContext->OutStatus;
	return TargetContext->Status;
}
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
			TEXT("wasmtime-v45.0.0;cranelift=1;opt=speed_and_size;wasm32_memory_stable=1;dll_sha256=%s"),
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

	FAvidScriptVmPreparedExportCall PreparedBegin;
	FAvidScriptVmPreparedExportCall PreparedTick;
	TestTrue(
		TEXT("BeginPlay prepares a stable direct call"),
		Backend->PrepareExportCall(BeginHandle, PreparedBegin, Error));
	TestTrue(
		TEXT("Tick prepares a stable direct call"),
		Backend->PrepareExportCall(TickHandle, PreparedTick, Error));
	TestTrue(TEXT("prepared BeginPlay is valid"), PreparedBegin.IsValid());
	TestTrue(TEXT("prepared Tick is valid"), PreparedTick.IsValid());
	TestEqual(
		TEXT("prepared BeginPlay parameter count is exact"),
		PreparedBegin.ParameterCellCount,
		0u);
	TestEqual(
		TEXT("prepared Tick parameter count is exact"),
		PreparedTick.ParameterCellCount,
		1u);

	TestTrue(TEXT("BeginPlay calls"), Backend->Call(BeginHandle, FAvidScriptVmCallFrame(), Error));
	TestTrue(
		TEXT("prepared BeginPlay calls"),
		PreparedBegin.Call(FAvidScriptVmCallFrame(), Error));
	FAvidScriptVmCallFrame TickFrame;
	TickFrame.CellCount = 1;
	const float DeltaSeconds = 1.0f / 60.0f;
	FMemory::Memcpy(TickFrame.Cells, &DeltaSeconds, sizeof(float));
	TestTrue(TEXT("Tick calls"), Backend->Call(TickHandle, TickFrame, Error));
	TestTrue(TEXT("prepared Tick calls"), PreparedTick.Call(TickFrame, Error));

	FAvidScriptVmCallFrame WrongFrame;
	WrongFrame.CellCount = 2;
	TestFalse(TEXT("cached invocation shape rejects wrong arity"), Backend->Call(TickHandle, WrongFrame, Error));
	TestEqual(TEXT("wrong arity category"), Error.Category, FString(TEXT("invalid_arguments")));
	TestFalse(
		TEXT("prepared invocation rejects wrong arity"),
		PreparedTick.Call(WrongFrame, Error));
	TestEqual(
		TEXT("prepared wrong arity category"),
		Error.Category,
		FString(TEXT("invalid_arguments")));

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

	FAvidScriptVmExportHandle GrowMemoryHandle;
	TestTrue(
		TEXT("memory grow export resolves"),
		Backend->ResolveExport(
			TEXT("avid_grow_memory"),
			GrowMemoryHandle,
			Error));
	TestTrue(
		TEXT("memory grow export calls"),
		Backend->Call(
			GrowMemoryHandle,
			FAvidScriptVmCallFrame(),
			Error));
	constexpr uint32 SecondPageAddress = 64u * 1024u;
	Read.Init(0, Written.Num());
	TestTrue(
		TEXT("cached memory handle writes after guest memory grow"),
		GuestMemory->WriteBytes(
			SecondPageAddress,
			Written,
			MemoryError));
	TestTrue(
		TEXT("cached memory handle reads after guest memory grow"),
		GuestMemory->ReadBytes(
			SecondPageAddress,
			Read,
			MemoryError));
	TestEqual(TEXT("grown guest bytes round trip"), Read, Written);

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
	TestFalse(
		TEXT("stale prepared export rejects without dereferencing freed storage"),
		PreparedBegin.Call(FAvidScriptVmCallFrame(), Error));
	TestEqual(
		TEXT("stale prepared category"),
		Error.Category,
		FString(TEXT("stale_export")));
	Backend->Unload();

	TestTrue(
		TEXT("backend reloads with a fresh cached memory handle"),
		Backend->Load(
			Bytecode,
			TEXT("wasmtime_reloaded"),
			Config,
			Error));
	GuestMemory = Backend->GetGuestMemory();
	Read.Init(0, Written.Num());
	TestTrue(
		TEXT("reloaded guest memory write succeeds"),
		GuestMemory->WriteBytes(32, Written, MemoryError));
	TestTrue(
		TEXT("reloaded guest memory read succeeds"),
		GuestMemory->ReadBytes(32, Read, MemoryError));
	TestEqual(TEXT("reloaded guest bytes round trip"), Read, Written);
	Backend->Unload();

	const uint8 Malformed[] = { 0x00, 0x61, 0x73, 0x6d };
	TestFalse(TEXT("malformed WASM rejects"), Backend->Load(MakeArrayView(Malformed), TEXT("wasmtime_malformed"), Config, Error));
	TestFalse(TEXT("malformed WASM reports details"), Error.Details.IsEmpty());
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimePrecompiledArtifactTest,
	"AvidScript.VM.Wasmtime.PrecompiledArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimePrecompiledArtifactTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> JitBackend =
		CreateWasmtimeBackendForTest(Error);
#if !AVIDSCRIPT_WITH_WASMTIME
	TestNull(
		TEXT("Wasmtime JIT is unavailable without its managed dependency"),
		JitBackend.Get());
	TUniquePtr<IAvidScriptVmBackend> PrecompiledBackend =
		CreateWasmtimePrecompiledBackendForTest(Error);
	TestNull(
		TEXT("Wasmtime precompiled backend is unavailable without its managed dependency"),
		PrecompiledBackend.Get());
	return true;
#else
	if (!TestNotNull(TEXT("Wasmtime JIT backend is created"), JitBackend.Get()))
	{
		return false;
	}

	const TArray<uint8> Bytecode = BuildWasmtimeLifecycleFixture();
	FAvidScriptVmLoadConfig Config;
	if (!LoadWasmtimeTestModule(
		*this,
		*JitBackend,
		Bytecode,
		Config,
		Error))
	{
		return false;
	}
	const FString CompilerBuildIdentity =
		JitBackend->GetBackendInfo().RuntimeBuildIdentity;
	const FString TargetTriple = JitBackend->GetBackendInfo().TargetTriple;
	JitBackend->Unload();
	const uint8 ForeignExecutionBytes[] = { 0x00, 0x61, 0x73, 0x6d };
	FAvidScriptVmArtifactView MismatchedWasmArtifact =
		FAvidScriptVmArtifactView::FromWasmBytecode(Bytecode);
	MismatchedWasmArtifact.ExecutionBytes = MakeArrayView(ForeignExecutionBytes);
	TestFalse(
		TEXT("JIT artifact cannot separate executed and validated WASM bytes"),
		JitBackend->LoadArtifact(
			MismatchedWasmArtifact,
			TEXT("wasmtime_jit_identity_mismatch"),
			Config,
			Error));
	TestEqual(
		TEXT("JIT identity mismatch category"),
		Error.Category,
		FString(TEXT("artifact_identity_mismatch")));

	TArray<uint8> SerializedBytes;
	FString SerializationError;
	if (!TestTrue(
		TEXT("Wasmtime module serializes with the production engine configuration"),
		SerializeWasmtimeFixture(
			Bytecode,
			SerializedBytes,
			SerializationError)))
	{
		AddError(SerializationError);
		return false;
	}

	TUniquePtr<IAvidScriptVmBackend> PrecompiledBackend =
		CreateWasmtimePrecompiledBackendForTest(Error);
	if (!TestNotNull(
		TEXT("Wasmtime precompiled backend is created"),
		PrecompiledBackend.Get()))
	{
		return false;
	}
	TestEqual(
		TEXT("precompiled backend has a stable identity"),
		PrecompiledBackend->GetBackendInfo().StableBackendId,
		FString(TEXT("wasmtime.cranelift.precompiled")));
	TestTrue(
		TEXT("precompiled backend advertises AOT"),
		EnumHasAnyFlags(
			PrecompiledBackend->GetBackendInfo().Capabilities,
			EAvidScriptVmCapability::Aot));

	TestFalse(
		TEXT("precompiled backend rejects raw WASM loading"),
		PrecompiledBackend->Load(
			Bytecode,
			TEXT("wasmtime_precompiled_raw"),
			Config,
			Error));
	TestEqual(
		TEXT("raw WASM rejection category"),
		Error.Category,
		FString(TEXT("artifact_format_mismatch")));

	const FString SerializedIdentity =
		FAvidScriptHash::Sha256Hex(SerializedBytes);
	const FString CanonicalIdentity = FAvidScriptHash::Sha256Hex(Bytecode);
	const FAvidScriptVmArtifactView VerifiedArtifact =
		FAvidScriptVmArtifactView::FromWasmtimeSerialized(
			SerializedBytes,
			Bytecode,
			SerializedIdentity,
			CanonicalIdentity,
			CompilerBuildIdentity,
			TargetTriple,
			EAvidScriptVmArtifactTrust::VerifiedPackage);

	FAvidScriptVmArtifactView InvalidArtifact = VerifiedArtifact;
	InvalidArtifact.Trust = EAvidScriptVmArtifactTrust::Untrusted;
	TestFalse(
		TEXT("untrusted serialized artifact is rejected"),
		PrecompiledBackend->LoadArtifact(
			InvalidArtifact,
			TEXT("wasmtime_precompiled_untrusted"),
			Config,
			Error));
	TestEqual(
		TEXT("untrusted artifact category"),
		Error.Category,
		FString(TEXT("artifact_untrusted")));

	InvalidArtifact = VerifiedArtifact;
	InvalidArtifact.ExecutionIdentity = TEXT("invalid-sha256");
	TestFalse(
		TEXT("serialized artifact with a mismatched digest is rejected"),
		PrecompiledBackend->LoadArtifact(
			InvalidArtifact,
			TEXT("wasmtime_precompiled_digest_mismatch"),
			Config,
			Error));
	TestEqual(
		TEXT("digest mismatch category"),
		Error.Category,
		FString(TEXT("artifact_identity_mismatch")));

	InvalidArtifact = VerifiedArtifact;
	InvalidArtifact.CompilerBuildIdentity = TEXT("foreign-wasmtime-build");
	TestFalse(
		TEXT("serialized artifact from another compiler build is rejected"),
		PrecompiledBackend->LoadArtifact(
			InvalidArtifact,
			TEXT("wasmtime_precompiled_compiler_mismatch"),
			Config,
			Error));
	TestEqual(
		TEXT("compiler mismatch category"),
		Error.Category,
		FString(TEXT("artifact_compiler_mismatch")));

	if (!TestTrue(
		TEXT("verified serialized artifact loads"),
		PrecompiledBackend->LoadArtifact(
			VerifiedArtifact,
			TEXT("wasmtime_precompiled_verified"),
			Config,
			Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	TestTrue(
		TEXT("serialized module load time is measured"),
		PrecompiledBackend->GetLoadMetrics().ModuleLoadMs > 0.0);
	FAvidScriptVmExportHandle BeginPlayHandle;
	TestTrue(
		TEXT("precompiled BeginPlay export resolves"),
		PrecompiledBackend->ResolveExport(
			TEXT("avid_on_begin_play"),
			BeginPlayHandle,
			Error));
	TestTrue(
		TEXT("precompiled BeginPlay export executes"),
		PrecompiledBackend->Call(
			BeginPlayHandle,
			FAvidScriptVmCallFrame(),
			Error));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimeArtifactCompilerTest,
	"AvidScript.VM.Wasmtime.ArtifactCompiler",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeArtifactCompilerTest::RunTest(
	const FString& Parameters)
{
	static uint32 ArtifactCompilerTestNonce = 0;
	TArray<uint8> Bytecode = BuildWasmtimeLifecycleFixture();
	TArray<uint8> NonceSection;
	AppendWasmtimeString(NonceSection, "avidscript_artifact_compiler_test");
	AppendWasmtimeU32Leb(NonceSection, ++ArtifactCompilerTestNonce);
	AppendWasmtimeSection(Bytecode, 0, NonceSection);
	FAvidScriptVmArtifactCompileRequest Request;
	Request.Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Request.Selection.ExecutionMode = EAvidScriptVmExecutionMode::Aot;
	Request.Selection.ArtifactFormat =
		EAvidScriptVmArtifactFormat::WasmtimeSerialized;
	Request.CanonicalWasmBytes = Bytecode;

	FAvidScriptVmArtifactCompileResult FirstResult;
#if !AVIDSCRIPT_WITH_WASMTIME
	TestFalse(
		TEXT("artifact compiler is unavailable without Wasmtime"),
		CompileAvidScriptVmArtifact(Request, FirstResult));
	TestEqual(
		TEXT("unavailable artifact compiler category"),
		FirstResult.Error.Category,
		FString(TEXT("backend_unavailable")));
	return true;
#else
	if (!TestTrue(
			TEXT("production Wasmtime artifact compiles"),
			CompileAvidScriptVmArtifact(Request, FirstResult)))
	{
		AddError(
			FirstResult.Error.Category
			+ TEXT(": ")
			+ FirstResult.Error.Details);
		return false;
	}
	TestTrue(TEXT("compile result succeeds"), FirstResult.bSucceeded);
	TestFalse(TEXT("first compile is a cache miss"), FirstResult.bCacheHit);
	TestFalse(
		TEXT("serialized artifact bytes are present"),
		FirstResult.Artifact.ExecutionBytes.IsEmpty());
	TestEqual(
		TEXT("canonical identity is exact"),
		FirstResult.Artifact.CanonicalWasmIdentity,
		FAvidScriptHash::Sha256Hex(Bytecode));
	TestEqual(
		TEXT("attestation id has fixed width"),
		FirstResult.Artifact.AttestationId.Len(),
		32);
	TestEqual(
		TEXT("attestation id is lowercase"),
		FirstResult.Artifact.AttestationId,
		FirstResult.Artifact.AttestationId.ToLower());
	TestTrue(
		TEXT("exact compiled tuple is authorized"),
		AuthorizeAvidScriptVmArtifact(
			FirstResult.Artifact.AttestationId,
			FirstResult.Artifact));

	FAvidScriptVmArtifactCompileResult CachedResult;
	if (!TestTrue(
			TEXT("same-process artifact compile succeeds"),
			CompileAvidScriptVmArtifact(Request, CachedResult)))
	{
		AddError(
			CachedResult.Error.Category
			+ TEXT(": ")
			+ CachedResult.Error.Details);
		return false;
	}
	TestTrue(TEXT("second compile is a cache hit"), CachedResult.bCacheHit);
	TestNotEqual(
		TEXT("cache hit receives a fresh attestation"),
		CachedResult.Artifact.AttestationId,
		FirstResult.Artifact.AttestationId);
	TestTrue(
		TEXT("cached tuple is authorized"),
		AuthorizeAvidScriptVmArtifact(
			CachedResult.Artifact.AttestationId,
			CachedResult.Artifact));

	FAvidScriptVmOwnedArtifact MutatedArtifact = CachedResult.Artifact;
	MutatedArtifact.ExecutionIdentity = TEXT("invalid-execution-sha256");
	TestFalse(
		TEXT("changed execution identity is rejected"),
		AuthorizeAvidScriptVmArtifact(
			CachedResult.Artifact.AttestationId,
			MutatedArtifact));

	MutatedArtifact = CachedResult.Artifact;
	MutatedArtifact.CompilerBuildIdentity += TEXT("-foreign");
	TestFalse(
		TEXT("changed compiler identity is rejected"),
		AuthorizeAvidScriptVmArtifact(
			CachedResult.Artifact.AttestationId,
			MutatedArtifact));

	MutatedArtifact = CachedResult.Artifact;
	MutatedArtifact.TargetTriple = TEXT("foreign-target");
	TestFalse(
		TEXT("changed target triple is rejected"),
		AuthorizeAvidScriptVmArtifact(
			CachedResult.Artifact.AttestationId,
			MutatedArtifact));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimeWideParameterExportTest,
	"AvidScript.VM.Wasmtime.WideParameterExport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeWideParameterExportTest::RunTest(
	const FString& Parameters)
{
#if !AVIDSCRIPT_WITH_WASMTIME
	return true;
#else
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend =
		CreateWasmtimeBackendForTest(Error);
	FAvidScriptVmLoadConfig Config;
	const TArray<uint8> Bytecode =
		BuildWasmtimeWideParameterFixture();
	if (!LoadWasmtimeTestModule(
			*this,
			*Backend,
			Bytecode,
			Config,
			Error))
	{
		return false;
	}

	FAvidScriptVmExportHandle Handle;
	TestFalse(
		TEXT("export wider than the fixed call frame rejects at resolution"),
		Backend->ResolveExport(TEXT("wide"), Handle, Error));
	TestEqual(
		TEXT("wide export reports invalid_export"),
		Error.Category,
		FString(TEXT("invalid_export")));
	TestFalse(
		TEXT("wide export never publishes a resolvable handle"),
		Handle.IsValid());
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
	using namespace AvidScriptVmResultFixture;

	FAvidScriptVmError Error;
	FAvidScriptVmLoadConfig Config;
	TUniquePtr<IAvidScriptVmBackend> Backend =
		CreateWasmtimeBackendForTest(Error);

	const TArray<uint8> VoidFixture = Build(TConstArrayView<FValue>());
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

	const TArray<uint8> I32Fixture = BuildSingle(EValueKind::I32, 7);
	if (!LoadWasmtimeTestModule(*this, *Backend, I32Fixture, Config, Error))
	{
		return false;
	}
	TestTrue(
		TEXT("i32 result export resolves"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	FAvidScriptVmPreparedExportCall PreparedResult;
	TestTrue(
		TEXT("i32 result export prepares"),
		Backend->PrepareExportCall(Handle, PreparedResult, Error));
	TestEqual(
		TEXT("prepared i32 result count is exact"),
		PreparedResult.ResultCellCount,
		1u);
	TestTrue(
		TEXT("i32 result export calls"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error, &Result));
	TestEqual(TEXT("i32 result has one cell"), Result.CellCount, 1u);
	TestEqual(TEXT("i32 result value is preserved"), Result.Cells[0], 7u);
	TestTrue(
		TEXT("prepared i32 result export calls"),
		PreparedResult.Call(
			FAvidScriptVmCallFrame(),
			Error,
			&Result));
	TestEqual(
		TEXT("prepared i32 result value is preserved"),
		Result.Cells[0],
		7u);

	const uint64 I64Bits = 0x0123456789abcdefULL;
	const TArray<uint8> I64Fixture =
		BuildSingle(EValueKind::I64, I64Bits);
	if (!LoadWasmtimeTestModule(*this, *Backend, I64Fixture, Config, Error))
	{
		return false;
	}
	TestTrue(
		TEXT("i64 result export resolves"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	TestTrue(
		TEXT("i64 result export calls"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error, &Result));
	TestEqual(TEXT("i64 result has two cells"), Result.CellCount, 2u);
	TestEqual(
		TEXT("i64 low cell is first"),
		Result.Cells[0],
		static_cast<uint32>(I64Bits));
	TestEqual(
		TEXT("i64 high cell is second"),
		Result.Cells[1],
		static_cast<uint32>(I64Bits >> 32));

	const double F64Value = -123.5;
	uint64 F64Bits = 0;
	FMemory::Memcpy(&F64Bits, &F64Value, sizeof(F64Bits));
	const TArray<uint8> F64Fixture =
		BuildSingle(EValueKind::F64, F64Bits);
	if (!LoadWasmtimeTestModule(*this, *Backend, F64Fixture, Config, Error))
	{
		return false;
	}
	TestTrue(
		TEXT("f64 result export resolves"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	TestTrue(
		TEXT("f64 result export calls"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error, &Result));
	TestEqual(TEXT("f64 result has two cells"), Result.CellCount, 2u);
	TestEqual(
		TEXT("f64 low bits cell is first"),
		Result.Cells[0],
		static_cast<uint32>(F64Bits));
	TestEqual(
		TEXT("f64 high bits cell is second"),
		Result.Cells[1],
		static_cast<uint32>(F64Bits >> 32));

	const float F32Value = -3.25f;
	uint32 F32Bits = 0;
	FMemory::Memcpy(&F32Bits, &F32Value, sizeof(F32Bits));
	const TArray<uint8> F32Fixture =
		BuildSingle(EValueKind::F32, F32Bits);
	if (!LoadWasmtimeTestModule(*this, *Backend, F32Fixture, Config, Error))
	{
		return false;
	}
	TestTrue(
		TEXT("f32 result export resolves"),
		Backend->ResolveExport(TEXT("result_test"), Handle, Error));
	TestTrue(
		TEXT("f32 result export calls"),
		Backend->Call(Handle, FAvidScriptVmCallFrame(), Error, &Result));
	TestEqual(TEXT("f32 result has one cell"), Result.CellCount, 1u);
	TestEqual(TEXT("f32 result bits are preserved"), Result.Cells[0], F32Bits);

	TArray<FValue> OversizeResults;
	for (uint32 Index = 0;
		Index < FAvidScriptVmCallResult::MaxCells + 1;
		++Index)
	{
		OversizeResults.Add(FValue{ EValueKind::I32, Index + 7 });
	}
	const TArray<uint8> OversizeFixture = Build(OversizeResults);
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
		BuildSingle(EValueKind::ExternRef, 0);
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
	FAvidScriptVmWasmtimeTypedGuestResultBridgeTest,
	"AvidScript.VM.Wasmtime.TypedGuestResultBridge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeTypedGuestResultBridgeTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptWasmtimeTypedGuestResultContext TargetContext;
	FAvidScriptVmPreparedTypedHostTarget PreparedTarget;
	PreparedTarget.Context = &TargetContext;
	PreparedTarget.SelfI32PairGuestResult =
		&InvokeWasmtimeTypedGuestResultForTest;
	TestTrue(
		TEXT("guest-result target binds its exact shape"),
		PreparedTarget.IsBoundForShape(
			EAvidScriptVmTypedHostShape::SelfI32PairToGuestI32));
	TestFalse(
		TEXT("guest-result target does not bind the legacy pair shape"),
		PreparedTarget.IsBoundForShape(
			EAvidScriptVmTypedHostShape::SelfI32PairToI32));
	TestTrue(
		TEXT("guest-result target participates in any-target validation"),
		PreparedTarget.HasAnyTarget());

#if !AVIDSCRIPT_WITH_WASMTIME
	return true;
#else
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend =
		CreateWasmtimeBackendForTest(Error);
	if (!TestNotNull(
			TEXT("Wasmtime typed guest-result backend is created"),
			Backend.Get()))
	{
		return false;
	}

	FAvidScriptVmBindingPackage Package =
		MakeWasmtimeTypedGuestResultPackage();
	FAvidScriptVmTypedHostImport Import;
	Import.StableId = WasmtimeTypedGuestResultStableId;
	Import.BindingOrdinal = 0;
	Import.ModuleName = TEXT("avidscript");
	Import.ImportName = WasmtimeTypedGuestResultImportName;
	Import.Signature = TEXT("(iiiii)i");
	Import.Shape =
		EAvidScriptVmTypedHostShape::SelfI32PairToGuestI32;
	Import.PreparedTarget = PreparedTarget;
	TArray<FAvidScriptVmTypedHostImport> Imports = { Import };
	FAvidScriptWasmtimeTypedBridgeDispatcher Dispatcher;
	FAvidScriptVmLoadConfig Config;
	Config.BindingPackage = &Package;
	Config.TypedHostDispatcher = &Dispatcher;
	Config.TypedHostImports = Imports;

	if (!TestTrue(
			TEXT("typed guest-result fixture loads"),
			Backend->Load(
				BuildWasmtimeSelfI32PairGuestResultFixture(),
				TEXT("typed_guest_result"),
				Config,
				Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}

	FAvidScriptVmExportHandle RunHandle;
	if (!TestTrue(
			TEXT("typed guest-result export resolves"),
			Backend->ResolveExport(TEXT("run"), RunHandle, Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	FAvidScriptVmCallResult Result;
	TestTrue(
		TEXT("typed guest-result success returns"),
		Backend->Call(
			RunHandle,
			FAvidScriptVmCallFrame(),
			Error,
			&Result));
	TestEqual(TEXT("success returns one status cell"), Result.CellCount, 1u);
	TestEqual(
		TEXT("success preserves the exact target status"),
		static_cast<int32>(Result.Cells[0]),
		1);
	TestEqual(TEXT("prepared target is called once"), TargetContext.CallCount, 1);
	TestEqual(TEXT("self slot is forwarded"), TargetContext.SelfSlot, 7);
	TestEqual(
		TEXT("self generation is forwarded"),
		TargetContext.SelfGeneration,
		11);
	TestEqual(TEXT("left operand is forwarded"), TargetContext.Left, 13);
	TestEqual(TEXT("right operand is forwarded"), TargetContext.Right, 17);
	TestEqual(
		TEXT("guest address is forwarded"),
		TargetContext.GuestAddress,
		64);

	TargetContext.Status = EAvidScriptVmTypedHostStatus::Rejected;
	TargetContext.OutStatus = 99;
	TestFalse(
		TEXT("typed guest-result rejection traps"),
		Backend->Call(
			RunHandle,
			FAvidScriptVmCallFrame(),
			Error,
			&Result));
	TestEqual(
		TEXT("rejection category is host import failure"),
		Error.Category,
		FString(TEXT("host_import_failed")));
	TestEqual(
		TEXT("rejection identifies the typed import"),
		Error.ImportName,
		FString(WasmtimeTypedGuestResultImportName));
	TestEqual(
		TEXT("rejected target is called exactly once more"),
		TargetContext.CallCount,
		2);
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
	FAvidScriptVmPreparedExportCall PreparedTrap;
	TestTrue(
		TEXT("trap export prepares"),
		Backend->PrepareExportCall(TrapHandle, PreparedTrap, Error));
	TestFalse(TEXT("guest trap fails"), Backend->Call(TrapHandle, FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("guest trap category"), Error.Category, FString(TEXT("trap")));
	TestFalse(TEXT("guest trap details are nonempty"), Error.Details.IsEmpty());
	TestTrue(TEXT("guest trap has structured frames"), !Error.StackFrames.IsEmpty());
	TestFalse(
		TEXT("prepared guest trap fails through the common diagnostic core"),
		PreparedTrap.Call(FAvidScriptVmCallFrame(), Error));
	TestEqual(
		TEXT("prepared guest trap category"),
		Error.Category,
		FString(TEXT("trap")));
	TestTrue(
		TEXT("prepared guest trap has structured frames"),
		!Error.StackFrames.IsEmpty());

	FAvidScriptWasmtimeTestDispatcher Dispatcher;
	Config.HostDispatcher = &Dispatcher;
	const TArray<uint8> AddFixture = BuildWasmtimeI32ImportFixture("host_add_i32", 3);
	TestTrue(TEXT("reentrant fixture loads"), Backend->Load(AddFixture, TEXT("wasmtime_reentrant"), Config, Error));
	FAvidScriptVmExportHandle BeginHandle;
	TestTrue(TEXT("reentrant BeginPlay resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginHandle, Error));
	FAvidScriptVmPreparedExportCall PreparedBegin;
	TestTrue(
		TEXT("reentrant BeginPlay prepares"),
		Backend->PrepareExportCall(BeginHandle, PreparedBegin, Error));
	Dispatcher.BackendToUnload = Backend.Get();
	TestFalse(
		TEXT("prepared reentrant unload fails active call safely"),
		PreparedBegin.Call(FAvidScriptVmCallFrame(), Error));
	TestEqual(TEXT("reentrant unload category"), Error.Category, FString(TEXT("reentrant_unload")));
	TestFalse(TEXT("deferred unload completes"), Backend->IsLoaded());
	return true;
#endif
}

#endif
