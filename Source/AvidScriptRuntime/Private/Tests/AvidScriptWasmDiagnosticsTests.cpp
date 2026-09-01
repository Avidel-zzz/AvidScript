#if WITH_DEV_AUTOMATION_TESTS

#include "Diagnostics/AvidScriptWasmDebugMap.h"

#include "AvidScriptRuntimeSession.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Ssl.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
const FString GDiagnosticsSourceSha = FString::ChrN(64, TEXT('a'));
const FString GDiagnosticsSemanticSha = FString::ChrN(64, TEXT('b'));
const FString GDiagnosticsGuestIrSha = FString::ChrN(64, TEXT('c'));
const FString GDiagnosticsArtifactWrongSha = FString::ChrN(64, TEXT('d'));
const FString GDiagnosticsWasmSha = FString::ChrN(64, TEXT('e'));

FString BytesToDiagnosticsHex(const uint8* Bytes, int32 ByteCount)
{
	FString Hex;
	Hex.Reserve(ByteCount * 2);
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Hex += FString::Printf(TEXT("%02x"), Bytes[Index]);
	}
	return Hex;
}

FString ComputeDiagnosticsFileSha256(const FString& Path)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.IsEmpty())
	{
		return FString();
	}
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Digest);
	return BytesToDiagnosticsHex(Digest, UE_ARRAY_COUNT(Digest));
}

FString MakeDiagnosticsDebugMapJson(
	const FString& ModuleId,
	const FString& SourceFile,
	const FString& MapSourceSha,
	const FString& MapFrontendSha,
	const FString& MapSemanticSha,
	const FString& MapGuestIrSha,
	bool bDuplicateIndex = false,
	int32 FirstFunctionIndex = 7,
	int32 SecondFunctionIndex = 8,
	int32 ThirdFunctionIndex = INDEX_NONE,
	int32 ImportedFunctionCount = 7,
	int32 DefinedFunctionCount = 2)
{
	const FString OptionalBeginPlayFunction = ThirdFunctionIndex == INDEX_NONE
		? FString()
		: FString::Printf(
			TEXT("    { \"wasm_function_index\": %d, \"guest_function_id\": \"function:symbol:method:Test.BeginPlay()\", \"method_symbol_id\": \"symbol:method:Test.BeginPlay()\", \"display_name\": \"Test.BeginPlay()\", \"span\": { \"start\": 30, \"length\": 8, \"line\": 13, \"column\": 2, \"end_line\": 13, \"end_column\": 10 } },\n"),
			SecondFunctionIndex);
	const int32 TickFunctionIndex = ThirdFunctionIndex == INDEX_NONE ? SecondFunctionIndex : ThirdFunctionIndex;
	return FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"debug_version\": \"1.0\",\n")
		TEXT("  \"module_id\": \"%s\",\n")
		TEXT("  \"imported_function_count\": %d,\n")
		TEXT("  \"defined_function_count\": %d,\n")
		TEXT("  \"source\": { \"id\": \"%s\", \"sha256\": \"%s\" },\n")
		TEXT("  \"provenance\": { \"frontend_artifact_sha256\": \"%s\", \"semantic_sha256\": \"%s\", \"guest_ir_sha256\": \"%s\" },\n")
		TEXT("  \"functions\": [\n")
		TEXT("    { \"wasm_function_index\": %d, \"guest_function_id\": \"function:symbol:method:Test.Helper()\", \"method_symbol_id\": \"symbol:method:Test.Helper()\", \"display_name\": \"Test.Helper()\", \"span\": { \"start\": 10, \"length\": 20, \"line\": 10, \"column\": 2, \"end_line\": 12, \"end_column\": 3 } },\n")
		TEXT("%s")
		TEXT("    { \"wasm_function_index\": %d, \"guest_function_id\": \"function:symbol:method:Test.Tick(float)\", \"method_symbol_id\": \"symbol:method:Test.Tick(float)\", \"display_name\": \"Test.Tick(float)\", \"span\": { \"start\": 40, \"length\": 8, \"line\": 14, \"column\": 2, \"end_line\": 16, \"end_column\": 3 } }\n")
		TEXT("  ]\n")
		TEXT("}\n"),
		*ModuleId,
		ImportedFunctionCount,
		DefinedFunctionCount,
		*SourceFile,
		*MapSourceSha,
		*MapFrontendSha,
		*MapSemanticSha,
		*MapGuestIrSha,
		FirstFunctionIndex,
		*OptionalBeginPlayFunction,
		bDuplicateIndex ? FirstFunctionIndex : TickFunctionIndex);
}

