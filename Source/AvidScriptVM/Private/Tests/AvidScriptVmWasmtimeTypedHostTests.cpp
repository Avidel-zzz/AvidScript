#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptVmBackend.h"

#include "Misc/AutomationTest.h"

#ifndef AVIDSCRIPT_WITH_WASMTIME
#define AVIDSCRIPT_WITH_WASMTIME 0
#endif

namespace
{
constexpr const TCHAR* TypedImportName = TEXT("avid_s1_1111111111111111");
constexpr const TCHAR* TypedStableId =
	TEXT("1111111111111111111111111111111111111111111111111111111111111111");

void AppendTypedU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendTypedI32Leb(TArray<uint8>& Bytes, int32 Value)
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

void AppendTypedString(TArray<uint8>& Bytes, const char* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	AppendTypedU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value[Index]));
	}
}

void AppendTypedSection(TArray<uint8>& Module, uint8 Id, const TArray<uint8>& Payload)
{
	Module.Add(Id);
	AppendTypedU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendTypedI32Const(TArray<uint8>& Bytes, int32 Value)
{
	Bytes.Add(0x41);
	AppendTypedI32Leb(Bytes, Value);
}

TArray<uint8> BuildTypedHostFixture(bool bPairImport, int32 Left = 0, int32 Right = 0)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendTypedU32Leb(Types, 2);
	if (bPairImport)
	{
		const uint8 PairType[] = { 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f };
		Types.Append(PairType, UE_ARRAY_COUNT(PairType));
	}
	else
	{
		const uint8 EmptyResultType[] = { 0x60, 0x00, 0x01, 0x7f };
		Types.Append(EmptyResultType, UE_ARRAY_COUNT(EmptyResultType));
	}
	const uint8 ExportType[] = { 0x60, 0x00, 0x01, 0x7f };
	Types.Append(ExportType, UE_ARRAY_COUNT(ExportType));
	AppendTypedSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendTypedU32Leb(Imports, 1);
	AppendTypedString(Imports, "avidscript");
	AppendTypedString(Imports, "avid_s1_1111111111111111");
	Imports.Add(0x00);
	AppendTypedU32Leb(Imports, 0);
	AppendTypedSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendTypedU32Leb(Functions, 1);
	AppendTypedU32Leb(Functions, 1);
	AppendTypedSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendTypedU32Leb(Exports, 1);
	AppendTypedString(Exports, "run");
	Exports.Add(0x00);
	AppendTypedU32Leb(Exports, 1);
	AppendTypedSection(Module, 7, Exports);

	TArray<uint8> Body;
	Body.Add(0x00);
	if (bPairImport)
	{
		AppendTypedI32Const(Body, Left);
		AppendTypedI32Const(Body, Right);
	}
	Body.Add(0x10);
	AppendTypedU32Leb(Body, 0);
	Body.Add(0x0b);
	TArray<uint8> Code;
	AppendTypedU32Leb(Code, 1);
	AppendTypedU32Leb(Code, static_cast<uint32>(Body.Num()));
	Code.Append(Body);
	AppendTypedSection(Module, 10, Code);
	return Module;
}

FAvidScriptVmBindingPackage MakeTypedBindingPackage(
	const FString& Signature = TEXT("(ii)i"),
	uint32 Ordinal = 7)
{
	FAvidScriptVmBindingPackage Package;
	Package.PackageName = TEXT("avidscript.phase54.typed_host");
	Package.PackageHash = FString::ChrN(64, TEXT('1'));
	FAvidScriptVmDynamicImport Import;
	Import.StableId = TypedStableId;
	Import.Ordinal = Ordinal;
	Import.ModuleName = TEXT("avidscript");
	Import.ImportName = TypedImportName;
	Import.Signature = Signature;
	Package.Imports.Add(MoveTemp(Import));
	return Package;
}

FAvidScriptVmTypedHostImport MakeTypedImport(
	EAvidScriptVmTypedHostShape Shape = EAvidScriptVmTypedHostShape::I32PairToI32,
	const FString& Signature = TEXT("(ii)i"),
	uint32 Ordinal = 7)
{
	FAvidScriptVmTypedHostImport Import;
	Import.StableId = TypedStableId;
	Import.BindingOrdinal = Ordinal;
	Import.ModuleName = TEXT("avidscript");
	Import.ImportName = TypedImportName;
	Import.Signature = Signature;
	Import.Shape = Shape;
	return Import;
}

