#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptLanguageErrorCatalog.h"
#include "AvidScriptRuntimeBackendTestLanes.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptTaskResultAbi.h"
#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Memory/AvidScriptManagedHeap.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"

#include <array>

namespace AvidScriptLanguageErrorCatalogTests
{
constexpr uint8 BaseWasm[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x29, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64,
	0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69,
	0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00,
	0x10, 0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e,
	0x5f, 0x65, 0x6e, 0x64, 0x5f, 0x70, 0x6c, 0x61,
	0x79, 0x00, 0x00,
	0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
};
const FString ModuleId = TEXT("language_catalog_test");
const FString SourceSha256 = FString::ChrN(64, 'a');

void U32(TArray<uint8>& Bytes, uint32 Value)
{
	do
	{
		uint8 Byte = Value & 0x7f;
		Value >>= 7;
		Bytes.Add(Byte | (Value ? 0x80 : 0));
	} while (Value);
}

void Custom(TArray<uint8>& Module, const ANSICHAR* Name, const FString& Text)
{
	TArray<uint8> Payload;
	const int32 NameBytes = FCStringAnsi::Strlen(Name);
	U32(Payload, NameBytes);
	Payload.Append(reinterpret_cast<const uint8*>(Name), NameBytes);
	const FTCHARToUTF8 Encoded(*Text);
	Payload.Append(reinterpret_cast<const uint8*>(Encoded.Get()), Encoded.Length());
	Module.Add(0);
	U32(Module, Payload.Num());
	Module.Append(Payload);
}

void WasmName(TArray<uint8>& Bytes, const ANSICHAR* Name)
{
	const int32 Length = FCStringAnsi::Strlen(Name);
	U32(Bytes, Length);
	Bytes.Append(reinterpret_cast<const uint8*>(Name), Length);
}

void WasmSection(TArray<uint8>& Module, uint8 Id, const TArray<uint8>& Payload)
{
	Module.Add(Id);
	U32(Module, Payload.Num());
	Module.Append(Payload);
}

FString Provenance(int32 GuestSchema = 17)
{
	const FString GuestVersion = GuestSchema == 23 ? TEXT("1.22")
		: GuestSchema == 22 ? TEXT("1.21")
		: GuestSchema == 21 ? TEXT("1.20")
		: GuestSchema == 20 ? TEXT("1.19") : TEXT("1.16");
	return FString::Printf(TEXT("module_id=%s\nsource_id=Scripts/SourceThrow.cs\nsource_sha256=%s\n")
		TEXT("frontend_sha256=%s\nsemantic_sha256=%s\nguest_ir=%d/%s"),
		*ModuleId, *SourceSha256, *FString::ChrN(64, 'b'), *FString::ChrN(64, 'c'),
		GuestSchema, *GuestVersion);
}

TSharedRef<FJsonObject> Document(int32 GuestSchema = 17)
{
	auto Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetNumberField(TEXT("guest_ir_schema_version"), GuestSchema);
	Root->SetStringField(TEXT("guest_ir_version"),
		GuestSchema == 23 ? TEXT("1.22")
			: GuestSchema == 22 ? TEXT("1.21")
			: GuestSchema == 21 ? TEXT("1.20")
			: GuestSchema == 20 ? TEXT("1.19") : TEXT("1.16"));
	Root->SetStringField(TEXT("module_id"), ModuleId);
	Root->SetStringField(TEXT("source_sha256"), SourceSha256);
	auto Type = MakeShared<FJsonObject>();
	Type->SetNumberField(TEXT("token"), 1);
	Type->SetStringField(TEXT("type_id"), TEXT("type:global::System.Exception"));
	Root->SetArrayField(TEXT("types"), {MakeShared<FJsonValueObject>(Type)});
	auto Source = MakeShared<FJsonObject>();
	Source->SetNumberField(TEXT("token"), 1);
	Source->SetStringField(TEXT("source_id"), TEXT("Scripts/SourceThrow.cs"));
	Source->SetNumberField(TEXT("source_length"), 100);
	Source->SetNumberField(TEXT("start"), 5);
	Source->SetNumberField(TEXT("length"), 10);
	Source->SetNumberField(TEXT("line"), 1);
	Source->SetNumberField(TEXT("column"), 5);
	Source->SetNumberField(TEXT("end_line"), 1);
	Source->SetNumberField(TEXT("end_column"), 15);
	Root->SetArrayField(TEXT("sources"), {MakeShared<FJsonValueObject>(Source)});
	return Root;
}

FString Json(const TSharedRef<FJsonObject>& Root)
{
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	check(FJsonSerializer::Serialize(Root, Writer));
	return Text;
}

TArray<uint8> Module(const FString* Metadata, bool bProvenance = true,
	bool bDuplicate = false, int32 GuestSchema = 17)
{
	TArray<uint8> Wasm;
	Wasm.Append(BaseWasm, UE_ARRAY_COUNT(BaseWasm));
	if (bProvenance) Custom(Wasm, "avidscript.provenance", Provenance(GuestSchema));
	if (Metadata)
	{
		Custom(Wasm, "avidscript.language_errors", *Metadata);
		if (bDuplicate) Custom(Wasm, "avidscript.language_errors", *Metadata);
	}
	return Wasm;
}

TArray<uint8> TaskFaultImportModule(int32 GuestSchema)
{
	TArray<uint8> Wasm;
	Wasm.Append(BaseWasm, 8); // magic and WASM version only
	TArray<uint8> Types;
	const uint8 Signatures[] = {
		2, 0x60, 4, 0x7e, 0x7f, 0x7f, 0x7e, 1, 0x7f,
		0x60, 0, 0
	};
	Types.Append(Signatures, UE_ARRAY_COUNT(Signatures));
	WasmSection(Wasm, 1, Types);
	TArray<uint8> Imports;
	U32(Imports, 1);
	WasmName(Imports, "avidscript");
	WasmName(Imports, "avid_task_fault_language_error_v1");
	Imports.Add(0); // function import
	U32(Imports, 0); // (i64, i32, i32, i64) -> i32
	WasmSection(Wasm, 2, Imports);
	TArray<uint8> Functions;
	U32(Functions, 1);
	U32(Functions, 1); // () -> void
	WasmSection(Wasm, 3, Functions);
	TArray<uint8> Exports;
	U32(Exports, 2);
	for (const ANSICHAR* Name : {"avid_on_begin_play", "avid_on_end_play"})
	{
		WasmName(Exports, Name);
		Exports.Add(0); // function export
		U32(Exports, 1); // imported function occupies index 0
	}
	WasmSection(Wasm, 7, Exports);
	TArray<uint8> Code;
	U32(Code, 1);
	const uint8 Body[] = {
		0, // no locals
		0x42, 0, 0x41, 1, 0x41, 1, 0x42, 0,
		0x10, 0, // call imported fault function
		0x1a, // drop its result
		0x0b
	};
	U32(Code, UE_ARRAY_COUNT(Body));
	Code.Append(Body, UE_ARRAY_COUNT(Body));
	WasmSection(Wasm, 10, Code);
	Custom(Wasm, "avidscript.provenance", Provenance(GuestSchema));
	const FString Metadata = Json(Document(GuestSchema));
	Custom(Wasm, "avidscript.language_errors", Metadata);
	return Wasm;
}

TArray<uint8> TaskReadImportModule(int32 GuestSchema, const ANSICHAR* ImportName)
{
	TArray<uint8> Wasm;
	Wasm.Append(BaseWasm, 8);
	TArray<uint8> Types;
	const uint8 Signatures[] = {
		2, 0x60, 1, 0x7e, 1, 0x7e, // (i64) -> i64
		0x60, 0, 0 // () -> void
	};
	Types.Append(Signatures, UE_ARRAY_COUNT(Signatures));
	WasmSection(Wasm, 1, Types);
	TArray<uint8> Imports;
	U32(Imports, 1);
	WasmName(Imports, "avidscript");
	WasmName(Imports, ImportName);
	Imports.Add(0);
	U32(Imports, 0);
	WasmSection(Wasm, 2, Imports);
	TArray<uint8> Functions;
	U32(Functions, 1);
	U32(Functions, 1);
	WasmSection(Wasm, 3, Functions);
	TArray<uint8> Exports;
	U32(Exports, 2);
	for (const ANSICHAR* Name : {"avid_on_begin_play", "avid_on_end_play"})
	{
		WasmName(Exports, Name);
		Exports.Add(0);
		U32(Exports, 1);
	}
	WasmSection(Wasm, 7, Exports);
	TArray<uint8> Code;
	U32(Code, 1);
	const uint8 Body[] = {
		0, // no locals
		0x42, 0, // i64.const 0: missing Session task context
		0x10, 0, // call imported read function
		0x1a, // drop i64 result
		0x0b
	};
	U32(Code, UE_ARRAY_COUNT(Body));
	Code.Append(Body, UE_ARRAY_COUNT(Body));
	WasmSection(Wasm, 10, Code);
	Custom(Wasm, "avidscript.provenance", Provenance(GuestSchema));
	const FString Metadata = Json(Document(GuestSchema));
	Custom(Wasm, "avidscript.language_errors", Metadata);
	return Wasm;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptLanguageErrorCatalogRuntimeTest,
	"AvidScript.Runtime.LanguageErrorCatalog.LoadAndReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptLanguageErrorCatalogRuntimeTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptLanguageErrorCatalogTests;
	const FString ValidJson = Json(Document());
	const TArray<uint8> ValidWasm = Module(&ValidJson);
	FString Error;
	TUniquePtr<FAvidScriptLanguageErrorCatalog> Catalog;
	if (!TestTrue(TEXT("read compiler-shaped language-error section"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(ValidWasm, ModuleId, Catalog, Error)))
	{
		AddError(Error);
		return false;
	}
	const FString* Type = Catalog->FindType(1);
	const FAvidScriptLanguageErrorSource* Source = Catalog->FindSource(1);
	TestTrue(TEXT("type token resolves to semantic type"), Type
		&& *Type == TEXT("type:global::System.Exception"));
	TestTrue(TEXT("source token resolves to UTF-16 span"), Source
		&& Source->SourceId == TEXT("Scripts/SourceThrow.cs")
		&& Source->SourceLength == 100 && Source->Start == 5 && Source->Length == 10
		&& Source->Line == 1 && Source->Column == 5 && Source->EndLine == 1 && Source->EndColumn == 15);
	TestNull(TEXT("zero type token is rejected"), Catalog->FindType(0));
	TestNull(TEXT("unknown source token is rejected"), Catalog->FindSource(2));
	const FString AsyncJson = Json(Document(21));
	const TArray<uint8> AsyncWasm = Module(&AsyncJson, true, false, 21);
	Catalog.Reset();
	TestTrue(TEXT("IR 21 async Task language-error catalog loads"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			AsyncWasm, ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 21 authorizes Task language-error fault"),
		Catalog && Catalog->SupportsTaskLanguageErrorFault());
	const FString AsyncExceptionJson = Json(Document(22));
	const TArray<uint8> AsyncExceptionWasm = Module(&AsyncExceptionJson, true, false, 22);
	Catalog.Reset();
	TestTrue(TEXT("IR 22 async exception catalog loads"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			AsyncExceptionWasm, ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 22 authorizes Task language-error fault"),
		Catalog && Catalog->SupportsTaskLanguageErrorFault());
	auto DirectEmpty = Document(23);
	DirectEmpty->SetArrayField(TEXT("types"), {});
	DirectEmpty->SetArrayField(TEXT("sources"), {});
	const FString DirectEmptyJson = Json(DirectEmpty);
	const TArray<uint8> DirectEmptyWasm = Module(&DirectEmptyJson, true, false, 23);
	Catalog.Reset();
	TestTrue(TEXT("IR 23 permits a paired empty language-error catalog"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			DirectEmptyWasm, ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 23 empty catalog retains Task fault ABI"),
		Catalog && Catalog->SupportsTaskLanguageErrorFault());

	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("canonical metadata loads")),
				Runtime.LoadModule(ValidWasm.GetData(), ValidWasm.Num(), ModuleId, Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestNotNull(TEXT("loaded runtime retains validated catalog"), Runtime.GetLanguageErrorCatalog());
		FAvidScriptHostCall FaultCall;
		FaultCall.BindingId = EAvidScriptHostBindingId::TaskFaultLanguageErrorV1;
		FAvidScriptHostCallResult FaultResult;
		TestFalse(TEXT("IR 17 cannot use Task language-error fault import"),
			Runtime.DispatchHostCall(FaultCall, FaultResult));
		TestEqual(TEXT("Old IR has a stable import-version error"), FaultResult.ErrorCategory,
			FString(TEXT("task_language_error_version")));
		Runtime.Unload();
		TestNull(TEXT("unload clears language-error catalog"), Runtime.GetLanguageErrorCatalog());
		const TArray<uint8> OldWasm = Module(nullptr, false);
		TestTrue(TEXT("old module without catalog still loads"),
			Runtime.LoadModule(OldWasm.GetData(), OldWasm.Num(), TEXT("old_module"), Result));
		TestNull(TEXT("old module has no stale catalog"), Runtime.GetLanguageErrorCatalog());
		Runtime.Unload();
	}

	TArray<TPair<FString, TArray<uint8>>> Invalid;
	auto BadSchema = Document();
	BadSchema->SetNumberField(TEXT("schema_version"), 2);
	const FString BadSchemaJson = Json(BadSchema);
	Invalid.Emplace(TEXT("future schema"), Module(&BadSchemaJson));
	auto BadType = Document();
	BadType->GetArrayField(TEXT("types"))[0]->AsObject()->SetNumberField(TEXT("token"), 2);
	const FString BadTypeJson = Json(BadType);
	Invalid.Emplace(TEXT("noncanonical type token"), Module(&BadTypeJson));
	auto BadSpan = Document();
	BadSpan->GetArrayField(TEXT("sources"))[0]->AsObject()->SetNumberField(TEXT("start"), 95);
	const FString BadSpanJson = Json(BadSpan);
	Invalid.Emplace(TEXT("out-of-bounds source span"), Module(&BadSpanJson));
	auto BadHash = Document();
	BadHash->SetStringField(TEXT("source_sha256"), FString::ChrN(64, 'd'));
	const FString BadHashJson = Json(BadHash);
	Invalid.Emplace(TEXT("provenance mismatch"), Module(&BadHashJson));
	Invalid.Emplace(TEXT("missing IR 17 catalog"), Module(nullptr));
	Invalid.Emplace(TEXT("missing IR 20 catalog"), Module(nullptr, true, false, 20));
	Invalid.Emplace(TEXT("missing IR 22 catalog"), Module(nullptr, true, false, 22));
	Invalid.Emplace(TEXT("missing IR 23 catalog"), Module(nullptr, true, false, 23));
	auto OldEmpty = Document(22);
	OldEmpty->SetArrayField(TEXT("types"), {});
	OldEmpty->SetArrayField(TEXT("sources"), {});
	const FString OldEmptyJson = Json(OldEmpty);
	Invalid.Emplace(TEXT("IR 22 cannot use an empty catalog"),
		Module(&OldEmptyJson, true, false, 22));
	Invalid.Emplace(TEXT("IR 20 catalog with IR 17 metadata"), Module(&ValidJson, true, false, 20));
	Invalid.Emplace(TEXT("IR 22 catalog with IR 21 provenance"),
		Module(&AsyncExceptionJson, true, false, 21));
	Invalid.Emplace(TEXT("IR 21 catalog with IR 22 provenance"),
		Module(&AsyncJson, true, false, 22));
	Invalid.Emplace(TEXT("missing provenance"), Module(&ValidJson, false));
	Invalid.Emplace(TEXT("duplicate section"), Module(&ValidJson, true, true));
	const FString DuplicateJson = FString::Printf(
		TEXT("{\"schema_version\":1,\"schema_version\":1,\"guest_ir_schema_version\":17,\"guest_ir_version\":\"1.16\",")
		TEXT("\"module_id\":\"language_catalog_test\",\"source_sha256\":\"%s\",")
		TEXT("\"types\":[{\"token\":1,\"type_id\":\"type:global::System.Exception\"}],")
		TEXT("\"sources\":[{\"token\":1,\"source_id\":\"Scripts/SourceThrow.cs\",")
		TEXT("\"source_length\":100,\"start\":5,\"length\":10,\"line\":1,\"column\":5,")
		TEXT("\"end_line\":1,\"end_column\":15}]}"), *SourceSha256);
	Invalid.Emplace(TEXT("duplicate JSON key"), Module(&DuplicateJson));
	TArray<uint8> MalformedProvenance = Module(nullptr, false);
	Custom(MalformedProvenance, "avidscript.provenance", TEXT("guest_ir=17/1.16"));
	Invalid.Emplace(TEXT("malformed provenance"), MoveTemp(MalformedProvenance));
	TArray<uint8> Truncated = ValidWasm;
	Truncated.Add(0);
	Truncated.Add(0x80);
	Invalid.Emplace(TEXT("truncated trailing section"), MoveTemp(Truncated));
	for (const auto& Scenario : Invalid)
	{
		Catalog.Reset();
		Error.Reset();
		TestFalse(*Scenario.Key, FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Scenario.Value, ModuleId, Catalog, Error));
		TestTrue(TEXT("rejection has no partial catalog"), !Catalog && !Error.IsEmpty());
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult Result;
		TestFalse(*FString::Printf(TEXT("runtime rejects %s"), *Scenario.Key),
			Runtime.LoadModule(Scenario.Value.GetData(), Scenario.Value.Num(), ModuleId, Result));
		TestEqual(TEXT("metadata rejection has a stable category"), Result.ErrorCategory,
			FString(TEXT("invalid_language_error_metadata")));
		TestNull(TEXT("metadata rejection retains no catalog"), Runtime.GetLanguageErrorCatalog());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptLanguageErrorCatalogRealArtifactTest,
	"AvidScript.Runtime.LanguageErrorCatalog.RealCompilerArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptLanguageErrorCatalogRealArtifactTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptLanguageErrorCatalogTests/GuestFixtures/throw-caller.wasm"));
	TArray<uint8> CanonicalWasm;
	if (!TestTrue(TEXT("read C# compiler WASM fixture"), FFileHelper::LoadFileToArray(CanonicalWasm, *Path)))
		return false;
	const FString ModuleId = TEXT("csharp:Scripts/SourceThrow.cs");
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("real IR 17 WASM loads")),
				Runtime.LoadModule(CanonicalWasm.GetData(), CanonicalWasm.Num(), ModuleId, Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		const FAvidScriptLanguageErrorCatalog* Catalog = Runtime.GetLanguageErrorCatalog();
		const FString* Type = Catalog ? Catalog->FindType(1) : nullptr;
		const FAvidScriptLanguageErrorSource* Source = Catalog ? Catalog->FindSource(1) : nullptr;
		TestTrue(TEXT("real C# exception type is retained"), Type
			&& *Type == TEXT("type:global::System.Exception"));
		TestTrue(TEXT("real C# throw source is retained"), Source
			&& Source->SourceId == TEXT("Scripts/SourceThrow.cs")
			&& Source->Start >= 0 && Source->Length > 0
			&& Source->Start + Source->Length <= Source->SourceLength);
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("uncaught UE entry fails")),
			Runtime.BeginPlay(Result));
		TestEqual(TEXT("uncaught language error has a distinct category"),
			Result.ErrorCategory, FString(TEXT("language_error_uncaught")));
		TestTrue(TEXT("uncaught report includes type and source position"),
			Type && Source && Result.ErrorMessage.Contains(*Type)
			&& Result.ErrorMessage.Contains(Source->SourceId)
			&& Result.ErrorMessage.Contains(FString::Printf(TEXT(":%d:%d"), Source->Line, Source->Column)));
		const AvidScript::Managed::FHeap* Heap = Runtime.GetManagedHeapForTesting();
		TestTrue(TEXT("uncaught call releases managed invocation roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
		const FString SourceId = Source ? Source->SourceId : FString();
		Runtime.Unload();
		TestNull(TEXT("real compiler catalog is released on unload"), Runtime.GetLanguageErrorCatalog());
		FAvidScriptRuntimeSession Session;
		Session.SetBackendSelectionForTesting(Lane.Selection);
		FAvidScriptWasmReloadManifest Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(ModuleId);
		Manifest.RequiredExports = {TEXT("avid_on_begin_play")};
		Manifest.RequiredImports = {
			{TEXT("avidscript"), TEXT("avid_managed_heap_v1")},
			{TEXT("avidscript"), TEXT("avid_language_error_report_v1")},
		};
		FAvidScriptWasmReloadResult SessionResult;
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Session rejects uncaught BeginPlay")),
			Session.LoadInitialModule(CanonicalWasm.GetData(), CanonicalWasm.Num(), Manifest, SessionResult));
		TestEqual(TEXT("Session preserves language error category"),
			SessionResult.ErrorCategory, FString(TEXT("language_error_uncaught")));
		TestTrue(TEXT("Session preserves source diagnostic"), !SourceId.IsEmpty()
			&& SessionResult.ErrorMessage.Contains(SourceId));
		TestFalse(TEXT("failed initial activation has no live Session VM"), Session.IsLiveLoaded());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptLanguageErrorCatalogHandledArtifactTest,
	"AvidScript.Runtime.LanguageErrorCatalog.HandledCompilerArtifact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptLanguageErrorCatalogHandledArtifactTest::RunTest(const FString& Parameters)
{
	const FString Path = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptLanguageErrorCatalogTests/GuestFixtures/multi-catch.wasm"));
	TArray<uint8> CanonicalWasm;
	if (!TestTrue(TEXT("read C# catch compiler WASM fixture"),
			FFileHelper::LoadFileToArray(CanonicalWasm, *Path)))
		return false;
	const FString ModuleId = TEXT("csharp:Scripts/SourceThrow.cs");
	const TArray<FAvidScriptRuntimeBackendTestLane> Lanes = GetAvidScriptRuntimeBackendTestLanes();
	if (!TestEqual(TEXT("handled language errors require WAMR and Wasmtime lanes"), Lanes.Num(), 2))
		return false;
	for (const FAvidScriptRuntimeBackendTestLane& Lane : Lanes)
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("handled IR 17 WASM loads")),
				Runtime.LoadModule(CanonicalWasm.GetData(), CanonicalWasm.Num(), ModuleId, Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		const FAvidScriptLanguageErrorCatalog* Catalog = Runtime.GetLanguageErrorCatalog();
		const FString* Type = Catalog ? Catalog->FindType(1) : nullptr;
		const FAvidScriptLanguageErrorSource* FirstSource = Catalog ? Catalog->FindSource(1) : nullptr;
		const FAvidScriptLanguageErrorSource* SecondSource = Catalog ? Catalog->FindSource(2) : nullptr;
		TestTrue(TEXT("handled C# exception type is retained"), Type
			&& *Type == TEXT("type:global::System.Exception"));
		TestTrue(TEXT("both throw sites retain distinct source positions"), FirstSource
			&& SecondSource && FirstSource->SourceId == SecondSource->SourceId
			&& FirstSource->Start < SecondSource->Start);
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("caught BeginPlay succeeds")),
				Runtime.BeginPlay(Result)))
			AddError(Result.ErrorMessage);
		const AvidScript::Managed::FHeap* Heap = Runtime.GetManagedHeapForTesting();
		TestTrue(TEXT("handled call releases managed invocation roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
		Runtime.Unload();
		TestNull(TEXT("handled compiler catalog is released on unload"),
			Runtime.GetLanguageErrorCatalog());
	}
	const FString TypedPath = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptLanguageErrorCatalogTests/GuestFixtures/typed-catch.wasm"));
	TArray<uint8> TypedWasm;
	if (!TestTrue(TEXT("read typed C# catch compiler WASM fixture"),
			FFileHelper::LoadFileToArray(TypedWasm, *TypedPath)))
		return false;
	for (const FAvidScriptRuntimeBackendTestLane& Lane : Lanes)
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("typed catch WASM loads")),
				Runtime.LoadModule(TypedWasm.GetData(), TypedWasm.Num(), ModuleId, Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		const FAvidScriptLanguageErrorCatalog* Catalog = Runtime.GetLanguageErrorCatalog();
		const FString* ArgumentType = Catalog ? Catalog->FindType(1) : nullptr;
		const FString* InvalidType = Catalog ? Catalog->FindType(2) : nullptr;
		const FAvidScriptLanguageErrorSource* FirstSource = Catalog ? Catalog->FindSource(1) : nullptr;
		const FAvidScriptLanguageErrorSource* SecondSource = Catalog ? Catalog->FindSource(2) : nullptr;
		TestTrue(TEXT("typed catches retain ordered dynamic exception types and sources"),
			ArgumentType && InvalidType && FirstSource && SecondSource
			&& *ArgumentType == TEXT("type:global::System.ArgumentException")
			&& *InvalidType == TEXT("type:global::System.InvalidOperationException")
			&& FirstSource->SourceId == SecondSource->SourceId
			&& FirstSource->Start < SecondSource->Start && !Catalog->FindType(3));
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("typed catch BeginPlay succeeds")),
				Runtime.BeginPlay(Result)))
			AddError(Result.ErrorMessage);
		const AvidScript::Managed::FHeap* Heap = Runtime.GetManagedHeapForTesting();
		TestTrue(TEXT("typed catches release managed invocation roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
		Runtime.Unload();
		TestNull(TEXT("typed catch catalog is released on unload"),
			Runtime.GetLanguageErrorCatalog());
	}
	const FString LocalPath = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptLanguageErrorCatalogTests/GuestFixtures/local-catch.wasm"));
	TArray<uint8> LocalWasm;
	if (!TestTrue(TEXT("read same-method C# catch compiler WASM fixture"),
			FFileHelper::LoadFileToArray(LocalWasm, *LocalPath)))
		return false;
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("local catch WASM loads")),
				Runtime.LoadModule(LocalWasm.GetData(), LocalWasm.Num(), ModuleId, Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		const FAvidScriptLanguageErrorCatalog* Catalog = Runtime.GetLanguageErrorCatalog();
		const FString* Type = Catalog ? Catalog->FindType(1) : nullptr;
		const FAvidScriptLanguageErrorSource* Source = Catalog ? Catalog->FindSource(1) : nullptr;
		TestTrue(TEXT("local throw retains its type and source"), Type && Source
			&& *Type == TEXT("type:global::System.Exception")
			&& Source->SourceId == TEXT("Scripts/SourceThrow.cs")
			&& !Catalog->FindSource(2));
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("local catch BeginPlay succeeds")),
				Runtime.BeginPlay(Result)))
			AddError(Result.ErrorMessage);
		const AvidScript::Managed::FHeap* Heap = Runtime.GetManagedHeapForTesting();
		TestTrue(TEXT("local catch releases managed invocation roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
		Runtime.Unload();
		TestNull(TEXT("local catch catalog is released on unload"),
			Runtime.GetLanguageErrorCatalog());
	}
	const TArray<FString> HandledFixtures = {
		TEXT("finally-catch.wasm"), TEXT("nested-finally-catch.wasm"),
		TEXT("throw-finally-catch.wasm"), TEXT("nested-local-throw-finally.wasm"),
		TEXT("multi-local-throw-finally.wasm"), TEXT("side-effect-throw-finally.wasm"),
		TEXT("mixed-local-throw-finally.wasm"),
		TEXT("mixed-branching-finally.wasm"),
		TEXT("called-return-finally.wasm"), TEXT("called-branching-finally.wasm"),
		TEXT("catch-finally.wasm"), TEXT("catch-branching-finally.wasm"),
		TEXT("cleanup-replaces-error.wasm"), TEXT("nested-cleanup-replaces-error.wasm"),
		TEXT("outer-nested-cleanup-replaces-error.wasm"),
		TEXT("outermost-cleanup-replaces-error.wasm"),
		TEXT("consecutive-nested-cleanup-replaces-error.wasm"),
		TEXT("branching-cleanup.wasm"),
		TEXT("catch-rethrow.wasm"), TEXT("catch-rethrow-finally.wasm"),
		TEXT("catch-rethrow-branching-finally.wasm"), TEXT("nested-rethrow.wasm"),
		TEXT("nested-catch-branching-finally.wasm"),
		TEXT("local-catch-rethrow-finally.wasm"),
		TEXT("local-catch-rethrow-branching-finally.wasm"),
		TEXT("catch-variable.wasm"), TEXT("conditional-guard.wasm"),
		TEXT("void-throw-producer.wasm"), TEXT("conditional-void-guard.wasm")};
	for (const FString& FixtureName : HandledFixtures)
	{
		const FString FixturePath = FPaths::Combine(FPaths::ProjectSavedDir(),
			TEXT("AvidScriptLanguageErrorCatalogTests/GuestFixtures"), FixtureName);
		TArray<uint8> FixtureWasm;
		if (!TestTrue(*FString::Printf(TEXT("read C# handled fixture %s"), *FixtureName),
				FFileHelper::LoadFileToArray(FixtureWasm, *FixturePath)))
			return false;
		for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
		{
			FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
			FAvidScriptWasmSmokeResult Result;
			const FString FixtureLabel = FString::Printf(TEXT("%s loads"), *FixtureName);
			if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, *FixtureLabel),
					Runtime.LoadModule(FixtureWasm.GetData(), FixtureWasm.Num(), ModuleId, Result)))
			{
				AddError(Result.ErrorMessage);
				continue;
			}
			TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
			const FAvidScriptLanguageErrorCatalog* Catalog = Runtime.GetLanguageErrorCatalog();
			const FString* Type = Catalog ? Catalog->FindType(1) : nullptr;
			const FAvidScriptLanguageErrorSource* Source = Catalog ? Catalog->FindSource(1) : nullptr;
			const bool bHasThirdThrow = FixtureName == TEXT("multi-local-throw-finally.wasm")
				|| FixtureName == TEXT("mixed-branching-finally.wasm")
				|| FixtureName == TEXT("consecutive-nested-cleanup-replaces-error.wasm");
			const bool bHasSecondThrow = bHasThirdThrow
				|| FixtureName == TEXT("side-effect-throw-finally.wasm")
				|| FixtureName == TEXT("mixed-local-throw-finally.wasm")
				|| FixtureName == TEXT("called-return-finally.wasm")
				|| FixtureName == TEXT("catch-finally.wasm")
				|| FixtureName == TEXT("catch-branching-finally.wasm")
				|| FixtureName == TEXT("cleanup-replaces-error.wasm")
				|| FixtureName == TEXT("nested-cleanup-replaces-error.wasm")
				|| FixtureName == TEXT("outer-nested-cleanup-replaces-error.wasm")
				|| FixtureName == TEXT("outermost-cleanup-replaces-error.wasm")
				|| FixtureName == TEXT("catch-rethrow.wasm")
				|| FixtureName == TEXT("nested-rethrow.wasm")
				|| FixtureName == TEXT("catch-variable.wasm");
			const FAvidScriptLanguageErrorSource* SecondSource =
				Catalog ? Catalog->FindSource(2) : nullptr;
			const FAvidScriptLanguageErrorSource* ThirdSource =
				Catalog ? Catalog->FindSource(3) : nullptr;
			TestTrue(*FString::Printf(TEXT("%s retains its throw type and source"), *FixtureName),
				Type && Source && *Type == TEXT("type:global::System.Exception")
				&& Source->SourceId == TEXT("Scripts/SourceThrow.cs")
				&& (bHasSecondThrow ? SecondSource
					&& SecondSource->SourceId == Source->SourceId
					&& SecondSource->Start > Source->Start
					&& (bHasThirdThrow ? ThirdSource
						&& ThirdSource->SourceId == Source->SourceId
						&& ThirdSource->Start > SecondSource->Start
						&& !Catalog->FindSource(4) : !ThirdSource)
					: !SecondSource && !ThirdSource));
			if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane,
					*FString::Printf(TEXT("%s BeginPlay succeeds"), *FixtureName)),
					Runtime.BeginPlay(Result)))
				AddError(Result.ErrorMessage);
			const AvidScript::Managed::FHeap* Heap = Runtime.GetManagedHeapForTesting();
			TestTrue(*FString::Printf(TEXT("%s releases managed invocation roots"), *FixtureName),
				Heap && Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
			Runtime.Unload();
			TestNull(*FString::Printf(TEXT("%s catalog is released on unload"), *FixtureName),
				Runtime.GetLanguageErrorCatalog());
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTaskLanguageErrorVmImportTest,
	"AvidScript.Runtime.LanguageErrorCatalog.TaskFaultVmImport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskLanguageErrorVmImportTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptLanguageErrorCatalogTests;
	const FString FixturePath = FPaths::Combine(FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Tests/Fixtures/WasmBackend/P66_TaskLanguageError.wasm"));
	TArray<uint8> GeneratedWasm;
	if (!TestTrue(TEXT("read production-backend IR 20 WASM fixture"),
		FFileHelper::LoadFileToArray(GeneratedWasm, *FixturePath)))
		return false;
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("generated IR 20 WASM loads")),
			Runtime.LoadModule(GeneratedWasm.GetData(), GeneratedWasm.Num(), TEXT("minimal"), Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		TestTrue(TEXT("generated IR 20 catalog authorizes the fault import"),
			Runtime.GetLanguageErrorCatalog()
			&& Runtime.GetLanguageErrorCatalog()->SupportsTaskLanguageErrorFault());
		Runtime.Unload();
	}
	for (const int32 GuestSchema : {20, 21, 22, 23, 17})
	{
		const TArray<uint8> Wasm = TaskFaultImportModule(GuestSchema);
		for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
		{
			FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
			FAvidScriptWasmSmokeResult Result;
			if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("fault import WASM loads")),
				Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), ModuleId, Result)))
			{
				AddError(Result.ErrorMessage);
				continue;
			}
			TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
			const bool bAllowedVersion = GuestSchema >= 20 && GuestSchema <= 23;
			TestEqual(TEXT("catalog gates the imported function by IR version"),
				Runtime.GetLanguageErrorCatalog()->SupportsTaskLanguageErrorFault(), bAllowedVersion);
			TestFalse(TEXT("WASM call rejects missing Session task context"),
				Runtime.BeginPlay(Result));
			TestEqual(TEXT("VM import preserves the Host rejection category"),
				Result.ErrorCategory, bAllowedVersion
					? FString(TEXT("task_result_context"))
					: FString(TEXT("task_language_error_version")));
			TestEqual(TEXT("VM reports the called import"), Result.ImportName,
				FString(TEXT("avid_task_fault_language_error_v1")));
			const AvidScript::Managed::FHeap* Heap = Runtime.GetManagedHeapForTesting();
			TestTrue(TEXT("rejected VM import leaves no managed roots"), Heap
				&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
			Runtime.Unload();
		}
	}
	for (const ANSICHAR* ImportName : {
		AvidScript::TaskResult::Abi::LanguageErrorMetaImport,
		AvidScript::TaskResult::Abi::LanguageErrorRootImport })
	{
		for (const int32 GuestSchema : {20, 21, 22, 23, 17})
		{
			const TArray<uint8> Wasm = TaskReadImportModule(GuestSchema, ImportName);
			for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
			{
				FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
				FAvidScriptWasmSmokeResult Result;
				if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Task read import WASM loads")),
					Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), ModuleId, Result)))
				{
					AddError(Result.ErrorMessage);
					continue;
				}
				TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
				TestFalse(TEXT("Task read import rejects missing Session task context"),
					Runtime.BeginPlay(Result));
				TestEqual(TEXT("Task read import preserves version or context rejection"),
					Result.ErrorCategory, GuestSchema >= 20 && GuestSchema <= 23
						? FString(TEXT("task_result_context"))
						: FString(TEXT("task_language_error_version")));
				TestEqual(TEXT("VM reports the called Task read import"),
					Result.ImportName, FString(UTF8_TO_TCHAR(ImportName)));
				const AvidScript::Managed::FHeap* Heap = Runtime.GetManagedHeapForTesting();
				TestTrue(TEXT("Rejected Task read import leaves no managed roots"), Heap
					&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
				Runtime.Unload();
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTaskLanguageErrorAdmissionTest,
	"AvidScript.Runtime.Continuation.TaskLanguageErrorAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskLanguageErrorAdmissionTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptLanguageErrorCatalogTests;
	using namespace AvidScript::Managed;
	const FString Metadata = Json(Document(20));
	const TArray<uint8> Wasm = Module(&Metadata, true, false, 20);
	TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
	if (!TestNotNull(TEXT("Task error test world exists"), World.Get())) return false;
	const TArray<FAvidScriptRuntimeBackendTestLane> Lanes = GetAvidScriptRuntimeBackendTestLanes();
	if (!TestEqual(TEXT("Task error admission covers two VM lanes"), Lanes.Num(), 2)) return false;
	for (const FAvidScriptRuntimeBackendTestLane& Lane : Lanes)
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult LoadResult;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("catalog-bearing WASM loads")),
			Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), ModuleId, LoadResult)))
		{
			AddError(LoadResult.ErrorMessage);
			return false;
		}
		FHeap* Heap = Runtime.GetManagedHeapForTesting();
		const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
		if (!TestTrue(TEXT("Task error heap configures"), Heap
			&& Heap->Configure(Layouts) == EHeapError::Ok)) return false;
		const TSharedPtr<FAvidScriptSessionContinuations> Owner =
			MakeShared<FAvidScriptSessionContinuations>();
		FAvidScriptContinuationHostEndpoint& Endpoint = Owner->ResetActive(World.Get());
		const int64 Task = Endpoint.CreateTaskResult(TEXT("type:int32"));
		const int64 Target = Endpoint.CreateTaskResult(TEXT("type:int32"));
		const TSharedPtr<FAvidScriptSessionContinuations> ForeignOwner =
			MakeShared<FAvidScriptSessionContinuations>();
		FAvidScriptContinuationHostEndpoint& Foreign = ForeignOwner->ResetActive(World.Get());
		const int64 ForeignTask = Foreign.CreateTaskResult(TEXT("type:int32"));
		if (!TestTrue(TEXT("Session tasks exist"), Task > 0 && Target > 0 && ForeignTask > 0))
			return false;
		FAvidScriptWasmHostContext Context;
		Context.World = World.Get();
		Context.Tasks = &Endpoint;
		Context.Continuations = &Endpoint;
		Runtime.SetHostContext(Context);
		FAvidScriptHostCallResult Result;
		auto InvokeFaultImport = [&Runtime](int64 TaskToken, int32 TypeToken,
			int32 SourceToken, uint64 ObjectToken, FAvidScriptHostCallResult& OutResult)
		{
			FAvidScriptHostCall Call;
			Call.BindingId = EAvidScriptHostBindingId::TaskFaultLanguageErrorV1;
			Call.Int64Args[0] = TaskToken;
			Call.IntArgs[0] = TypeToken;
			Call.IntArgs[1] = SourceToken;
			Call.Int64Args[1] = static_cast<int64>(ObjectToken);
			return Runtime.DispatchHostCall(Call, OutResult);
		};
		auto InvokeReadImport = [&Runtime](EAvidScriptHostBindingId BindingId,
			int64 TaskToken, FAvidScriptHostCallResult& OutResult)
		{
			FAvidScriptHostCall Call;
			Call.BindingId = BindingId;
			Call.Int64Args[0] = TaskToken;
			return Runtime.DispatchHostCall(Call, OutResult);
		};
		TestTrue(TEXT("Combined IR catalog authorizes Task error import"),
			Runtime.GetLanguageErrorCatalog()
			&& Runtime.GetLanguageErrorCatalog()->SupportsTaskLanguageErrorFault());
		FAvidScriptTaskLanguageError ReadError;
		TestFalse(TEXT("Outside VM invocation cannot read a Task error"),
			Runtime.ReadTaskLanguageError(Task, ReadError, Result));
		TestEqual(TEXT("Missing read invocation category"), Result.ErrorCategory,
			FString(TEXT("task_result_context")));
		TestFalse(TEXT("Outside VM invocation cannot read Task error metadata"),
			InvokeReadImport(EAvidScriptHostBindingId::TaskLanguageErrorMetaV1, Task, Result));
		TestEqual(TEXT("Metadata read needs an invocation"), Result.ErrorCategory,
			FString(TEXT("task_result_context")));
		TestFalse(TEXT("Outside VM invocation cannot fault a task"),
			InvokeFaultImport(Task, 1, 1, 1, Result));
		TestEqual(TEXT("Missing invocation category"), Result.ErrorCategory,
			FString(TEXT("task_result_context")));
		FToken OuterFrame = 0, OuterRoot = 0, OuterObject = 0;
		TestTrue(TEXT("Older frame starts"), Heap->PushFrame(OuterFrame) == EHeapError::Ok);
		TestTrue(TEXT("Older frame root exists"),
			Heap->CreateRoot(OuterFrame, 0, OuterRoot) == EHeapError::Ok);
		TestTrue(TEXT("Older frame object allocates"),
			Heap->Allocate(1, OuterRoot, OuterObject) == EHeapError::Ok);
		const uint64 Invocation = Runtime.BeginVmInvocation();
		FToken Frame = 0, Root = 0, ErrorObject = 0;
		TestTrue(TEXT("Current invocation frame starts"),
			Heap->PushFrame(Frame) == EHeapError::Ok);
		TestTrue(TEXT("Current frame root exists"),
			Heap->CreateRoot(Frame, 0, Root) == EHeapError::Ok);
		TestTrue(TEXT("Current error object allocates"),
			Heap->Allocate(1, Root, ErrorObject) == EHeapError::Ok);
		TestFalse(TEXT("Foreign Session task is rejected"),
			InvokeFaultImport(ForeignTask, 1, 1, ErrorObject, Result));
		TestEqual(TEXT("Foreign task has identity category"), Result.ErrorCategory,
			FString(TEXT("task_result_identity")));
		TestFalse(TEXT("Unknown type token is rejected"),
			InvokeFaultImport(Task, 2, 1, ErrorObject, Result));
		TestEqual(TEXT("Unknown type has catalog category"), Result.ErrorCategory,
			FString(TEXT("task_language_error_catalog")));
		TestFalse(TEXT("Unknown source token is rejected"),
			InvokeFaultImport(Task, 1, 2, ErrorObject, Result));
		TestFalse(TEXT("Older frame root cannot be submitted"),
			InvokeFaultImport(Task, 1, 1, OuterObject, Result));
		TestEqual(TEXT("Older frame has root category"), Result.ErrorCategory,
			FString(TEXT("task_language_error_root")));
		FAvidScriptTaskResultSnapshot Snapshot;
		TestFalse(TEXT("Rejected reports leave task running"),
			Endpoint.ReadTaskResult(Task, Snapshot));
		const uint32 RootsBeforeRead = Heap->GetStats().LiveRoots;
		TestFalse(TEXT("Running Task has no language error to read"),
			Runtime.ReadTaskLanguageError(Task, ReadError, Result));
		TestEqual(TEXT("Rejected read preserves root count"),
			Heap->GetStats().LiveRoots, RootsBeforeRead);
		TestTrue(TEXT("Current frame root faults Task<int>"),
			InvokeFaultImport(Task, 1, 1, ErrorObject, Result));
		TestTrue(TEXT("Admission returns success"), Result.bSucceeded && Result.ReturnValue == 1);
		const uint32 LiveRoots = Heap->GetStats().LiveRoots;
		TestFalse(TEXT("Completed task cannot be faulted twice"),
			InvokeFaultImport(Task, 1, 1, ErrorObject, Result));
		TestEqual(TEXT("Rejected completion category"), Result.ErrorCategory,
			FString(TEXT("task_result_complete")));
		TestEqual(TEXT("Rejected completion releases temporary root"),
			Heap->GetStats().LiveRoots, LiveRoots);
		TestTrue(TEXT("Task reports catalog and object tokens"),
			Endpoint.ReadTaskResult(Task, Snapshot)
			&& Snapshot.LanguageError.IsSet()
			&& Snapshot.LanguageError->TypeToken == 1
			&& Snapshot.LanguageError->SourceToken == 1
			&& Snapshot.LanguageError->ObjectToken == ErrorObject);
		TArray<int64> Waiters;
		TestTrue(TEXT("Session propagates rooted language error"),
			Endpoint.PropagateTaskFailure(Task, Target, Waiters));
		const uint32 RootsBeforeAcquisition = Heap->GetStats().LiveRoots;
		TestTrue(TEXT("Task error metadata import returns catalog tokens"),
			InvokeReadImport(EAvidScriptHostBindingId::TaskLanguageErrorMetaV1, Task, Result)
			&& static_cast<uint64>(Result.ReturnValueI64) == ((uint64(1) << 32) | 1));
		TestEqual(TEXT("Metadata read does not root the error object"),
			Heap->GetStats().LiveRoots, RootsBeforeAcquisition);
		TestTrue(TEXT("Task error root import returns the object"),
			InvokeReadImport(EAvidScriptHostBindingId::TaskLanguageErrorRootV1, Task, Result)
			&& static_cast<uint64>(Result.ReturnValueI64) == ErrorObject);
		TestEqual(TEXT("Root import adds one invocation-owned root"),
			Heap->GetStats().LiveRoots, RootsBeforeAcquisition + 1);
		TestTrue(TEXT("Faulted Task error reads into current invocation"),
			Runtime.ReadTaskLanguageError(Task, ReadError, Result)
			&& ReadError.TypeToken == 1 && ReadError.SourceToken == 1
			&& ReadError.ObjectToken == ErrorObject);
		TestEqual(TEXT("Native read adds one more invocation-owned root"),
			Heap->GetStats().LiveRoots, RootsBeforeAcquisition + 2);
		TestFalse(TEXT("Foreign Session Task error cannot be read"),
			Runtime.ReadTaskLanguageError(ForeignTask, ReadError, Result));
		TestEqual(TEXT("Foreign read has identity category"), Result.ErrorCategory,
			FString(TEXT("task_result_identity")));
		TestFalse(TEXT("Foreign Session Task metadata cannot be read"),
			InvokeReadImport(EAvidScriptHostBindingId::TaskLanguageErrorMetaV1,
				ForeignTask, Result));
		TestEqual(TEXT("Foreign metadata read has identity category"), Result.ErrorCategory,
			FString(TEXT("task_result_identity")));
		Runtime.EndVmInvocation(Invocation);
		TestTrue(TEXT("Older frame closes"), Heap->PopFrame(OuterFrame) == EHeapError::Ok);
		TestTrue(TEXT("Root survives invocation exit"), Heap->Collect() == EHeapError::Ok);
		TestTrue(TEXT("Faulted tasks retain error object"), Heap->IsAlive(ErrorObject));
		TestTrue(TEXT("Source task releases"), Endpoint.ReleaseTaskResult(Task));
		TestTrue(TEXT("Target retains root after source release"),
			Heap->Collect() == EHeapError::Ok && Heap->IsAlive(ErrorObject));
		const uint64 ReadInvocation = Runtime.BeginVmInvocation();
		const uint32 RootsWithoutReadFrame = Heap->GetStats().LiveRoots;
		TestTrue(TEXT("Metadata read needs no new object root"),
			InvokeReadImport(EAvidScriptHostBindingId::TaskLanguageErrorMetaV1, Target, Result)
			&& static_cast<uint64>(Result.ReturnValueI64) == ((uint64(1) << 32) | 1));
		TestFalse(TEXT("Task error read needs a frame above the invocation floor"),
			Runtime.ReadTaskLanguageError(Target, ReadError, Result));
		TestEqual(TEXT("Missing frame read has root category"), Result.ErrorCategory,
			FString(TEXT("task_language_error_root")));
		TestEqual(TEXT("Missing frame read leaves roots unchanged"),
			Heap->GetStats().LiveRoots, RootsWithoutReadFrame);
		FToken ReadFrame = 0;
		TestTrue(TEXT("Awaiter invocation frame starts"),
			Heap->PushFrame(ReadFrame) == EHeapError::Ok);
		TestTrue(TEXT("Propagated Task error root import enters awaiter frame"),
			InvokeReadImport(EAvidScriptHostBindingId::TaskLanguageErrorRootV1, Target, Result)
			&& static_cast<uint64>(Result.ReturnValueI64) == ErrorObject);
		TestTrue(TEXT("Target task releases"), Endpoint.ReleaseTaskResult(Target));
		TestTrue(TEXT("Awaiter frame keeps object alive after last Task release"),
			Heap->Collect() == EHeapError::Ok && Heap->IsAlive(ErrorObject)
			&& Heap->GetStats().LiveRoots == 1);
		Runtime.EndVmInvocation(ReadInvocation);
		TestTrue(TEXT("Awaiter frame exit releases last language-error root"),
			Heap->Collect() == EHeapError::Ok && !Heap->IsAlive(ErrorObject)
			&& Heap->GetStats().LiveRoots == 0);
		ForeignOwner->Teardown();
		Owner->Teardown();
		Runtime.Unload();
	}
	return true;
}

#endif