FString MakeDiagnosticsDebugMapV2Json(
	const FString& ModuleId,
	const FString& SourceFile,
	const FString& WasmSha256)
{
	return FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 2,\n")
		TEXT("  \"debug_version\": \"2.0\",\n")
		TEXT("  \"module_id\": \"%s\",\n")
		TEXT("  \"imported_function_count\": 7,\n")
		TEXT("  \"defined_function_count\": 2,\n")
		TEXT("  \"source\": { \"id\": \"%s\", \"sha256\": \"%s\" },\n")
		TEXT("  \"provenance\": { \"frontend_artifact_sha256\": \"%s\", \"semantic_sha256\": \"%s\", \"guest_ir_sha256\": \"%s\", \"wasm_sha256\": \"%s\" },\n")
		TEXT("  \"functions\": [\n")
		TEXT("    { \"wasm_function_index\": 7, \"guest_function_id\": \"function:symbol:method:Test.Helper()\", \"method_symbol_id\": \"symbol:method:Test.Helper()\", \"display_name\": \"Test.Helper()\", \"span\": { \"start\": 10, \"length\": 20, \"line\": 10, \"column\": 2, \"end_line\": 12, \"end_column\": 3 }, \"sequence_points\": [\n")
		TEXT("      { \"wasm_function_offset\": 10, \"guest_instruction_id\": \"helper:i0\", \"semantic_operation_id\": \"helper:op0\", \"probe_id\": \"1111111111111111\", \"kind\": \"statement\", \"hidden\": false, \"span\": { \"start\": 50, \"length\": 4, \"line\": 20, \"column\": 4, \"end_line\": 20, \"end_column\": 8 } },\n")
		TEXT("      { \"wasm_function_offset\": 30, \"guest_instruction_id\": \"helper:i1\", \"semantic_operation_id\": \"helper:op1\", \"probe_id\": null, \"kind\": \"hidden\", \"hidden\": true, \"span\": { \"start\": 60, \"length\": 2, \"line\": 25, \"column\": 1, \"end_line\": 25, \"end_column\": 3 } },\n")
		TEXT("      { \"wasm_function_offset\": 40, \"guest_instruction_id\": \"helper:i2\", \"semantic_operation_id\": \"helper:op2\", \"probe_id\": \"3333333333333333\", \"kind\": \"call\", \"hidden\": false, \"span\": { \"start\": 70, \"length\": 6, \"line\": 30, \"column\": 6, \"end_line\": 30, \"end_column\": 12 } }\n")
		TEXT("    ], \"frame\": { \"byte_size\": 24, \"variables\": [\n")
		TEXT("      { \"symbol_id\": \"symbol:parameter:input\", \"name\": \"input\", \"kind\": \"parameter\", \"type_id\": \"type:int32\", \"value_kind\": \"scalar\", \"storage\": \"i32\", \"offset\": 4, \"byte_size\": 4, \"declaration\": { \"start\": 20, \"length\": 5, \"line\": 10, \"column\": 5, \"end_line\": 10, \"end_column\": 10 }, \"scope\": { \"start\": 10, \"length\": 70, \"line\": 10, \"column\": 2, \"end_line\": 40, \"end_column\": 10 } },\n")
		TEXT("      { \"symbol_id\": \"symbol:local:result\", \"name\": \"result\", \"kind\": \"local\", \"type_id\": \"type:int32\", \"value_kind\": \"scalar\", \"storage\": \"i32\", \"offset\": 8, \"byte_size\": 4, \"declaration\": { \"start\": 60, \"length\": 2, \"line\": 25, \"column\": 1, \"end_line\": 25, \"end_column\": 3 }, \"scope\": { \"start\": 10, \"length\": 70, \"line\": 10, \"column\": 2, \"end_line\": 40, \"end_column\": 10 } }\n")
		TEXT("    ] } },\n")
		TEXT("    { \"wasm_function_index\": 8, \"guest_function_id\": \"function:symbol:method:Test.Tick(float)\", \"method_symbol_id\": \"symbol:method:Test.Tick(float)\", \"display_name\": \"Test.Tick(float)\", \"span\": { \"start\": 80, \"length\": 8, \"line\": 40, \"column\": 2, \"end_line\": 40, \"end_column\": 10 }, \"sequence_points\": [] }\n")
		TEXT("  ]\n")
		TEXT("}\n"),
		*ModuleId,
		*SourceFile,
		*GDiagnosticsSourceSha,
		*GDiagnosticsSourceSha,
		*GDiagnosticsSemanticSha,
		*GDiagnosticsGuestIrSha,
		*WasmSha256);
}

bool WriteDiagnosticsArtifact(const FString& Path, const FString& Json, FString& OutSha256)
{
	if (!FFileHelper::SaveStringToFile(Json, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}
	OutSha256 = ComputeDiagnosticsFileSha256(Path);
	return !OutSha256.IsEmpty();
}

void AppendRuntimeDiagnosticsU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendRuntimeDiagnosticsString(TArray<uint8>& Bytes, const char* Value)
{
	const int32 Length = FCStringAnsi::Strlen(Value);
	AppendRuntimeDiagnosticsU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Value[Index]));
	}
}

void AppendRuntimeDiagnosticsSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendRuntimeDiagnosticsU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

TArray<uint8> BuildRuntimeDiagnosticsFixture(
	bool bTrapBeginPlay,
	bool bTrapTick,
	bool bDirectTrapBeginPlay = false,
	uint32 FunctionImportCount = 1)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	AppendRuntimeDiagnosticsU32Leb(Types, 3);
	const uint8 EmptyFunctionType[] = { 0x60, 0x00, 0x00 };
	Types.Append(EmptyFunctionType, UE_ARRAY_COUNT(EmptyFunctionType));
	const uint8 TickFunctionType[] = { 0x60, 0x01, 0x7d, 0x00 };
	Types.Append(TickFunctionType, UE_ARRAY_COUNT(TickFunctionType));
	const uint8 HostAddFunctionType[] = { 0x60, 0x01, 0x7f, 0x01, 0x7f };
	Types.Append(HostAddFunctionType, UE_ARRAY_COUNT(HostAddFunctionType));
	AppendRuntimeDiagnosticsSection(Module, 1, Types);

	TArray<uint8> Imports;
	AppendRuntimeDiagnosticsU32Leb(Imports, FunctionImportCount);
	for (uint32 ImportIndex = 0; ImportIndex < FunctionImportCount; ++ImportIndex)
	{
		AppendRuntimeDiagnosticsString(Imports, "avidscript");
		AppendRuntimeDiagnosticsString(Imports, "host_add_i32");
		Imports.Add(0x00);
		AppendRuntimeDiagnosticsU32Leb(Imports, 2);
	}
	AppendRuntimeDiagnosticsSection(Module, 2, Imports);

	TArray<uint8> Functions;
	AppendRuntimeDiagnosticsU32Leb(Functions, 3);
	AppendRuntimeDiagnosticsU32Leb(Functions, 0);
	AppendRuntimeDiagnosticsU32Leb(Functions, 0);
	AppendRuntimeDiagnosticsU32Leb(Functions, 1);
	AppendRuntimeDiagnosticsSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendRuntimeDiagnosticsU32Leb(Exports, 2);
	AppendRuntimeDiagnosticsString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	AppendRuntimeDiagnosticsU32Leb(Exports, FunctionImportCount + 1);
	AppendRuntimeDiagnosticsString(Exports, "avid_on_tick");
	Exports.Add(0x00);
	AppendRuntimeDiagnosticsU32Leb(Exports, FunctionImportCount + 2);
	AppendRuntimeDiagnosticsSection(Module, 7, Exports);

	TArray<uint8> Code;
	AppendRuntimeDiagnosticsU32Leb(Code, 3);
	const TArray<uint8> TrapHelper = { 0x00, 0x00, 0x0b };
	AppendRuntimeDiagnosticsU32Leb(Code, static_cast<uint32>(TrapHelper.Num()));
	Code.Append(TrapHelper);
	TArray<uint8> BeginPlay = { 0x00, 0x0b };
	if (bTrapBeginPlay)
	{
		if (bDirectTrapBeginPlay)
		{
			BeginPlay = { 0x00, 0x00, 0x0b };
		}
		else
		{
			BeginPlay = { 0x00, 0x10 };
			AppendRuntimeDiagnosticsU32Leb(BeginPlay, FunctionImportCount);
			BeginPlay.Add(0x0b);
		}
	}
	AppendRuntimeDiagnosticsU32Leb(Code, static_cast<uint32>(BeginPlay.Num()));
	Code.Append(BeginPlay);
	TArray<uint8> Tick = { 0x00, 0x0b };
	if (bTrapTick)
	{
		Tick = { 0x00, 0x10 };
		AppendRuntimeDiagnosticsU32Leb(Tick, FunctionImportCount);
		Tick.Add(0x0b);
	}
	AppendRuntimeDiagnosticsU32Leb(Code, static_cast<uint32>(Tick.Num()));
	Code.Append(Tick);
	AppendRuntimeDiagnosticsSection(Module, 10, Code);
	return Module;
}