TUniquePtr<IAvidScriptVmBackend> CreateTypedWasmtimeBackend(FAvidScriptVmError& OutError)
{
	FAvidScriptVmBackendSelection Selection;
	Selection.BackendKind = EAvidScriptVmBackendKind::Wasmtime;
	Selection.ExecutionMode = EAvidScriptVmExecutionMode::Jit;
	Selection.ArtifactFormat = EAvidScriptVmArtifactFormat::WasmBytecode;
	return CreateAvidScriptVmBackend(Selection, OutError);
}

class FTypedHostDispatcher final : public IAvidScriptVmTypedHostDispatcher
{
public:
	EAvidScriptVmTypedHostStatus DispatchEmptyI32(
		uint32 BindingOrdinal,
		int32& OutValue) override
	{
		LastOrdinal = BindingOrdinal;
		OutValue = Bias + 5;
		return Status;
	}

	EAvidScriptVmTypedHostStatus DispatchI32PairToI32(
		uint32 BindingOrdinal,
		int32 Left,
		int32 Right,
		int32& OutValue) override
	{
		LastOrdinal = BindingOrdinal;
		LastLeft = Left;
		LastRight = Right;
		OutValue = Left + Right + Bias;
		return Status;
	}

	EAvidScriptVmTypedHostStatus DispatchSelfI32PairToI32(
		uint32,
		int32,
		int32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::FallbackRequired;
	}

	EAvidScriptVmTypedHostStatus DispatchSelfPropertyI32GetSet(
		uint32,
		int32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::FallbackRequired;
	}

	EAvidScriptVmTypedHostStatus DispatchSelfVectorValue(
		uint32,
		int32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::FallbackRequired;
	}

	EAvidScriptVmTypedHostStatus DispatchStableObjectRoundtrip(
		uint32,
		int32,
		int32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::FallbackRequired;
	}

	EAvidScriptVmTypedHostStatus DispatchCommandBufferSubmit(
		uint32,
		int32,
		int32,
		int32&) override
	{
		return EAvidScriptVmTypedHostStatus::FallbackRequired;
	}

	EAvidScriptVmTypedHostStatus Status = EAvidScriptVmTypedHostStatus::Succeeded;
	uint32 LastOrdinal = MAX_uint32;
	int32 LastLeft = 0;
	int32 LastRight = 0;
	int32 Bias = 0;
};

