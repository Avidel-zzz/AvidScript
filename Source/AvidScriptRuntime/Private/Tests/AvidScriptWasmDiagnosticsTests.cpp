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
	int32 ThirdFunctionIndex = INDEX_NONE)
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
		TEXT("  \"source\": { \"id\": \"%s\", \"sha256\": \"%s\" },\n")
		TEXT("  \"provenance\": { \"frontend_sha256\": \"%s\", \"semantic_sha256\": \"%s\", \"guest_ir_sha256\": \"%s\" },\n")
		TEXT("  \"functions\": [\n")
		TEXT("    { \"wasm_function_index\": %d, \"guest_function_id\": \"function:symbol:method:Test.Helper()\", \"method_symbol_id\": \"symbol:method:Test.Helper()\", \"display_name\": \"Test.Helper()\", \"span\": { \"start\": 10, \"length\": 20, \"line\": 10, \"column\": 2, \"end_line\": 12, \"end_column\": 3 } },\n")
		TEXT("%s")
		TEXT("    { \"wasm_function_index\": %d, \"guest_function_id\": \"function:symbol:method:Test.Tick(float)\", \"method_symbol_id\": \"symbol:method:Test.Tick(float)\", \"display_name\": \"Test.Tick(float)\", \"span\": { \"start\": 40, \"length\": 8, \"line\": 14, \"column\": 2, \"end_line\": 16, \"end_column\": 3 } }\n")
		TEXT("  ]\n")
		TEXT("}\n"),
		*ModuleId,
		*SourceFile,
		*MapSourceSha,
		*MapFrontendSha,
		*MapSemanticSha,
		*MapGuestIrSha,
		FirstFunctionIndex,
		*OptionalBeginPlayFunction,
		bDuplicateIndex ? FirstFunctionIndex : TickFunctionIndex);
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