FString MakeRuntimeDiagnosticsManifestJson(
	const FString& RuntimeModuleId,
	const FString& GuestModuleId,
	const FString& WasmFile,
	const FString& WasmSha256,
	const FString& DebugMapFile,
	const FString& DebugMapSha256,
	int32 DebugImportedFunctionCount,
	bool bIncludeDebugMap)
{
	const FString SourceAndGuest = bIncludeDebugMap
		? FString::Printf(
			TEXT("  \"source\": { \"file\": \"Scripts/AvidScript/Test.cs\", \"sha256\": \"%s\", \"frontend_sha256\": \"%s\", \"semantic_sha256\": \"%s\" },\n")
			TEXT("  \"guest_ir\": { \"module_id\": \"%s\", \"sha256\": \"%s\" },\n")
			TEXT("  \"debug_map\": { \"file\": \"%s\", \"sha256\": \"%s\", \"schema_version\": 1, \"version\": \"1.0\", \"module_id\": \"%s\", \"imported_function_count\": %d, \"defined_function_count\": 3 },\n"),
			*GDiagnosticsSourceSha,
			*GDiagnosticsArtifactWrongSha,
			*GDiagnosticsSemanticSha,
			*GuestModuleId,
			*GDiagnosticsGuestIrSha,
			*DebugMapFile,
			*DebugMapSha256,
			*GuestModuleId,
			DebugImportedFunctionCount)
		: FString();
	return FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"module_id\": \"%s\",\n")
		TEXT("  \"abi_version\": 1,\n")
		TEXT("  \"language\": \"csharp\",\n")
		TEXT("%s")
		TEXT("  \"wasm\": { \"file\": \"%s\", \"sha256\": \"%s\" },\n")
		TEXT("  \"required_exports\": [\"avid_on_begin_play\", \"avid_on_tick\"],\n")
		TEXT("  \"required_imports\": [{ \"module\": \"avidscript\", \"name\": \"host_add_i32\" }]\n")
		TEXT("}\n"),
		*RuntimeModuleId,
		*SourceAndGuest,
		*WasmFile,
		*WasmSha256);
}