bool ResolveAndCallTypedRun(
	FAutomationTestBase& Test,
	IAvidScriptVmBackend& Backend,
	int32 Expected,
	FAvidScriptVmError& OutError)
{
	FAvidScriptVmExportHandle Handle;
	if (!Test.TestTrue(TEXT("typed run resolves"), Backend.ResolveExport(TEXT("run"), Handle, OutError)))
	{
		return false;
	}
	FAvidScriptVmCallFrame Frame;
	FAvidScriptVmCallResult Result;
	if (!Test.TestTrue(TEXT("typed run succeeds"), Backend.Call(Handle, Frame, OutError, &Result)))
	{
		return false;
	}
	Test.TestEqual(TEXT("typed run returns one cell"), Result.CellCount, 1u);
	Test.TestEqual(TEXT("typed run result"), static_cast<int32>(Result.Cells[0]), Expected);
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmWasmtimeTypedHostTest,
	"AvidScript.VM.Wasmtime.TypedHost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmWasmtimeTypedHostTest::RunTest(const FString& Parameters)
{
	FAvidScriptVmError Error;
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateTypedWasmtimeBackend(Error);
#if !AVIDSCRIPT_WITH_WASMTIME
	TestNull(TEXT("Wasmtime typed host is unavailable without the managed dependency"), Backend.Get());
	return true;
#else
	if (!TestNotNull(TEXT("Wasmtime typed backend is created"), Backend.Get()))
	{
		return false;
	}

	const TArray<uint8> PairFixture = BuildTypedHostFixture(true, 17, 19);
	FAvidScriptVmBindingPackage Package = MakeTypedBindingPackage();
	TArray<FAvidScriptVmTypedHostImport> Imports = { MakeTypedImport() };
	FTypedHostDispatcher Dispatcher;
	Dispatcher.Bias = 3;
	FAvidScriptVmLoadConfig Config;
	Config.BindingPackage = &Package;
	Config.TypedHostDispatcher = &Dispatcher;
	Config.TypedHostImports = Imports;
	if (!TestTrue(TEXT("typed pair fixture loads"), Backend->Load(PairFixture, TEXT("typed_pair"), Config, Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}
	ResolveAndCallTypedRun(*this, *Backend, 39, Error);
	TestEqual(TEXT("typed ordinal is instance-local"), Dispatcher.LastOrdinal, 7u);
	TestEqual(TEXT("typed left is direct"), Dispatcher.LastLeft, 17);
	TestEqual(TEXT("typed right is direct"), Dispatcher.LastRight, 19);

	Dispatcher.Status = EAvidScriptVmTypedHostStatus::Rejected;
	FAvidScriptVmExportHandle RejectHandle;
	TestTrue(TEXT("reject export resolves"), Backend->ResolveExport(TEXT("run"), RejectHandle, Error));
	FAvidScriptVmCallFrame EmptyFrame;
	TestFalse(TEXT("typed rejection traps"), Backend->Call(RejectHandle, EmptyFrame, Error));
	TestEqual(TEXT("typed rejection category"), Error.Category, FString(TEXT("host_import_failed")));
	TestEqual(TEXT("typed rejection import"), Error.ImportName, FString(TypedImportName));

	FAvidScriptVmLoadConfig MissingDispatcherConfig;
	MissingDispatcherConfig.BindingPackage = &Package;
	MissingDispatcherConfig.TypedHostImports = Imports;
	TestFalse(TEXT("missing typed dispatcher rejects load"), Backend->Load(
		PairFixture,
		TEXT("typed_missing_dispatcher"),
		MissingDispatcherConfig,
		Error));
	TestEqual(TEXT("missing dispatcher category"), Error.Category, FString(TEXT("typed_host_config_invalid")));

	TArray<FAvidScriptVmTypedHostImport> DuplicateImports = { Imports[0], Imports[0] };
	Config.TypedHostImports = DuplicateImports;
	TestFalse(TEXT("duplicate typed identity rejects load"), Backend->Load(
		PairFixture,
		TEXT("typed_duplicate"),
		Config,
		Error));
	TestEqual(TEXT("duplicate category"), Error.Category, FString(TEXT("typed_host_identity_conflict")));

	TArray<FAvidScriptVmTypedHostImport> InvalidImports = {
		MakeTypedImport(EAvidScriptVmTypedHostShape::I32PairToI32, TEXT("(i)i"))
	};
	Config.TypedHostImports = InvalidImports;
	TestFalse(TEXT("wrong typed metadata signature rejects load"), Backend->Load(
		PairFixture,
		TEXT("typed_wrong_metadata"),
		Config,
		Error));
	TestEqual(TEXT("wrong metadata category"), Error.Category, FString(TEXT("typed_host_contract_invalid")));

	Config.TypedHostImports = Imports;
	const TArray<uint8> WrongArityFixture = BuildTypedHostFixture(false);
	TestFalse(TEXT("wrong actual import arity rejects load"), Backend->Load(
		WrongArityFixture,
		TEXT("typed_wrong_arity"),
		Config,
		Error));
	TestEqual(TEXT("wrong arity reaches Wasmtime type validation"), Error.Category, FString(TEXT("instantiate_failed")));

	FTypedHostDispatcher FirstDispatcher;
	FTypedHostDispatcher SecondDispatcher;
	FirstDispatcher.Bias = 1;
	SecondDispatcher.Bias = 100;
	FAvidScriptVmLoadConfig FirstConfig = Config;
	FAvidScriptVmLoadConfig SecondConfig = Config;
	FirstConfig.TypedHostDispatcher = &FirstDispatcher;
	SecondConfig.TypedHostDispatcher = &SecondDispatcher;
	TUniquePtr<IAvidScriptVmBackend> FirstBackend = CreateTypedWasmtimeBackend(Error);
	TUniquePtr<IAvidScriptVmBackend> SecondBackend = CreateTypedWasmtimeBackend(Error);
	TestTrue(TEXT("first typed instance loads"), FirstBackend->Load(
		PairFixture,
		TEXT("typed_first"),
		FirstConfig,
		Error));
	TestTrue(TEXT("second typed instance loads"), SecondBackend->Load(
		PairFixture,
		TEXT("typed_second"),
		SecondConfig,
		Error));
	ResolveAndCallTypedRun(*this, *FirstBackend, 37, Error);
	ResolveAndCallTypedRun(*this, *SecondBackend, 136, Error);
	return true;
#endif
}

#endif
