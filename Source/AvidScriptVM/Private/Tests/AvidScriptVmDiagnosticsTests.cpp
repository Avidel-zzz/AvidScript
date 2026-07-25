#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWamrCallStack.h"

#include "AvidScriptVmBackend.h"
#include "AvidScriptWasmModuleLayout.h"
#include "Misc/AutomationTest.h"

namespace
{
void AppendDiagnosticU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendDiagnosticString(TArray<uint8>& Bytes, const char* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	AppendDiagnosticU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value[Index]));
	}
}

void AppendDiagnosticSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendDiagnosticU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

TArray<uint8> BuildDiagnosticTrapFixture()
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendDiagnosticU32Leb(Types, 2);
	const uint8 EmptyFunctionType[] = { 0x60, 0x00, 0x00 };
	Types.Append(EmptyFunctionType, UE_ARRAY_COUNT(EmptyFunctionType));
	const uint8 TickFunctionType[] = { 0x60, 0x01, 0x7d, 0x00 };
	Types.Append(TickFunctionType, UE_ARRAY_COUNT(TickFunctionType));
	AppendDiagnosticSection(Module, 1, Types);

	TArray<uint8> Functions;
	AppendDiagnosticU32Leb(Functions, 3);
	AppendDiagnosticU32Leb(Functions, 0);
	AppendDiagnosticU32Leb(Functions, 0);
	AppendDiagnosticU32Leb(Functions, 1);
	AppendDiagnosticSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendDiagnosticU32Leb(Exports, 2);
	AppendDiagnosticString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	AppendDiagnosticU32Leb(Exports, 1);
	AppendDiagnosticString(Exports, "avid_on_tick");
	Exports.Add(0x00);
	AppendDiagnosticU32Leb(Exports, 2);
	AppendDiagnosticSection(Module, 7, Exports);

	TArray<uint8> Code;
	AppendDiagnosticU32Leb(Code, 3);
	const TArray<uint8> TrapHelper = { 0x00, 0x00, 0x0b };
	AppendDiagnosticU32Leb(Code, static_cast<uint32>(TrapHelper.Num()));
	Code.Append(TrapHelper);
	const TArray<uint8> BeginPlay = { 0x00, 0x0b };
	AppendDiagnosticU32Leb(Code, static_cast<uint32>(BeginPlay.Num()));
	Code.Append(BeginPlay);
	const TArray<uint8> Tick = { 0x00, 0x10, 0x00, 0x0b };
	AppendDiagnosticU32Leb(Code, static_cast<uint32>(Tick.Num()));
	Code.Append(Tick);
	AppendDiagnosticSection(Module, 10, Code);
	return Module;
}