TArray<uint8> BuildRuntimeDiagnosticsFixture(bool bTrapBeginPlay, bool bTrapTick)
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
	AppendRuntimeDiagnosticsU32Leb(Imports, 1);
	AppendRuntimeDiagnosticsString(Imports, "avidscript");
	AppendRuntimeDiagnosticsString(Imports, "host_add_i32");
	Imports.Add(0x00);
	AppendRuntimeDiagnosticsU32Leb(Imports, 2);
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
	AppendRuntimeDiagnosticsU32Leb(Exports, 2);
	AppendRuntimeDiagnosticsString(Exports, "avid_on_tick");
	Exports.Add(0x00);
	AppendRuntimeDiagnosticsU32Leb(Exports, 3);
	AppendRuntimeDiagnosticsSection(Module, 7, Exports);

	TArray<uint8> Code;
	AppendRuntimeDiagnosticsU32Leb(Code, 3);
	const TArray<uint8> TrapHelper = { 0x00, 0x00, 0x0b };
	AppendRuntimeDiagnosticsU32Leb(Code, static_cast<uint32>(TrapHelper.Num()));
	Code.Append(TrapHelper);
	TArray<uint8> BeginPlay = { 0x00, 0x0b };
	if (bTrapBeginPlay)
	{
		BeginPlay = { 0x00, 0x10, 0x01, 0x0b };
	}
	AppendRuntimeDiagnosticsU32Leb(Code, static_cast<uint32>(BeginPlay.Num()));
	Code.Append(BeginPlay);
	TArray<uint8> Tick = { 0x00, 0x0b };
	if (bTrapTick)
	{
		Tick = { 0x00, 0x10, 0x01, 0x0b };
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
	bool bIncludeDebugMap)
{
	const FString SourceAndGuest = bIncludeDebugMap
		? FString::Printf(
			TEXT("  \"source\": { \"file\": \"Scripts/AvidScript/Test.cs\", \"sha256\": \"%s\", \"frontend_sha256\": \"%s\", \"semantic_sha256\": \"%s\" },\n")
			TEXT("  \"guest_ir\": { \"module_id\": \"%s\", \"sha256\": \"%s\" },\n")
			TEXT("  \"debug_map\": { \"file\": \"%s\", \"sha256\": \"%s\", \"schema_version\": 1, \"version\": \"1.0\", \"module_id\": \"%s\" },\n"),
			*GDiagnosticsSourceSha,
			*GDiagnosticsArtifactWrongSha,
			*GDiagnosticsSemanticSha,
			*GuestModuleId,
			*GDiagnosticsGuestIrSha,
			*DebugMapFile,
			*DebugMapSha256,
			*GuestModuleId)
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
	FString& OutManifestPath)
{
	const FString RuntimeModuleId = Stem + TEXT("_runtime");
	const FString GuestModuleId = TEXT("csharp:Scripts/AvidScript/Test.cs");
	const FString WasmFileName = Stem + TEXT(".wasm");
	const FString DebugMapFileName = Stem + TEXT(".csharp.debug.json");
	const FString WasmPath = FPaths::Combine(Root, WasmFileName);
	const TArray<uint8> WasmBytes = BuildRuntimeDiagnosticsFixture(bTrapBeginPlay, bTrapTick);
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
				GDiagnosticsSourceSha,
				GDiagnosticsSemanticSha,
				GDiagnosticsGuestIrSha,
				false,
				1,
				2,
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
		7
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
	TestTrue(
		TEXT("valid debug map loads"),
		FAvidScriptWasmDebugMap::LoadAndValidate(
			ValidPath,
			ValidSha,
			Expected,
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

		TArray<FAvidScriptWasmDiagnosticFrame> Frames;
		DebugMap->MapFrames(VmFrames, Frames);
		TestEqual(TEXT("mapping preserves every VM frame"), Frames.Num(), 2);
		if (Frames.Num() == 2)
		{
			TestTrue(TEXT("known function is source mapped"), Frames[0].bSourceMapped);
			TestEqual(TEXT("known function name"), Frames[0].FunctionName, FString(TEXT("Test.Helper()")));
			TestEqual(TEXT("source identity remains project relative"), Frames[0].SourceFile, SourceFile);
			TestEqual(TEXT("public source line is one based"), Frames[0].Line, 11);
			TestEqual(TEXT("public source column is one based"), Frames[0].Column, 3);
			TestEqual(TEXT("public end line is one based"), Frames[0].EndLine, 13);
			TestEqual(TEXT("public end column is one based"), Frames[0].EndColumn, 4);
			TestFalse(TEXT("unknown function remains raw"), Frames[1].bSourceMapped);
			TestEqual(TEXT("unknown function index survives"), Frames[1].FunctionIndex, 99u);
			TestEqual(TEXT("unknown raw token survives"), Frames[1].RawFunctionToken, FString(TEXT("$f99")));
		}
	}

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
		TEXT("non-contiguous function index is rejected"),
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
		WriteRuntimeDiagnosticsFixture(Root, TEXT("tick_trap"), false, true, true, TickTrapManifestPath)))
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
	}

	FString LegacyManifestPath;
	TestTrue(
		TEXT("legacy manifest fixture writes"),
		WriteRuntimeDiagnosticsFixture(Root, TEXT("legacy_tick_trap"), false, true, false, LegacyManifestPath));
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
	FAvidScriptWasmSmokeResult LegacyTickResult;
	TestFalse(TEXT("legacy runtime trap is preserved"), LegacySession.TickLive(1.0f / 60.0f, LegacyTickResult));
	TestTrue(TEXT("legacy trap keeps raw frames"), !LegacyTickResult.DiagnosticFrames.IsEmpty());
	TestFalse(
		TEXT("legacy trap does not invent source mapping"),
		LegacyTickResult.DiagnosticFrames.ContainsByPredicate([](const FAvidScriptWasmDiagnosticFrame& Frame)
		{
			return Frame.bSourceMapped;
		}));

	const TArray<uint8> HealthyBytecode = BuildRuntimeDiagnosticsFixture(false, false);
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
		WriteRuntimeDiagnosticsFixture(Root, TEXT("begin_trap"), true, false, true, BeginTrapManifestPath));
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
		TEXT("candidate BeginPlay trap is source mapped"),
		RejectedReload.RuntimeResult.DiagnosticFrames.ContainsByPredicate([](const FAvidScriptWasmDiagnosticFrame& Frame)
		{
			return Frame.bSourceMapped && Frame.FunctionName == TEXT("Test.Helper()");
		}));
	FAvidScriptWasmSmokeResult PreservedTickResult;
	TestTrue(TEXT("preserved runtime continues ticking"), ReloadSession.TickLive(1.0f / 60.0f, PreservedTickResult));
	TestTrue(TEXT("preserved healthy Tick has no diagnostic frames"), PreservedTickResult.DiagnosticFrames.IsEmpty());
	return true;
}

#endif