bool WriteRuntimeDiagnosticsFixture(
	const FString& Root,
	const FString& Stem,
	bool bTrapBeginPlay,
	bool bTrapTick,
	bool bIncludeDebugMap,
	bool bDirectTrapBeginPlay,
	int32 FunctionImportCount,
	FString& OutManifestPath)
{
	const FString RuntimeModuleId = Stem + TEXT("_runtime");
	const FString GuestModuleId = TEXT("csharp:Scripts/AvidScript/Test.cs");
	const FString WasmFileName = Stem + TEXT(".wasm");
	const FString DebugMapFileName = Stem + TEXT(".csharp.debug.json");
	const FString WasmPath = FPaths::Combine(Root, WasmFileName);
	const TArray<uint8> WasmBytes = BuildRuntimeDiagnosticsFixture(
		bTrapBeginPlay,
		bTrapTick,
		bDirectTrapBeginPlay,
		static_cast<uint32>(FunctionImportCount));
	if (!FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath))
	{
		return false;
	}
	const FString WasmSha256 = ComputeDiagnosticsFileSha256(WasmPath);

	FString DebugMapSha256;
	if (bIncludeDebugMap
		&& !WriteDiagnosticsArtifact(
			FPaths::Combine(Root, DebugMapFileName),
			MakeDiagnosticsDebugMapJson(
				GuestModuleId,
				TEXT("Scripts/AvidScript/Test.cs"),
				GDiagnosticsSourceSha,
				GDiagnosticsArtifactWrongSha,
				GDiagnosticsSemanticSha,
				GDiagnosticsGuestIrSha,
				false,
				FunctionImportCount,
				FunctionImportCount + 1,
				FunctionImportCount + 2,
				FunctionImportCount,
				3),
			DebugMapSha256))
	{
		return false;
	}

	OutManifestPath = FPaths::Combine(Root, Stem + TEXT(".avidscript.json"));
	return FFileHelper::SaveStringToFile(
		MakeRuntimeDiagnosticsManifestJson(
			RuntimeModuleId,
			GuestModuleId,
			WasmFileName,
			WasmSha256,
			DebugMapFileName,
			DebugMapSha256,
			FunctionImportCount,
			bIncludeDebugMap),
		*OutManifestPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmDebugMapValidationTest,
	"AvidScript.Runtime.Diagnostics.DebugMapValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmDebugMapValidationTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScript/Tests/P45_6/DebugMapValidation"));
	IFileManager::Get().MakeDirectory(*Root, true);
	const FString ValidPath = FPaths::Combine(Root, TEXT("valid.csharp.debug.json"));
	const FString ModuleId = TEXT("csharp:Scripts/AvidScript/Test.cs");
	const FString SourceFile = TEXT("Scripts/AvidScript/Test.cs");
	const FAvidScriptWasmDebugProvenance Expected = {
		ModuleId,
		SourceFile,
		GDiagnosticsSourceSha,
		GDiagnosticsSourceSha,
		GDiagnosticsSemanticSha,
		GDiagnosticsGuestIrSha,
		7,
		2
	};

	FString ValidSha;
	if (!TestTrue(
		TEXT("valid debug map writes"),
		WriteDiagnosticsArtifact(
			ValidPath,
			MakeDiagnosticsDebugMapJson(ModuleId, SourceFile, GDiagnosticsSourceSha, GDiagnosticsSourceSha, GDiagnosticsSemanticSha, GDiagnosticsGuestIrSha),
			ValidSha)))
	{
		return false;
	}

	TSharedPtr<const FAvidScriptWasmDebugMap> DebugMap;
	FString ErrorCategory;
	FString ErrorSource;
	TArray<FAvidScriptWasmFunctionExport> FunctionExports;
	FAvidScriptWasmFunctionExport& BeginPlayExport = FunctionExports.AddDefaulted_GetRef();
	BeginPlayExport.Name = TEXT("avid_on_begin_play");
	BeginPlayExport.FunctionIndex = 7;
	TestTrue(
		TEXT("valid debug map loads"),
		FAvidScriptWasmDebugMap::LoadAndValidate(
			ValidPath,
			ValidSha,
			Expected,
			MakeArrayView(FunctionExports),
			DebugMap,
			ErrorCategory,
			ErrorSource));
	TestTrue(TEXT("valid debug map is immutable and available"), DebugMap.IsValid());

	if (DebugMap.IsValid())
	{
		TArray<FAvidScriptVmStackFrame> VmFrames;
		FAvidScriptVmStackFrame& MappedVmFrame = VmFrames.AddDefaulted_GetRef();
		MappedVmFrame.FunctionIndex = 7;
		MappedVmFrame.FunctionOffset = 0x2a;
		MappedVmFrame.RawFunctionToken = TEXT("$f7");
		FAvidScriptVmStackFrame& UnknownVmFrame = VmFrames.AddDefaulted_GetRef();
		UnknownVmFrame.FunctionIndex = 99;
		UnknownVmFrame.FunctionOffset = 3;
		UnknownVmFrame.RawFunctionToken = TEXT("$f99");
		FAvidScriptVmStackFrame& NamedVmFrame = VmFrames.AddDefaulted_GetRef();
		NamedVmFrame.FunctionOffset = 5;
		NamedVmFrame.RawFunctionToken = TEXT("avid_on_begin_play");

		TArray<FAvidScriptWasmDiagnosticFrame> Frames;
		DebugMap->MapFrames(VmFrames, Frames);
		TestEqual(TEXT("mapping preserves every VM frame"), Frames.Num(), 3);
		if (Frames.Num() == 3)
		{
			TestTrue(TEXT("known function is source mapped"), Frames[0].bSourceMapped);
			TestEqual(
				TEXT("known function publishes C# frame kind"),
				Frames[0].Kind,
				EAvidScriptWasmDiagnosticFrameKind::CSharp);
			TestEqual(TEXT("known function name"), Frames[0].FunctionName, FString(TEXT("Test.Helper()")));
			TestEqual(TEXT("source identity remains project relative"), Frames[0].SourceFile, SourceFile);
			TestEqual(TEXT("source hash remains navigable provenance"), Frames[0].SourceSha256, GDiagnosticsSourceSha);
			TestEqual(TEXT("public source line is one based"), Frames[0].Line, 11);
			TestEqual(TEXT("public source column is one based"), Frames[0].Column, 3);
			TestEqual(TEXT("public end line is one based"), Frames[0].EndLine, 13);
			TestEqual(TEXT("public end column is one based"), Frames[0].EndColumn, 4);
			TestFalse(TEXT("unknown function remains raw"), Frames[1].bSourceMapped);
			TestEqual(
				TEXT("unknown function publishes WASM frame kind"),
				Frames[1].Kind,
				EAvidScriptWasmDiagnosticFrameKind::Wasm);
			TestEqual(TEXT("unknown function index survives"), Frames[1].FunctionIndex, 99u);
			TestEqual(TEXT("unknown raw token survives"), Frames[1].RawFunctionToken, FString(TEXT("$f99")));
			TestTrue(TEXT("named export frame is source mapped"), Frames[2].bSourceMapped);
			TestEqual(TEXT("named export resolves actual function index"), Frames[2].FunctionIndex, 7u);
			TestEqual(TEXT("named export retains raw token"), Frames[2].RawFunctionToken, FString(TEXT("avid_on_begin_play")));
		}
	}

	const FString V2Path = FPaths::Combine(Root, TEXT("valid-v2.csharp.debug.json"));
	FString V2Sha;
	FAvidScriptWasmDebugProvenance V2Expected = Expected;
	V2Expected.WasmSha256 = GDiagnosticsWasmSha;
	TestTrue(
		TEXT("v2 debug map writes"),
		WriteDiagnosticsArtifact(
			V2Path,
			MakeDiagnosticsDebugMapV2Json(ModuleId, SourceFile, GDiagnosticsWasmSha),
			V2Sha));
	TSharedPtr<const FAvidScriptWasmDebugMap> V2Map;
	ErrorCategory.Reset();
	ErrorSource.Reset();
	TestTrue(
		TEXT("v2 debug map loads"),
		FAvidScriptWasmDebugMap::LoadAndValidate(
			V2Path,
			V2Sha,
			V2Expected,
			MakeArrayView(FunctionExports),
			V2Map,
			ErrorCategory,
			ErrorSource));
	if (V2Map.IsValid())
	{
		TArray<FAvidScriptDebugBreakpoint> Breakpoints;
		V2Map->BuildBreakpointCatalog(Breakpoints);
		TestEqual(TEXT("breakpoint catalog omits hidden and missing probes"), Breakpoints.Num(), 2);
		if (Breakpoints.Num() == 2)
		{
			TestEqual(TEXT("catalog is ordered by function offset"), Breakpoints[0].ProbeId, 0x1111111111111111ULL);
			TestEqual(TEXT("later probe keeps deterministic order"), Breakpoints[1].ProbeId, 0x3333333333333333ULL);
			TestEqual(TEXT("catalog source remains project relative"), Breakpoints[0].SourceFile, SourceFile);
			TestEqual(TEXT("catalog source hash remains available"), Breakpoints[0].SourceSha256, GDiagnosticsSourceSha);
			TestEqual(TEXT("catalog function name survives"), Breakpoints[0].FunctionName, FString(TEXT("Test.Helper()")));
			TestEqual(TEXT("catalog kind survives"), Breakpoints[1].Kind, FString(TEXT("call")));
			TestEqual(TEXT("catalog line is one based"), Breakpoints[0].Line, 21);
			TestEqual(TEXT("catalog column is one based"), Breakpoints[0].Column, 5);
		}

		TArray<FAvidScriptVmStackFrame> V2VmFrames;
		for (const uint32 Offset : { 5u, 10u, 35u, 40u })
		{
			FAvidScriptVmStackFrame& VmFrame = V2VmFrames.AddDefaulted_GetRef();
			VmFrame.FunctionIndex = 7;
			VmFrame.FunctionOffset = Offset;
			VmFrame.RawFunctionToken = TEXT("$f7");
		}
		TArray<FAvidScriptWasmDiagnosticFrame> V2Frames;
		V2Map->MapFrames(V2VmFrames, V2Frames);
		TestEqual(TEXT("v2 mapping preserves frames"), V2Frames.Num(), 4);
		if (V2Frames.Num() == 4)
		{
			TestFalse(TEXT("offset before first point uses function fallback"), V2Frames[0].bSequencePointMapped);
			TestEqual(TEXT("function fallback kind"), V2Frames[0].SourceKind, FString(TEXT("function")));
			TestTrue(TEXT("exact first point maps"), V2Frames[1].bSequencePointMapped);
			TestEqual(TEXT("first point line is one based"), V2Frames[1].Line, 21);
			TestEqual(TEXT("hidden point resolves previous visible point"), V2Frames[2].Line, 21);
			TestEqual(TEXT("call point line is one based"), V2Frames[3].Line, 31);
			TestEqual(TEXT("call point kind survives"), V2Frames[3].SourceKind, FString(TEXT("call")));
		}

		TArray<uint8> FrameBytes;
		FrameBytes.SetNumZeroed(24);
		const int32 InputValue = 42;
		const int32 LocalValue = -7;
		FMemory::Memcpy(FrameBytes.GetData() + 4, &InputValue, sizeof(InputValue));
		FMemory::Memcpy(FrameBytes.GetData() + 8, &LocalValue, sizeof(LocalValue));
		FAvidScriptDebugVariablesSnapshot EarlySnapshot;
		FString VariableError;
		TestTrue(
			TEXT("first probe builds a bounded variable snapshot"),
			V2Map->BuildVariableSnapshot(
				0x1111111111111111ULL,
				FrameBytes,
				EarlySnapshot,
				VariableError));
		TestEqual(TEXT("parameter is visible before local declaration"), EarlySnapshot.Variables.Num(), 1);
		if (EarlySnapshot.Variables.Num() == 1)
		{
			TestEqual(TEXT("parameter name survives"), EarlySnapshot.Variables[0].Name, FString(TEXT("input")));
			TestEqual(TEXT("parameter value is decoded"), EarlySnapshot.Variables[0].Value, FString(TEXT("42")));
		}

		FAvidScriptDebugVariablesSnapshot LateSnapshot;
		VariableError.Reset();
		TestTrue(
			TEXT("later probe builds a lexical variable snapshot"),
			V2Map->BuildVariableSnapshot(
				0x3333333333333333ULL,
				FrameBytes,
				LateSnapshot,
				VariableError));
		TestEqual(TEXT("local appears after its declaration"), LateSnapshot.Variables.Num(), 2);
		if (LateSnapshot.Variables.Num() == 2)
		{
			TestEqual(TEXT("local name survives"), LateSnapshot.Variables[1].Name, FString(TEXT("result")));
			TestEqual(TEXT("signed local value is decoded"), LateSnapshot.Variables[1].Value, FString(TEXT("-7")));
		}
		TestEqual(TEXT("snapshot source remains project relative"), LateSnapshot.SourceFile, SourceFile);
		TestEqual(TEXT("snapshot line is one based"), LateSnapshot.Line, 31);
	}

	const FString InvalidVariablesPath = FPaths::Combine(
		Root,
		TEXT("invalid-variables.csharp.debug.json"));
	FString InvalidVariablesSha;
	const FString InvalidVariablesJson = MakeDiagnosticsDebugMapV2Json(
		ModuleId,
		SourceFile,
		GDiagnosticsWasmSha).Replace(
			TEXT("\"offset\": 8, \"byte_size\": 4"),
			TEXT("\"offset\": 4, \"byte_size\": 4"));
	TestTrue(
		TEXT("invalid variable map writes"),
		WriteDiagnosticsArtifact(
			InvalidVariablesPath,
			InvalidVariablesJson,
			InvalidVariablesSha));
	TSharedPtr<const FAvidScriptWasmDebugMap> InvalidVariablesMap;
	ErrorCategory.Reset();
	ErrorSource.Reset();
	TestFalse(
		TEXT("overlapping variable slots are rejected"),
		FAvidScriptWasmDebugMap::LoadAndValidate(
			InvalidVariablesPath,
			InvalidVariablesSha,
			V2Expected,
			MakeArrayView(FunctionExports),
			InvalidVariablesMap,
			ErrorCategory,
			ErrorSource));
	TestEqual(
		TEXT("invalid variable layout category"),
		ErrorCategory,
		FString(TEXT("debug_map_variable_layout_invalid")));

	FAvidScriptWasmDebugProvenance WrongWasmExpected = V2Expected;
	WrongWasmExpected.WasmSha256 = GDiagnosticsArtifactWrongSha;
	TSharedPtr<const FAvidScriptWasmDebugMap> WrongWasmMap;
	ErrorCategory.Reset();
	ErrorSource.Reset();
	TestFalse(
		TEXT("v2 WASM provenance mismatch is rejected"),
		FAvidScriptWasmDebugMap::LoadAndValidate(
			V2Path,
			V2Sha,
			WrongWasmExpected,
			MakeArrayView(FunctionExports),
			WrongWasmMap,
			ErrorCategory,
			ErrorSource));
	TestEqual(
		TEXT("v2 WASM mismatch category"),
		ErrorCategory,
		FString(TEXT("debug_map_wasm_mismatch")));

	const FString SparsePath = FPaths::Combine(Root, TEXT("sparse.csharp.debug.json"));
	FString SparseSha;
	const FAvidScriptWasmDebugProvenance SparseExpected = {
		ModuleId,
		SourceFile,
		GDiagnosticsSourceSha,
		GDiagnosticsSourceSha,
		GDiagnosticsSemanticSha,
		GDiagnosticsGuestIrSha,
		7,
		3
	};
	TestTrue(
		TEXT("sparse method map writes"),
		WriteDiagnosticsArtifact(
			SparsePath,
			MakeDiagnosticsDebugMapJson(
				ModuleId,
				SourceFile,
				GDiagnosticsSourceSha,
				GDiagnosticsSourceSha,
				GDiagnosticsSemanticSha,
				GDiagnosticsGuestIrSha,
				false,
				7,
				9,
				INDEX_NONE,
				7,
				3),
			SparseSha));
	TSharedPtr<const FAvidScriptWasmDebugMap> SparseMap;
	ErrorCategory.Reset();
	ErrorSource.Reset();
	TestTrue(
		TEXT("constructors may leave valid holes in the mapped method index set"),
		FAvidScriptWasmDebugMap::LoadAndValidate(
			SparsePath,
			SparseSha,
			SparseExpected,
			TConstArrayView<FAvidScriptWasmFunctionExport>(),
			SparseMap,
			ErrorCategory,
			ErrorSource));

	auto ExpectRejected = [this, &Root, &Expected](
		const TCHAR* Label,
		const TCHAR* FileName,
		const FString& Json,
		const FString& ExpectedCategory,
		const FString* ExpectedHash = nullptr)
	{
		const FString Path = FPaths::Combine(Root, FileName);
		FString ActualHash;
		if (!WriteDiagnosticsArtifact(Path, Json, ActualHash))
		{
			AddError(FString::Printf(TEXT("%s fixture could not be written"), Label));
			return;
		}
		TSharedPtr<const FAvidScriptWasmDebugMap> RejectedMap;
		FString Category;
		FString Source;
		TestFalse(
			Label,
			FAvidScriptWasmDebugMap::LoadAndValidate(
				Path,
				ExpectedHash != nullptr ? *ExpectedHash : ActualHash,
				Expected,
				TConstArrayView<FAvidScriptWasmFunctionExport>(),
				RejectedMap,
				Category,
				Source));
		TestEqual(FString::Printf(TEXT("%s category"), Label), Category, ExpectedCategory);
		TestFalse(FString::Printf(TEXT("%s publishes no map"), Label), RejectedMap.IsValid());
	};

	ExpectRejected(
		TEXT("artifact hash mismatch is rejected"),
		TEXT("hash-mismatch.json"),
		MakeDiagnosticsDebugMapJson(ModuleId, SourceFile, GDiagnosticsSourceSha, GDiagnosticsSourceSha, GDiagnosticsSemanticSha, GDiagnosticsGuestIrSha),
		TEXT("debug_map_hash_mismatch"),
		&GDiagnosticsArtifactWrongSha);
	ExpectRejected(
		TEXT("Guest module mismatch is rejected"),
		TEXT("module-mismatch.json"),
		MakeDiagnosticsDebugMapJson(TEXT("csharp:Scripts/Other.cs"), SourceFile, GDiagnosticsSourceSha, GDiagnosticsSourceSha, GDiagnosticsSemanticSha, GDiagnosticsGuestIrSha),
		TEXT("debug_map_module_mismatch"));
	ExpectRejected(
		TEXT("source hash mismatch is rejected"),
		TEXT("source-mismatch.json"),
		MakeDiagnosticsDebugMapJson(ModuleId, SourceFile, GDiagnosticsArtifactWrongSha, GDiagnosticsSourceSha, GDiagnosticsSemanticSha, GDiagnosticsGuestIrSha),
		TEXT("debug_map_source_mismatch"));
	ExpectRejected(
		TEXT("frontend artifact hash mismatch is rejected"),
		TEXT("frontend-artifact-mismatch.json"),
		MakeDiagnosticsDebugMapJson(ModuleId, SourceFile, GDiagnosticsSourceSha, GDiagnosticsArtifactWrongSha, GDiagnosticsSemanticSha, GDiagnosticsGuestIrSha),
		TEXT("debug_map_provenance_mismatch"));
	ExpectRejected(
		TEXT("Guest IR hash mismatch is rejected"),
		TEXT("guest-ir-mismatch.json"),
		MakeDiagnosticsDebugMapJson(ModuleId, SourceFile, GDiagnosticsSourceSha, GDiagnosticsSourceSha, GDiagnosticsSemanticSha, GDiagnosticsArtifactWrongSha),
		TEXT("debug_map_guest_ir_mismatch"));
	ExpectRejected(
		TEXT("absolute source identity is rejected"),
		TEXT("absolute-source.json"),
		MakeDiagnosticsDebugMapJson(ModuleId, TEXT("C:/Private/Test.cs"), GDiagnosticsSourceSha, GDiagnosticsSourceSha, GDiagnosticsSemanticSha, GDiagnosticsGuestIrSha),
		TEXT("debug_map_source_invalid"));
	ExpectRejected(
		TEXT("duplicate function index is rejected"),
		TEXT("duplicate-index.json"),
		MakeDiagnosticsDebugMapJson(ModuleId, SourceFile, GDiagnosticsSourceSha, GDiagnosticsSourceSha, GDiagnosticsSemanticSha, GDiagnosticsGuestIrSha, true),
		TEXT("debug_map_duplicate_function_index"));
	ExpectRejected(
		TEXT("defined function range mismatch is rejected"),
		TEXT("defined-count-mismatch.json"),
		MakeDiagnosticsDebugMapJson(
			ModuleId,
			SourceFile,
			GDiagnosticsSourceSha,
			GDiagnosticsSourceSha,
			GDiagnosticsSemanticSha,
			GDiagnosticsGuestIrSha,
			false,
			7,
			8,
			INDEX_NONE,
			7,
			3),
		TEXT("debug_map_function_index_range_mismatch"));
	ExpectRejected(
		TEXT("out-of-range function index is rejected"),
		TEXT("function-index-range-mismatch.json"),
		MakeDiagnosticsDebugMapJson(ModuleId, SourceFile, GDiagnosticsSourceSha, GDiagnosticsSourceSha, GDiagnosticsSemanticSha, GDiagnosticsGuestIrSha, false, 7, 9),
		TEXT("debug_map_function_index_range_mismatch"));

	TSharedPtr<const FAvidScriptWasmDebugMap> MissingMap;
	ErrorCategory.Reset();
	ErrorSource.Reset();
	TestFalse(
		TEXT("missing map file is rejected"),
		FAvidScriptWasmDebugMap::LoadAndValidate(
			FPaths::Combine(Root, TEXT("missing.json")),
			ValidSha,
			Expected,
			TConstArrayView<FAvidScriptWasmFunctionExport>(),
			MissingMap,
			ErrorCategory,
			ErrorSource));
	TestEqual(TEXT("missing map category"), ErrorCategory, FString(TEXT("debug_map_file_missing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmDiagnosticRuntimeIntegrationTest,
	"AvidScript.Runtime.Diagnostics.ManifestRuntimeIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmDiagnosticRuntimeIntegrationTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScript/Tests/P45_6/RuntimeDiagnostics"));
	IFileManager::Get().MakeDirectory(*Root, true);

	FString TickTrapManifestPath;
	if (!TestTrue(
		TEXT("mapped Tick trap fixture writes"),
		WriteRuntimeDiagnosticsFixture(Root, TEXT("tick_trap"), false, true, true, false, 1, TickTrapManifestPath)))
	{
		return false;
	}
	FAvidScriptWasmReloadManifest TickTrapManifest;
	TArray<uint8> TickTrapBytecode;
	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	if (!TestTrue(
		TEXT("manifest loader validates and attaches debug map"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			TickTrapManifestPath,
			TickTrapManifest,
			TickTrapBytecode,
			LoadResult)))
	{
		AddError(LoadResult.ErrorMessage);
		return false;
	}
	TestTrue(TEXT("validated manifest retains immutable debug map"), TickTrapManifest.DebugMap.IsValid());

	FString ImportMismatchManifestPath;
	TestTrue(
		TEXT("two-import mismatch fixture writes"),
		WriteRuntimeDiagnosticsFixture(
			Root,
			TEXT("import_mismatch"),
			false,
			false,
			true,
			false,
			2,
			ImportMismatchManifestPath));
	FAvidScriptWasmReloadManifest ImportMismatchManifest;
	TArray<uint8> ImportMismatchBytecode;
	FAvidScriptWasmReloadManifestLoadResult ImportMismatchResult;
	TestFalse(
		TEXT("manifest import count cannot disagree with actual WASM function imports"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			ImportMismatchManifestPath,
			ImportMismatchManifest,
			ImportMismatchBytecode,
			ImportMismatchResult));
	TestEqual(
		TEXT("WASM import identity mismatch category"),
		ImportMismatchResult.ErrorCategory,
		FString(TEXT("manifest_wasm_import_mismatch")));

	FAvidScriptRuntimeSession TickTrapSession;
	FAvidScriptWasmReloadResult InitialResult;
	TestTrue(
		TEXT("mapped Tick trap runtime begins successfully"),
		TickTrapSession.LoadInitialModule(
			TickTrapBytecode.GetData(),
			TickTrapBytecode.Num(),
			TickTrapManifest,
			InitialResult));
	TestTrue(TEXT("healthy BeginPlay exposes no diagnostic frames"), InitialResult.RuntimeResult.DiagnosticFrames.IsEmpty());
	TArray<FAvidScriptDebugBreakpoint> TickTrapBreakpoints;
	FString BreakpointCatalogError;
	TestTrue(
		TEXT("live Session exposes its validated breakpoint catalog"),
		TickTrapSession.GetDebugBreakpointCatalog(TickTrapBreakpoints, BreakpointCatalogError));
	TestTrue(TEXT("v1 Session catalog remains empty"), TickTrapBreakpoints.IsEmpty());

	FAvidScriptWasmSmokeResult TickResult;
	TestFalse(TEXT("real WAMR Tick trap reaches Runtime"), TickTrapSession.TickLive(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Tick trap keeps root category"), TickResult.ErrorCategory, FString(TEXT("trap")));
	TestTrue(TEXT("Tick trap exposes diagnostic frames"), !TickResult.DiagnosticFrames.IsEmpty());
	if (!TickResult.DiagnosticFrames.IsEmpty())
	{
		const FAvidScriptWasmDiagnosticFrame& FirstFrame = TickResult.DiagnosticFrames[0];
		TestTrue(TEXT("Tick helper frame is source mapped"), FirstFrame.bSourceMapped);
		TestEqual(TEXT("Tick helper method name"), FirstFrame.FunctionName, FString(TEXT("Test.Helper()")));
		TestEqual(TEXT("Tick helper source line"), FirstFrame.Line, 11);
		TestEqual(TEXT("Tick helper raw index"), FirstFrame.FunctionIndex, 1u);
		TestEqual(
			TEXT("Tick helper is a C# call-stack frame"),
			FirstFrame.Kind,
			EAvidScriptWasmDiagnosticFrameKind::CSharp);
		const FAvidScriptWasmDiagnosticFrame& EntryFrame = TickResult.DiagnosticFrames.Last();
		TestEqual(
			TEXT("Tick failure retains UE entry boundary"),
			EntryFrame.Kind,
			EAvidScriptWasmDiagnosticFrameKind::HostEntry);
		TestEqual(TEXT("Tick failure entry export"), EntryFrame.FunctionName, FString(TEXT("avid_on_tick")));
	}

	FString LegacyManifestPath;
	TestTrue(
		TEXT("legacy manifest fixture writes"),
		WriteRuntimeDiagnosticsFixture(Root, TEXT("legacy_tick_trap"), false, true, false, false, 1, LegacyManifestPath));
	FAvidScriptWasmReloadManifest LegacyManifest;
	TArray<uint8> LegacyBytecode;
	FAvidScriptWasmReloadManifestLoadResult LegacyLoadResult;
	TestTrue(
		TEXT("legacy manifest without debug map remains loadable"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			LegacyManifestPath,
			LegacyManifest,
			LegacyBytecode,
			LegacyLoadResult));
	TestFalse(TEXT("legacy manifest has no debug map"), LegacyManifest.DebugMap.IsValid());
	FAvidScriptRuntimeSession LegacySession;
	FAvidScriptWasmReloadResult LegacyInitialResult;
	TestTrue(
		TEXT("legacy runtime begins"),
		LegacySession.LoadInitialModule(
			LegacyBytecode.GetData(),
			LegacyBytecode.Num(),
			LegacyManifest,
			LegacyInitialResult));
	TArray<FAvidScriptDebugBreakpoint> LegacyBreakpoints;
	BreakpointCatalogError.Reset();
	TestFalse(
		TEXT("Session without a validated debug map rejects breakpoint discovery"),
		LegacySession.GetDebugBreakpointCatalog(LegacyBreakpoints, BreakpointCatalogError));
	TestTrue(TEXT("rejected catalog cannot leak stale entries"), LegacyBreakpoints.IsEmpty());
	TestTrue(TEXT("rejected catalog explains the missing map"), !BreakpointCatalogError.IsEmpty());
	FAvidScriptWasmSmokeResult LegacyTickResult;
	TestFalse(TEXT("legacy runtime trap is preserved"), LegacySession.TickLive(1.0f / 60.0f, LegacyTickResult));
	TestTrue(TEXT("legacy trap keeps raw frames"), !LegacyTickResult.DiagnosticFrames.IsEmpty());
	TestFalse(
		TEXT("legacy trap does not invent source mapping"),
		LegacyTickResult.DiagnosticFrames.ContainsByPredicate([](const FAvidScriptWasmDiagnosticFrame& Frame)
		{
			return Frame.bSourceMapped;
		}));

	const TArray<uint8> HealthyBytecode = BuildRuntimeDiagnosticsFixture(false, false, false, 0);
	FAvidScriptRuntimeSession ReloadSession;
	FAvidScriptWasmReloadManifest HealthyManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("healthy_live"));
	FAvidScriptWasmReloadResult HealthyLoadResult;
	TestTrue(
		TEXT("healthy live runtime begins"),
		ReloadSession.LoadInitialModule(
			HealthyBytecode.GetData(),
			HealthyBytecode.Num(),
			HealthyManifest,
			HealthyLoadResult));

	FString BeginTrapManifestPath;
	TestTrue(
		TEXT("mapped BeginPlay trap fixture writes"),
		WriteRuntimeDiagnosticsFixture(Root, TEXT("begin_trap"), true, false, true, true, 1, BeginTrapManifestPath));
	FAvidScriptWasmReloadManifest BeginTrapManifest;
	TArray<uint8> BeginTrapBytecode;
	FAvidScriptWasmReloadManifestLoadResult BeginTrapLoadResult;
	TestTrue(
		TEXT("mapped BeginPlay trap manifest loads"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			BeginTrapManifestPath,
			BeginTrapManifest,
			BeginTrapBytecode,
			BeginTrapLoadResult));
	FAvidScriptWasmReloadResult RejectedReload;
	TestFalse(
		TEXT("candidate BeginPlay trap rejects reload"),
		ReloadSession.ReloadModule(
			BeginTrapBytecode.GetData(),
			BeginTrapBytecode.Num(),
			BeginTrapManifest,
			RejectedReload));
	TestTrue(TEXT("candidate trap preserves previous runtime"), RejectedReload.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("previous runtime remains active"), ReloadSession.GetLiveModuleId(), FString(TEXT("healthy_live")));
	TestTrue(
		TEXT("direct exported BeginPlay trap is source mapped"),
		RejectedReload.RuntimeResult.DiagnosticFrames.ContainsByPredicate([](const FAvidScriptWasmDiagnosticFrame& Frame)
		{
			return Frame.bSourceMapped
				&& Frame.FunctionName == TEXT("Test.BeginPlay()")
				&& Frame.RawFunctionToken == TEXT("avid_on_begin_play")
				&& Frame.FunctionIndex == 2;
		}));
	FAvidScriptWasmSmokeResult PreservedTickResult;
	TestTrue(TEXT("preserved runtime continues ticking"), ReloadSession.TickLive(1.0f / 60.0f, PreservedTickResult));
	TestTrue(TEXT("preserved healthy Tick has no diagnostic frames"), PreservedTickResult.DiagnosticFrames.IsEmpty());
	return true;
}

#endif