TArray<uint8> BuildDiagnosticImportFixture()
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Imports;
	AppendDiagnosticU32Leb(Imports, 4);
	AppendDiagnosticString(Imports, "avidscript");
	AppendDiagnosticString(Imports, "same");
	Imports.Add(0x00);
	AppendDiagnosticU32Leb(Imports, 0);
	AppendDiagnosticString(Imports, "env");
	AppendDiagnosticString(Imports, "memory");
	Imports.Add(0x02);
	Imports.Add(0x00);
	AppendDiagnosticU32Leb(Imports, 1);
	AppendDiagnosticString(Imports, "env");
	AppendDiagnosticString(Imports, "other");
	Imports.Add(0x00);
	AppendDiagnosticU32Leb(Imports, 0);
	AppendDiagnosticString(Imports, "avidscript");
	AppendDiagnosticString(Imports, "same");
	Imports.Add(0x00);
	AppendDiagnosticU32Leb(Imports, 0);
	AppendDiagnosticSection(Module, 2, Imports);
	return Module;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmCallStackParserTest,
	"AvidScript.VM.Diagnostics.CallStackParser",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmCallStackParserTest::RunTest(const FString& Parameters)
{
	TArray<FAvidScriptVmStackFrame> Frames;
	TestTrue(
		TEXT("valid WAMR stack parses"),
		ParseAvidScriptWamrCallStack(
			TEXT("\n#00: 0x002a - $f7\n#01: 0x0004 - avid_on_tick\n\n"),
			Frames));
	TestEqual(TEXT("two frames are retained"), Frames.Num(), 2);
	if (Frames.Num() == 2)
	{
		TestEqual(TEXT("numeric frame index"), Frames[0].FunctionIndex, 7u);
		TestEqual(TEXT("numeric frame offset"), Frames[0].FunctionOffset, 42u);
		TestEqual(TEXT("numeric raw token"), Frames[0].RawFunctionToken, FString(TEXT("$f7")));
		TestEqual(TEXT("named frame index remains unknown"), Frames[1].FunctionIndex, MAX_uint32);
		TestEqual(TEXT("named raw token is retained"), Frames[1].RawFunctionToken, FString(TEXT("avid_on_tick")));
	}

	TestFalse(
		TEXT("duplicate frame ordinal is rejected"),
		ParseAvidScriptWamrCallStack(TEXT("#00: 0x0001 - $f1\n#00: 0x0002 - $f2\n"), Frames));
	TestTrue(TEXT("failed parse clears partial frames"), Frames.IsEmpty());
	TestFalse(
		TEXT("invalid hexadecimal offset is rejected"),
		ParseAvidScriptWamrCallStack(TEXT("#00: 0xzzzz - $f1\n"), Frames));
	TestFalse(TEXT("empty call stack text is rejected"), ParseAvidScriptWamrCallStack(FString(), Frames));

	FString Oversized;
	for (int32 Index = 0; Index < 129; ++Index)
	{
		Oversized += FString::Printf(TEXT("#%02d: 0x0000 - $f%d\n"), Index, Index);
	}
	TestFalse(TEXT("oversized frame count is rejected"), ParseAvidScriptWamrCallStack(Oversized, Frames));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmModuleLayoutTest,
	"AvidScript.VM.Diagnostics.ModuleLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmModuleLayoutTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Bytecode = BuildDiagnosticTrapFixture();
	FAvidScriptWasmModuleLayout Layout;
	FString Error;
	TestTrue(
		TEXT("valid WASM function layout is inspected"),
		InspectAvidScriptWasmModuleLayout(MakeArrayView(Bytecode), Layout, Error));
	TestEqual(TEXT("fixture has no function imports"), Layout.ImportedFunctionCount, 0u);
	TestEqual(TEXT("fixture defined function count"), Layout.DefinedFunctionCount, 3u);
	TestEqual(TEXT("fixture function export count"), Layout.FunctionExports.Num(), 2);
	if (Layout.FunctionExports.Num() == 2)
	{
		TestEqual(TEXT("BeginPlay export name"), Layout.FunctionExports[0].Name, FString(TEXT("avid_on_begin_play")));
		TestEqual(TEXT("BeginPlay export index"), Layout.FunctionExports[0].FunctionIndex, 1u);
		TestEqual(TEXT("Tick export name"), Layout.FunctionExports[1].Name, FString(TEXT("avid_on_tick")));
		TestEqual(TEXT("Tick export index"), Layout.FunctionExports[1].FunctionIndex, 2u);
	}

	TArray<uint8> Truncated = Bytecode;
	Truncated.Pop();
	TestFalse(
		TEXT("truncated WASM layout is rejected"),
		InspectAvidScriptWasmModuleLayout(MakeArrayView(Truncated), Layout, Error));
	TestTrue(TEXT("layout failure explains its source"), !Error.IsEmpty());

	const TArray<uint8> ImportBytecode = BuildDiagnosticImportFixture();
	TestTrue(
		TEXT("mixed WASM imports are inspected"),
		InspectAvidScriptWasmModuleLayout(MakeArrayView(ImportBytecode), Layout, Error));
	TestEqual(TEXT("only function imports are counted"), Layout.ImportedFunctionCount, 3u);
	TestEqual(TEXT("all function import identities are retained"), Layout.FunctionImports.Num(), 3);
	if (Layout.FunctionImports.Num() == 3)
	{
		TestEqual(TEXT("first function import module"), Layout.FunctionImports[0].ModuleName, FString(TEXT("avidscript")));
		TestEqual(TEXT("first function import name"), Layout.FunctionImports[0].ImportName, FString(TEXT("same")));
		TestEqual(TEXT("non-function import does not disturb order"), Layout.FunctionImports[1].ModuleName, FString(TEXT("env")));
		TestEqual(TEXT("second function import name"), Layout.FunctionImports[1].ImportName, FString(TEXT("other")));
		TestEqual(TEXT("repeated function import module is retained"), Layout.FunctionImports[2].ModuleName, FString(TEXT("avidscript")));
		TestEqual(TEXT("repeated function import is retained"), Layout.FunctionImports[2].ImportName, FString(TEXT("same")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptVmTrapCallStackTest,
	"AvidScript.VM.Diagnostics.TrapCallStack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptVmTrapCallStackTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Bytecode = BuildDiagnosticTrapFixture();
	TUniquePtr<IAvidScriptVmBackend> Backend = CreateAvidScriptWamrBackend();
	FAvidScriptVmError Error;
	FAvidScriptVmLoadConfig Config;
	if (!TestTrue(TEXT("diagnostic fixture loads"), Backend->Load(MakeArrayView(Bytecode), TEXT("vm_diagnostic_trap"), Config, Error)))
	{
		AddError(Error.Category + TEXT(": ") + Error.Details);
		return false;
	}

	FAvidScriptVmExportHandle BeginPlayHandle;
	FAvidScriptVmExportHandle TickHandle;
	TestTrue(TEXT("BeginPlay resolves"), Backend->ResolveExport(TEXT("avid_on_begin_play"), BeginPlayHandle, Error));
	TestTrue(TEXT("Tick resolves"), Backend->ResolveExport(TEXT("avid_on_tick"), TickHandle, Error));
	FAvidScriptVmCallFrame EmptyFrame;
	TestTrue(TEXT("healthy BeginPlay succeeds"), Backend->Call(BeginPlayHandle, EmptyFrame, Error));
	TestTrue(TEXT("healthy BeginPlay has no stack frames"), Error.StackFrames.IsEmpty());

	FAvidScriptVmCallFrame TickFrame;
	TickFrame.CellCount = 1;
	float DeltaSeconds = 1.0f / 60.0f;
	FMemory::Memcpy(TickFrame.Cells, &DeltaSeconds, sizeof(float));
	TestFalse(TEXT("Tick trap is returned"), Backend->Call(TickHandle, TickFrame, Error));
	TestEqual(TEXT("Tick failure category"), Error.Category, FString(TEXT("trap")));
	TestTrue(TEXT("Tick trap retains frames"), !Error.StackFrames.IsEmpty());
	TestTrue(
		TEXT("Tick trap retains a numeric helper function index"),
		Error.StackFrames.ContainsByPredicate([](const FAvidScriptVmStackFrame& Frame)
		{
			return Frame.FunctionIndex != MAX_uint32;
		}));
	return true;
}

#endif
