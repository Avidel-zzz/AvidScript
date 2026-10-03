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

FString Provenance(int32 GuestSchema = 17, const TCHAR* LifetimeModel = TEXT("cancellation"), int32 StaticBaseSchema = 0,
	bool bAsyncComposable = false)
{
	const FString GuestVersion = FString::Printf(TEXT("1.%d"), GuestSchema - 1);
	FString Result = FString::Printf(TEXT("module_id=%s\nsource_id=Scripts/SourceThrow.cs\nsource_sha256=%s\n")
		TEXT("frontend_sha256=%s\nsemantic_sha256=%s\nguest_ir=%d/%s"),
		*ModuleId, *SourceSha256, *FString::ChrN(64, 'b'), *FString::ChrN(64, 'c'),
		GuestSchema, *GuestVersion);
	if (GuestSchema == 35 && bAsyncComposable)
		Result += FString::Printf(TEXT("\ntask_local_exception_model=%s"), LifetimeModel);
	if (StaticBaseSchema)
	{
		if (GuestSchema == 35)
			Result += FString::Printf(TEXT("\nexecution_base=%d/1.%d"), StaticBaseSchema, StaticBaseSchema - 1);
		else
			Result += FString::Printf(TEXT("\nguest_ir_base=%d/1.%d"), StaticBaseSchema, StaticBaseSchema - 1);
	}
	const int32 ProfileSchema = GuestSchema == 30
		|| ((GuestSchema >= 31 && GuestSchema <= 34) && (StaticBaseSchema == 29 || StaticBaseSchema == 30))
		? 26 : StaticBaseSchema ? StaticBaseSchema : GuestSchema;
	if (ProfileSchema == 25 || ProfileSchema == 26) Result += FString::Printf(TEXT("\ntask_local_exception_model=%s"), LifetimeModel);
	if (GuestSchema == 33) Result += TEXT("\nsemantic=52/1.61");
	if (GuestSchema == 34) Result += TEXT("\nsemantic=53/1.62");
	if (GuestSchema == 35) Result += bAsyncComposable
		? TEXT("\ncapabilities=async.await_readiness@1,async.cancellation_identity@1,error.cancellation_token_value@1,error.exception_values@1,managed.static_storage@1")
		  TEXT("\nsource_language=csharp\nsemantic=54/1.63")
		: TEXT("\ncapabilities=error.cancellation_token_value@1,managed.static_storage@1")
		  TEXT("\nsource_language=csharp\nsemantic=54/1.63");
	return Result;
}

TSharedRef<FJsonObject> Document(int32 GuestSchema = 17)
{
	auto Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetNumberField(TEXT("guest_ir_schema_version"), GuestSchema);
	Root->SetStringField(TEXT("guest_ir_version"), FString::Printf(TEXT("1.%d"), GuestSchema - 1));
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

FString VoidOwnerProvenance(const TCHAR* Capabilities = TEXT("error.async_void_owner@1"))
{
	return Provenance(36) + FString::Printf(TEXT("\nexecution_base=29/1.28\ntask_local_exception_model=cancellation")
		TEXT("\ncapabilities=%s\nsource_language=csharp\nsemantic=55/1.64"), Capabilities);
}

FString VoidCompositionProvenance(const TCHAR* Capabilities = TEXT("error.async_void_owner@1,managed.static_storage@1"))
{
	return Provenance(37) + FString::Printf(TEXT("\nexecution_base=29/1.28\ntask_local_exception_model=cancellation")
		TEXT("\ncapabilities=%s\nsource_language=csharp\nsemantic=56/1.65\nsource_execution=55/1.64"), Capabilities);
}

TArray<uint8> VoidOwnerModule(const FString* Metadata, const FString& ProvenanceText = VoidOwnerProvenance())
{
	TArray<uint8> Wasm;
	Wasm.Append(BaseWasm, UE_ARRAY_COUNT(BaseWasm));
	Custom(Wasm, "avidscript.provenance", ProvenanceText);
	if (Metadata) Custom(Wasm, "avidscript.language_errors", *Metadata);
	return Wasm;
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
	bool bDuplicate = false, int32 GuestSchema = 17, const TCHAR* LifetimeModel = TEXT("cancellation"),
	int32 StaticBaseSchema = 0, bool bAsyncComposable = false)
{
	TArray<uint8> Wasm;
	Wasm.Append(BaseWasm, UE_ARRAY_COUNT(BaseWasm));
	if (bProvenance) Custom(Wasm, "avidscript.provenance", Provenance(GuestSchema, LifetimeModel, StaticBaseSchema, bAsyncComposable));
	if (Metadata)
	{
		Custom(Wasm, "avidscript.language_errors", *Metadata);
		if (bDuplicate) Custom(Wasm, "avidscript.language_errors", *Metadata);
	}
	return Wasm;
}

TArray<uint8> TaskFaultImportModule(int32 GuestSchema,
	const ANSICHAR* ImportName = AvidScript::TaskResult::Abi::FaultLanguageErrorImport)
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
	WasmName(Imports, ImportName);
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
	TestFalse(TEXT("IR 23 does not authorize cancellation payloads"),
		Catalog && Catalog->SupportsTaskCancellationError());
	auto CancellationDocument = Document(24);
	CancellationDocument->GetArrayField(TEXT("types"))[0]->AsObject()->SetStringField(
		TEXT("type_id"), TEXT("type:global::System.Threading.Tasks.TaskCanceledException"));
	const FString CancellationJson = Json(CancellationDocument);
	TestTrue(TEXT("IR 24 cancellation catalog loads"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&CancellationJson, true, false, 24), ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 24 authorizes typed cancellation and existing fault ABI"),
		Catalog && Catalog->SupportsTaskCancellationError()
		&& Catalog->SupportsTaskLanguageErrorFault() && Catalog->IsCancellationType(1));
	TestFalse(TEXT("Unknown cancellation type token is rejected"),
		Catalog && Catalog->IsCancellationType(2));
	const FString LifetimeJson = Json(Document(25));
	const FString RoutedThrowJson = Json(Document(26));
	TestTrue(TEXT("IR 26 routed throws load with the cancellation ownership profile"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&RoutedThrowJson, true, false, 26), ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 26 authorizes language faults and typed cancellation"),
		Catalog && Catalog->SupportsTaskLanguageErrorFault() && Catalog->SupportsTaskCancellationError());
	for (const TCHAR* Model : {TEXT("fault"), TEXT("exception"), TEXT("cleanup"), TEXT("cancellation")})
	{
		TestTrue(TEXT("IR 25 catalog loads with its explicit exception profile"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				Module(&LifetimeJson, true, false, 25, Model), ModuleId, Catalog, Error));
		TestTrue(TEXT("IR 25 error profiles authorize Task faults"), Catalog && Catalog->SupportsTaskLanguageErrorFault());
		TestEqual(TEXT("Only cancellation profile authorizes typed cancellation"),
			Catalog && Catalog->SupportsTaskCancellationError(), FCString::Strcmp(Model, TEXT("cancellation")) == 0);
	}
	for (const TCHAR* Model : {TEXT("none"), TEXT("cleanup_only")})
	{
		TestTrue(TEXT("IR 25 no-error profile loads without a catalog"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				Module(nullptr, true, false, 25, Model), ModuleId, Catalog, Error));
		TestNull(TEXT("No-error profile has no error capability"), Catalog.Get());
	}
	const FString StaticJson = Json(Document(27));
	const FString TransferJson = Json(Document(28));
	const FString SynchronousAsyncJson = Json(Document(29));
	const FString StaticAsyncJson = Json(Document(30));
	auto ReadinessDocument = Document(31);
	ReadinessDocument->GetArrayField(TEXT("types"))[0]->AsObject()->SetStringField(
		TEXT("type_id"), TEXT("type:global::System.Threading.Tasks.TaskCanceledException"));
	const FString ReadinessJson = Json(ReadinessDocument);
	for (const int32 Base : {24, 25, 26, 29, 30})
	{
		TestTrue(TEXT("IR 31 loads each supported cancellation execution profile"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				Module(&ReadinessJson, true, false, 31, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
		TestTrue(TEXT("IR 31 preserves language faults and typed cancellation"),
			Catalog && Catalog->SupportsTaskLanguageErrorFault() && Catalog->SupportsTaskCancellationError()
			&& Catalog->IsCancellationType(1));
		TestFalse(TEXT("IR 31 does not authorize cancellation identity"),
			Catalog && Catalog->SupportsTaskCancellationIdentity());
	}
	auto IdentityDocument = Document(32);
	const FString CatchJson = Json(Document(33));
	TestTrue(TEXT("IR 33 admits owned catch values with the exact base and source"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&CatchJson, true, false, 33, TEXT("cancellation"), 29), ModuleId, Catalog, Error));
	TestTrue(TEXT("Catch values preserve cancellation identity authorization"), Catalog && Catalog->SupportsTaskCancellationIdentity());
	for (const int32 Base : {0, 24, 25, 26, 30, 31, 32, 33})
		TestFalse(TEXT("IR 33 rejects another execution base"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&CatchJson, true, false, 33, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
	for (const FString& Metadata : {
		Provenance(33, TEXT("cancellation"), 29).Replace(TEXT("semantic=52/1.61"), TEXT("semantic=50/1.59")),
		Provenance(33, TEXT("cancellation"), 29).Replace(TEXT("\nsemantic=52/1.61"), TEXT("")),
		Provenance(33, TEXT("cancellation"), 29).Replace(TEXT("guest_ir=33/1.32"), TEXT("guest_ir=33/1.31"))})
	{
		TArray<uint8> Mismatched = Module(&CatchJson, false);
		Custom(Mismatched, "avidscript.provenance", Metadata);
		TestFalse(TEXT("IR 33 rejects missing or mismatched identity"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(Mismatched, ModuleId, Catalog, Error));
	}
	TestFalse(TEXT("IR 33 requires its language error catalog"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
		Module(nullptr, true, false, 33, TEXT("cancellation"), 29), ModuleId, Catalog, Error));
	auto TokenDocument = Document(34);
	TokenDocument->GetArrayField(TEXT("types"))[0]->AsObject()->SetStringField(
		TEXT("type_id"), TEXT("type:global::System.OperationCanceledException"));
	const FString TokenJson = Json(TokenDocument);
	TestTrue(TEXT("IR 34 pure values need no language catalog"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
		Module(nullptr, true, false, 34, TEXT("cancellation"), 14), ModuleId, Catalog, Error));
	TestNull(TEXT("Pure token values grant no error capability"), Catalog.Get());
	TestFalse(TEXT("IR 34 pure values cannot carry an error catalog"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
		Module(&TokenJson, true, false, 34, TEXT("cancellation"), 14), ModuleId, Catalog, Error));
	for (const int32 Base : {17, 29})
	{
		TestTrue(TEXT("IR 34 exact error base loads"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&TokenJson, true, false, 34, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
		TestTrue(TEXT("IR 34 authorizes exception object tokens"), Catalog && Catalog->SupportsExceptionCancellationToken()
			&& Catalog->IsCancellationType(1));
		TestEqual(TEXT("Only asynchronous token base authorizes Task identity"),
			Catalog && Catalog->SupportsTaskCancellationIdentity(), Base == 29);
		TestEqual(TEXT("Only asynchronous token base authorizes Task faults"),
			Catalog && Catalog->SupportsTaskLanguageErrorFault(), Base == 29);
		TestFalse(TEXT("IR 34 error base requires a catalog"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(nullptr, true, false, 34, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
	}
	for (const int32 Base : {0, 4, 16, 20, 24, 26, 30, 31, 32, 33, 34})
		TestFalse(TEXT("IR 34 rejects unsupported bases"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&TokenJson, true, false, 34, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
	for (const int32 Base : {14, 17, 29})
	for (const FString& Metadata : {
		Provenance(34, TEXT("cancellation"), Base).Replace(TEXT("semantic=53/1.62"), TEXT("semantic=52/1.61")),
		Provenance(34, TEXT("cancellation"), Base).Replace(TEXT("\nsemantic=53/1.62"), TEXT("")),
		Provenance(34, TEXT("cancellation"), Base).Replace(TEXT("guest_ir=34/1.33"), TEXT("guest_ir=34/1.32")),
		Provenance(34, TEXT("cancellation"), Base) + TEXT("\nsemantic=53/1.62"),
		Provenance(34, TEXT("cancellation"), Base) + TEXT("\nunknown=1")})
	{
		TArray<uint8> Mismatched = Module(Base == 14 ? nullptr : &TokenJson, false);
		Custom(Mismatched, "avidscript.provenance", Metadata);
		TestFalse(TEXT("IR 34 rejects wrong versions, source, duplicate and extra fields"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(Mismatched, ModuleId, Catalog, Error));
	}
	for (const TCHAR* Model : {TEXT("none"), TEXT("fault"), TEXT("cleanup"), TEXT("cleanup_only"), TEXT("unknown")})
		TestFalse(TEXT("IR 34 asynchronous base keeps cancellation lifetime ownership"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&TokenJson, true, false, 34, Model, 29), ModuleId, Catalog, Error));
	const FString ComposableJson = Json(Document(35));
	TestTrue(TEXT("IR 35 pure static/token values load without a language catalog"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(nullptr, true, false, 35, TEXT("cancellation"), 14), ModuleId, Catalog, Error));
	TestNull(TEXT("IR 35 pure values grant no language error capability"), Catalog.Get());
	TestFalse(TEXT("IR 35 pure values reject a language catalog"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&ComposableJson, true, false, 35, TEXT("cancellation"), 14), ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 35 static initializer base loads its language catalog"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&ComposableJson, true, false, 35, TEXT("cancellation"), 17), ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 35 static initializer preserves only its execution-base error profile"),
		Catalog && Catalog->FindType(1) && !Catalog->SupportsExceptionCancellationToken()
			&& !Catalog->SupportsTaskCancellationIdentity());
	TestFalse(TEXT("IR 35 static initializer base requires its catalog"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(nullptr, true, false, 35, TEXT("cancellation"), 17), ModuleId, Catalog, Error));
	TestFalse(TEXT("IR 35 pure values bind the expected module identity"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(nullptr, true, false, 35, TEXT("cancellation"), 14),
			TEXT("different_module"), Catalog, Error));
	for (const FString& Metadata : {
		Provenance(35, TEXT("cancellation"), 17).Replace(TEXT("guest_ir=35/1.34"), TEXT("guest_ir=35/1.33")),
		Provenance(35, TEXT("cancellation"), 17).Replace(TEXT("execution_base=17/1.16"), TEXT("execution_base=29/1.28")),
		Provenance(35, TEXT("cancellation"), 17).Replace(TEXT("managed.static_storage@1"), TEXT("managed.static_storage@2")),
		Provenance(35, TEXT("cancellation"), 17).Replace(
			TEXT("error.cancellation_token_value@1,managed.static_storage@1"),
			TEXT("managed.static_storage@1,error.cancellation_token_value@1")),
		Provenance(35, TEXT("cancellation"), 17).Replace(TEXT("\nsource_language=csharp"), TEXT("")),
		Provenance(35, TEXT("cancellation"), 17).Replace(TEXT("semantic=54/1.63"), TEXT("semantic=53/1.62")),
		Provenance(35, TEXT("cancellation"), 17) + TEXT("\nunknown=1")})
	{
		TArray<uint8> Mismatched = Module(&ComposableJson, false);
		Custom(Mismatched, "avidscript.provenance", Metadata);
		TestFalse(TEXT("IR 35 rejects wrong versions, capability order and unknown fields"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(Mismatched, ModuleId, Catalog, Error));
	}
	auto ComposableAsyncDocument = Document(35);
	ComposableAsyncDocument->GetArrayField(TEXT("types"))[0]->AsObject()->SetStringField(
		TEXT("type_id"), TEXT("type:global::System.Threading.Tasks.TaskCanceledException"));
	const FString ComposableAsyncJson = Json(ComposableAsyncDocument);
	TestTrue(TEXT("IR 35 async five-capability catalog loads"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&ComposableAsyncJson, true, false, 35, TEXT("cancellation"), 29, true), ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 35 async profile grants fault, cancellation identity and exception token APIs"),
		Catalog && Catalog->SupportsTaskLanguageErrorFault() && Catalog->SupportsTaskCancellationError()
			&& Catalog->SupportsTaskCancellationIdentity() && Catalog->SupportsExceptionCancellationToken()
			&& Catalog->IsCancellationType(1));
	const FString ComposableAsyncPath = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptComposableIr35/GuestFixtures/composable-async-static-token.wasm"));
	TArray<uint8> ComposableAsyncWasm;
	if (TestTrue(TEXT("read same-source IR 35 async WASM fixture"),
			FFileHelper::LoadFileToArray(ComposableAsyncWasm, *ComposableAsyncPath)))
	{
		Catalog.Reset();
		if (TestTrue(TEXT("same-source IR 35 async WASM catalog loads"),
				FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(ComposableAsyncWasm,
					TEXT("csharp:Scripts/ComposableAsyncStaticToken.cs"), Catalog, Error)))
		{
			TestTrue(TEXT("same-source IR 35 async catalog authorizes its fault and cancellation APIs"),
				Catalog && Catalog->SupportsTaskLanguageErrorFault() && Catalog->SupportsTaskCancellationError()
					&& Catalog->SupportsTaskCancellationIdentity() && Catalog->SupportsExceptionCancellationToken());
		}
		else AddError(Error);
	}
	TestFalse(TEXT("IR 35 async profile requires its catalog"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(nullptr, true, false, 35, TEXT("cancellation"), 29, true), ModuleId, Catalog, Error));
	TestFalse(TEXT("IR 35 async profile binds the expected module identity"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&ComposableAsyncJson, true, false, 35, TEXT("cancellation"), 29, true),
			TEXT("different_module"), Catalog, Error));
	for (const FString& Metadata : {
		Provenance(35, TEXT("none"), 29, true),
		Provenance(35, TEXT("cancellation"), 29, true).Replace(TEXT("execution_base=29/1.28"), TEXT("execution_base=17/1.16")),
		Provenance(35, TEXT("cancellation"), 29, true).Replace(TEXT("error.exception_values@1,"), TEXT("")),
		Provenance(35, TEXT("cancellation"), 29, true).Replace(TEXT("async.await_readiness@1,async.cancellation_identity@1"),
			TEXT("async.cancellation_identity@1,async.await_readiness@1")),
		Provenance(35, TEXT("cancellation"), 29, true).Replace(TEXT("semantic=54/1.63"), TEXT("semantic=53/1.62")),
		Provenance(35, TEXT("cancellation"), 29, true).Replace(TEXT("source_language=csharp"), TEXT("source_language=guest-ir")),
		Provenance(35, TEXT("cancellation"), 29, true).Replace(TEXT("task_local_exception_model=cancellation\n"), TEXT("")),
		Provenance(35, TEXT("cancellation"), 29, true) + TEXT("\nunknown=1")})
	{
		TArray<uint8> Mismatched = Module(&ComposableAsyncJson, false);
		Custom(Mismatched, "avidscript.provenance", Metadata);
		TestFalse(TEXT("IR 35 async rejects malformed lifetime, profile, capabilities and source"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(Mismatched, ModuleId, Catalog, Error));
	}
	IdentityDocument->GetArrayField(TEXT("types"))[0]->AsObject()->SetStringField(
		TEXT("type_id"), TEXT("type:global::System.Threading.Tasks.TaskCanceledException"));
	const FString IdentityJson = Json(IdentityDocument);
	for (const int32 Base : {24, 25, 26, 29, 30})
	{
		TestTrue(TEXT("IR 32 supports each frozen cancellation base"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				Module(&IdentityJson, true, false, 32, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
		TestTrue(TEXT("IR 32 authorizes cancellation identity and previous terminal APIs"),
			Catalog && Catalog->SupportsTaskCancellationIdentity() && Catalog->SupportsTaskCancellationError()
			&& Catalog->SupportsTaskLanguageErrorFault() && Catalog->IsCancellationType(1));
		TestFalse(TEXT("IR 32 requires its catalog"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(nullptr, true, false, 32, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
		TestFalse(TEXT("An IR 31 catalog cannot authorize IR 32"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&ReadinessJson, true, false, 32, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
		TestFalse(TEXT("IR 32 catalog cannot hide in IR 31"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&IdentityJson, true, false, 31, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
		if (Base != 24)
			for (const TCHAR* Model : {TEXT("none"), TEXT("fault"), TEXT("exception"), TEXT("cleanup"), TEXT("cleanup_only"), TEXT("unknown")})
				TestFalse(TEXT("IR 32 keeps cancellation lifetime ownership"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
					Module(&IdentityJson, true, false, 32, Model, Base), ModuleId, Catalog, Error));
		for (const FString& Metadata : {
			Provenance(32, TEXT("cancellation"), Base).Replace(TEXT("guest_ir=32/1.31"), TEXT("guest_ir=32/1.30")),
			Provenance(32, TEXT("cancellation"), Base).Replace(*FString::Printf(TEXT("guest_ir_base=%d/1.%d"), Base, Base - 1),
				*FString::Printf(TEXT("guest_ir_base=%d/1.%d"), Base, Base - 2))})
		{
			TArray<uint8> Mismatched = Module(&IdentityJson, false);
			Custom(Mismatched, "avidscript.provenance", Metadata);
			TestFalse(TEXT("IR 32 rejects mismatched version pairs"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				Mismatched, ModuleId, Catalog, Error));
		}
	}
	for (const int32 Base : {0, 17, 20, 23, 27, 28, 31, 32, 33})
		TestFalse(TEXT("IR 32 rejects unsupported or missing base"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&IdentityJson, true, false, 32, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
	IdentityDocument->SetArrayField(TEXT("types"), {});
	IdentityDocument->SetArrayField(TEXT("sources"), {});
	const FString EmptyIdentityJson = Json(IdentityDocument);
	TestFalse(TEXT("IR 32 requires a nonempty cancellation catalog"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
		Module(&EmptyIdentityJson, true, false, 32, TEXT("cancellation"), 24), ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 30 static/async composition requires IR 29 base"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&StaticAsyncJson, true, false, 30, TEXT("cancellation"), 29), ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 30 preserves fault and cancellation capabilities"),
		Catalog && Catalog->SupportsTaskLanguageErrorFault() && Catalog->SupportsTaskCancellationError());
	TestTrue(TEXT("IR 29 source exceptions use the exact routed cancellation profile"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&SynchronousAsyncJson, true, false, 29, TEXT("cancellation"), 26), ModuleId, Catalog, Error));
	TestTrue(TEXT("IR 29 preserves fault and cancellation capabilities"),
		Catalog && Catalog->SupportsTaskLanguageErrorFault() && Catalog->SupportsTaskCancellationError());
	auto SynchronousAsyncEmpty = Document(29);
	SynchronousAsyncEmpty->SetArrayField(TEXT("types"), {});
	SynchronousAsyncEmpty->SetArrayField(TEXT("sources"), {});
	const FString SynchronousAsyncEmptyJson = Json(SynchronousAsyncEmpty);
	TestTrue(TEXT("IR 29 accepts a validated source profile without throw sites"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
			Module(&SynchronousAsyncEmptyJson, true, false, 29, TEXT("cancellation"), 26), ModuleId, Catalog, Error));
	for (const int32 Base : {20, 21, 22, 23, 24, 25, 26})
	{
		TestTrue(TEXT("IR 28 retains the selected Task error execution profile"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				Module(&TransferJson, true, false, 28, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
		TestTrue(TEXT("Checked transfer profile authorizes Task faults"), Catalog && Catalog->SupportsTaskLanguageErrorFault());
		TestEqual(TEXT("Transfer cannot grant cancellation to older profiles"),
			Catalog && Catalog->SupportsTaskCancellationError(), Base >= 24);
	}
	for (const int32 Base : {17, 20, 21, 22, 23, 24, 25, 26})
	{
		TestTrue(TEXT("IR 27 retains each existing error execution profile"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				Module(&StaticJson, true, false, 27, TEXT("cancellation"), Base), ModuleId, Catalog, Error));
		TestEqual(TEXT("Static storage cannot grant Task faults to IR 17"),
			Catalog && Catalog->SupportsTaskLanguageErrorFault(), Base >= 20);
		TestEqual(TEXT("Static storage cannot grant cancellation to older profiles"),
			Catalog && Catalog->SupportsTaskCancellationError(), Base >= 24);
	}
	for (const int32 Base : {4, 14, 18, 19, 25})
	{
		TestTrue(TEXT("Static storage retains no-error profiles without a catalog"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				Module(nullptr, true, false, 27, TEXT("none"), Base), ModuleId, Catalog, Error));
		TestNull(TEXT("No-error static profile has no Task error capability"), Catalog.Get());
	}
	auto StaticEmpty = Document(27);
	StaticEmpty->SetArrayField(TEXT("types"), {});
	StaticEmpty->SetArrayField(TEXT("sources"), {});
	const FString StaticEmptyJson = Json(StaticEmpty);
	for (const int32 Base : {23, 25})
		TestTrue(TEXT("Static storage retains cleanup-only empty catalogs"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				Module(&StaticEmptyJson, true, false, 27, TEXT("cleanup"), Base), ModuleId, Catalog, Error));

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
	Invalid.Emplace(TEXT("IR 31 requires a base execution profile"), Module(&ReadinessJson, true, false, 31));
	for (const int32 Base : {4, 17, 20, 21, 22, 23, 27, 28, 31, 32})
		Invalid.Emplace(TEXT("IR 31 rejects unsupported base profiles"), Module(&ReadinessJson, true, false, 31, TEXT("cancellation"), Base));
	auto ReadinessEmpty = Document(31);
	ReadinessEmpty->SetArrayField(TEXT("types"), {});
	ReadinessEmpty->SetArrayField(TEXT("sources"), {});
	const FString ReadinessEmptyJson = Json(ReadinessEmpty);
	for (const int32 Base : {24, 25, 26, 29, 30})
	{
		Invalid.Emplace(TEXT("IR 31 requires its catalog"), Module(nullptr, true, false, 31, TEXT("cancellation"), Base));
		Invalid.Emplace(TEXT("IR 31 rejects empty cancellation catalogs"), Module(&ReadinessEmptyJson, true, false, 31, TEXT("cancellation"), Base));
		const FString BaseJson = Json(Document(Base));
		Invalid.Emplace(TEXT("IR 31 cannot use its base catalog identity"), Module(&BaseJson, true, false, 31, TEXT("cancellation"), Base));
		const int32 InnerBase = Base == 30 ? 29 : Base == 29 ? 26 : 0;
		Invalid.Emplace(TEXT("IR 31 catalog cannot be downgraded"), Module(&ReadinessJson, true, false, Base, TEXT("cancellation"), InnerBase));
		if (Base != 24)
			for (const TCHAR* Model : {TEXT("none"), TEXT("fault"), TEXT("exception"), TEXT("cleanup"), TEXT("cleanup_only"), TEXT("unknown")})
				Invalid.Emplace(TEXT("IR 31 retains cancellation ownership"), Module(&ReadinessJson, true, false, 31, Model, Base));
		const FString Metadata = Provenance(31, TEXT("cancellation"), Base);
		for (const FString& MismatchedMetadata : {
			Metadata.Replace(TEXT("guest_ir=31/1.30"), TEXT("guest_ir=31/1.29")),
			Metadata.Replace(*FString::Printf(TEXT("guest_ir_base=%d/1.%d"), Base, Base - 1),
				*FString::Printf(TEXT("guest_ir_base=%d/1.%d"), Base, Base - 2))})
		{
			TArray<uint8> Mismatched = Module(&ReadinessJson, false);
			Custom(Mismatched, "avidscript.provenance", MismatchedMetadata);
			Invalid.Emplace(TEXT("IR 31 requires exact outer and base versions"), MoveTemp(Mismatched));
		}
	}
	Invalid.Emplace(TEXT("IR 30 requires its execution profile"), Module(&StaticAsyncJson, true, false, 30));
	Invalid.Emplace(TEXT("IR 30 requires its catalog"), Module(nullptr, true, false, 30, TEXT("cancellation"), 29));
	Invalid.Emplace(TEXT("IR 30 cannot load an older catalog"), Module(&SynchronousAsyncJson, true, false, 30, TEXT("cancellation"), 29));
	Invalid.Emplace(TEXT("IR 30 catalog cannot be downgraded to IR 29"), Module(&StaticAsyncJson, true, false, 29, TEXT("cancellation"), 26));
	for (const int32 Base : {17, 20, 24, 25, 26, 27, 28, 30, 31})
		Invalid.Emplace(TEXT("IR 30 base must be exactly IR 29"), Module(&StaticAsyncJson, true, false, 30, TEXT("cancellation"), Base));
	for (const TCHAR* Model : {TEXT("none"), TEXT("fault"), TEXT("exception"), TEXT("cleanup"), TEXT("cleanup_only"), TEXT("unknown")})
		Invalid.Emplace(TEXT("IR 30 requires cancellation ownership"), Module(&StaticAsyncJson, true, false, 30, Model, 29));
	auto StaticAsyncEmpty = Document(30);
	StaticAsyncEmpty->SetArrayField(TEXT("types"), {});
	StaticAsyncEmpty->SetArrayField(TEXT("sources"), {});
	const FString StaticAsyncEmptyJson = Json(StaticAsyncEmpty);
	Invalid.Emplace(TEXT("IR 30 initialization requires a nonempty error catalog"), Module(&StaticAsyncEmptyJson, true, false, 30, TEXT("cancellation"), 29));
	for (const FString& Metadata : {Provenance(30, TEXT("cancellation"), 29).Replace(TEXT("guest_ir_base=29/1.28"), TEXT("guest_ir_base=29/1.27")),
		Provenance(30, TEXT("cancellation"), 29).Replace(TEXT("guest_ir=30/1.29"), TEXT("guest_ir=30/1.28"))})
	{
		TArray<uint8> Mismatched = Module(&StaticAsyncJson, false);
		Custom(Mismatched, "avidscript.provenance", Metadata);
		Invalid.Emplace(TEXT("IR 30 requires exact outer and base version pairs"), MoveTemp(Mismatched));
	}
	Invalid.Emplace(TEXT("IR 29 requires its execution profile"), Module(&SynchronousAsyncJson, true, false, 29));
	Invalid.Emplace(TEXT("IR 29 requires its catalog"), Module(nullptr, true, false, 29, TEXT("cancellation"), 26));
	Invalid.Emplace(TEXT("IR 29 cannot load an older catalog"), Module(&RoutedThrowJson, true, false, 29, TEXT("cancellation"), 26));
	Invalid.Emplace(TEXT("IR 29 cannot disguise a new catalog as IR 26"), Module(&SynchronousAsyncJson, true, false, 26));
	for (const int32 Base : {17, 20, 21, 22, 23, 24, 25, 27, 28, 29, 30})
		Invalid.Emplace(TEXT("IR 29 base must be exactly IR 26"), Module(&SynchronousAsyncJson, true, false, 29, TEXT("cancellation"), Base));
	for (const TCHAR* Model : {TEXT("none"), TEXT("fault"), TEXT("exception"), TEXT("cleanup"), TEXT("cleanup_only"), TEXT("unknown")})
		Invalid.Emplace(TEXT("IR 29 requires cancellation ownership"), Module(&SynchronousAsyncJson, true, false, 29, Model, 26));
	for (const FString& Metadata : {Provenance(29, TEXT("cancellation"), 26).Replace(TEXT("guest_ir_base=26/1.25"), TEXT("guest_ir_base=26/1.24")),
		Provenance(29, TEXT("cancellation"), 26).Replace(TEXT("guest_ir=29/1.28"), TEXT("guest_ir=29/1.27"))})
	{
		TArray<uint8> MismatchedSource = Module(&SynchronousAsyncJson, false);
		Custom(MismatchedSource, "avidscript.provenance", Metadata);
		Invalid.Emplace(TEXT("IR 29 outer and base version pairs must be exact"), MoveTemp(MismatchedSource));
	}
	Invalid.Emplace(TEXT("transfer profile requires base metadata"), Module(&TransferJson, true, false, 28));
	for (const int32 Base : {19, 27, 28, 29})
		Invalid.Emplace(TEXT("transfer base must be an existing Task error profile"), Module(&TransferJson, true, false, 28, TEXT("cancellation"), Base));
	Invalid.Emplace(TEXT("transfer requires its catalog"), Module(nullptr, true, false, 28, TEXT("cancellation"), 24));
	Invalid.Emplace(TEXT("no-error base cannot hide a missing transfer catalog"), Module(nullptr, true, false, 28, TEXT("none"), 25));
	Invalid.Emplace(TEXT("transfer cannot use an older catalog identity"), Module(&CancellationJson, true, false, 28, TEXT("cancellation"), 24));
	Invalid.Emplace(TEXT("transfer retains routed throw ownership restrictions"), Module(&TransferJson, true, false, 28, TEXT("fault"), 26));
	auto TransferEmpty = Document(28);
	TransferEmpty->SetArrayField(TEXT("types"), {});
	TransferEmpty->SetArrayField(TEXT("sources"), {});
	const FString TransferEmptyJson = Json(TransferEmpty);
	Invalid.Emplace(TEXT("transfer requires nonempty catalog even with cleanup profile"), Module(&TransferEmptyJson, true, false, 28, TEXT("cleanup"), 23));
	for (const FString& Metadata : {Provenance(28) + TEXT("\nguest_ir_base=24/1.22"),
		Provenance(28, TEXT("cancellation"), 24).Replace(TEXT("guest_ir=28/1.27"), TEXT("guest_ir=28/1.26"))})
	{
		TArray<uint8> MismatchedTransfer = Module(&TransferJson, false);
		Custom(MismatchedTransfer, "avidscript.provenance", Metadata);
		Invalid.Emplace(TEXT("transfer outer and base version pairs must be exact"), MoveTemp(MismatchedTransfer));
	}
	Invalid.Emplace(TEXT("static profile requires base metadata"), Module(&StaticJson, true, false, 27));
	Invalid.Emplace(TEXT("static base cannot be self-referential"), Module(&StaticJson, true, false, 27, TEXT("cancellation"), 27));
	Invalid.Emplace(TEXT("static base cannot be a future version"), Module(&StaticJson, true, false, 27, TEXT("cancellation"), 28));
	Invalid.Emplace(TEXT("static base cannot predate managed heap"), Module(&StaticJson, true, false, 27, TEXT("cancellation"), 3));
	Invalid.Emplace(TEXT("old artifact cannot carry static base metadata"), Module(&CancellationJson, true, false, 24, TEXT("cancellation"), 24));
	Invalid.Emplace(TEXT("static base cannot bypass required catalog"), Module(nullptr, true, false, 27, TEXT("cancellation"), 24));
	Invalid.Emplace(TEXT("static catalog must retain outer artifact identity"), Module(&CancellationJson, true, false, 27, TEXT("cancellation"), 24));
	Invalid.Emplace(TEXT("static catalog cannot authorize downgraded execution profile"), Module(&StaticJson, true, false, 27, TEXT("cancellation"), 14));
	Invalid.Emplace(TEXT("static profile retains routed throw ownership restrictions"), Module(&StaticJson, true, false, 27, TEXT("fault"), 26));
	Invalid.Emplace(TEXT("static profile retains nonempty catalog restrictions"), Module(&StaticEmptyJson, true, false, 27, TEXT("cancellation"), 24));
	TArray<uint8> MismatchedStaticBase = Module(&StaticJson, false);
	Custom(MismatchedStaticBase, "avidscript.provenance", Provenance(27) + TEXT("\nguest_ir_base=24/1.22"));
	Invalid.Emplace(TEXT("static base version pair must match"), MoveTemp(MismatchedStaticBase));
	Invalid.Emplace(TEXT("IR 26 requires its catalog"), Module(nullptr, true, false, 26));
	Invalid.Emplace(TEXT("IR 26 catalog cannot use IR 25 provenance"), Module(&RoutedThrowJson, true, false, 25));
	Invalid.Emplace(TEXT("IR 25 catalog cannot use IR 26 provenance"), Module(&LifetimeJson, true, false, 26));
	for (const TCHAR* Model : {TEXT("none"), TEXT("fault"), TEXT("exception"), TEXT("cleanup"), TEXT("cleanup_only"), TEXT("unknown")})
		Invalid.Emplace(TEXT("IR 26 requires cancellation ownership"), Module(&RoutedThrowJson, true, false, 26, Model));
	Invalid.Emplace(TEXT("IR 25 fault profile requires its catalog"), Module(nullptr, true, false, 25, TEXT("fault")));
	Invalid.Emplace(TEXT("IR 25 unknown profile rejected"), Module(&LifetimeJson, true, false, 25, TEXT("unknown")));
	Invalid.Emplace(TEXT("IR 25 none profile rejects an error catalog"), Module(&LifetimeJson, true, false, 25, TEXT("none")));
	Invalid.Emplace(TEXT("IR 25 catalog cannot use IR 24 provenance"), Module(&LifetimeJson, true, false, 24));
	Invalid.Emplace(TEXT("IR 24 catalog cannot use IR 25 provenance"), Module(&CancellationJson, true, false, 25));
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
	Invalid.Emplace(TEXT("missing IR 24 catalog"), Module(nullptr, true, false, 24));
	Invalid.Emplace(TEXT("IR 24 catalog cannot use IR 23 provenance"),
		Module(&CancellationJson, true, false, 23));
	Invalid.Emplace(TEXT("IR 23 catalog cannot use IR 24 provenance"),
		Module(&DirectEmptyJson, true, false, 24));
	auto FutureGuest = Document(25);
	FutureGuest->SetNumberField(TEXT("guest_ir_schema_version"), 27);
	FutureGuest->SetStringField(TEXT("guest_ir_version"), TEXT("1.26"));
	const FString FutureGuestJson = Json(FutureGuest);
	Invalid.Emplace(TEXT("unknown Guest version remains rejected"),
		Module(&FutureGuestJson, true, false, 24));
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
	for (const int32 GuestSchema : {20, 21, 22, 23, 24, 25, 26, 17})
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
			const bool bAllowedVersion = GuestSchema >= 20 && GuestSchema <= 26;
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
		for (const int32 GuestSchema : {20, 21, 22, 23, 24, 25, 26, 17})
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
					Result.ErrorCategory, GuestSchema >= 20 && GuestSchema <= 26
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTaskCancellationImportVersionTest,
	"AvidScript.Runtime.Continuation.TaskCancellationImportVersion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskCancellationImportVersionTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptLanguageErrorCatalogTests;
	using namespace AvidScript::TaskResult::Abi;
	for (const ANSICHAR* ImportName : {
		CancelLanguageErrorImport, TerminalErrorMetaImport, TerminalErrorRootImport})
	{
		for (const int32 GuestSchema : {17, 20, 21, 22, 23, 24, 25, 26})
		{
			const TArray<uint8> Wasm = FCStringAnsi::Strcmp(ImportName, CancelLanguageErrorImport) == 0
				? TaskFaultImportModule(GuestSchema, ImportName)
				: TaskReadImportModule(GuestSchema, ImportName);
			for (const auto& Lane : GetAvidScriptRuntimeBackendTestLanes())
			{
				FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
				FAvidScriptWasmSmokeResult Result;
				if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("cancellation import links")),
					Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), ModuleId, Result)))
				{
					AddError(Result.ErrorMessage);
					return false;
				}
				TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
				TestFalse(TEXT("Cancellation import rejects invalid version or missing context"),
					Runtime.BeginPlay(Result));
				TestEqual(TEXT("Only cancellation-enabled IR reaches context validation"), Result.ErrorCategory,
					(GuestSchema == 24 || GuestSchema == 25 || GuestSchema == 26) ? FString(TEXT("task_result_context"))
						: FString(TEXT("task_language_error_version")));
				TestEqual(TEXT("VM reports the exact cancellation import"), Result.ImportName,
					FString(UTF8_TO_TCHAR(ImportName)));
				const auto* Heap = Runtime.GetManagedHeapForTesting();
				TestTrue(TEXT("Rejected cancellation leaves no invocation roots"), Heap
					&& Heap->GetStats().LiveRoots == 0 && Heap->GetStats().ActiveFrames == 0);
				Runtime.Unload();
			}
		}
	}
	return true;
}

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptAsyncVoidCatalogTest,
	"AvidScript.Runtime.LanguageErrorCatalog.AsyncVoidAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptAsyncVoidCatalogTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptLanguageErrorCatalogTests;
	const FString Metadata = Json(Document(36));
	TUniquePtr<FAvidScriptLanguageErrorCatalog> Catalog;
	FString Error;
	TestTrue(TEXT("IR36 owner-only catalog admits the exact source contract"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(VoidOwnerModule(&Metadata), ModuleId, Catalog, Error));
	TestTrue(TEXT("Owner-only contract grants fault and lifetime cancellation, without optional APIs"),
		Catalog && Catalog->SupportsAsyncVoidErrorOwner() && Catalog->SupportsTaskLanguageErrorFault()
			&& Catalog->SupportsTaskCancellationError() && !Catalog->SupportsTaskCancellationIdentity()
			&& !Catalog->SupportsExceptionCancellationToken());
	for (const TCHAR* Capabilities : {TEXT("async.cancellation_identity@1,error.async_void_owner@1"),
		TEXT("error.async_void_owner@1,error.cancellation_token_value@1"),
		TEXT("async.await_readiness@1,async.cancellation_identity@1,error.async_void_owner@1,error.exception_values@1")})
	{
		if (!TestTrue(TEXT("IR36 actual optional capabilities admit independently"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				VoidOwnerModule(&Metadata, VoidOwnerProvenance(Capabilities)), ModuleId, Catalog, Error))) return false;
		const FString Declared(Capabilities);
		TestEqual(TEXT("Identity API requires its actual declaration"), Catalog->SupportsTaskCancellationIdentity(),
			Declared.Contains(TEXT("async.cancellation_identity@1")));
		TestEqual(TEXT("Token API requires its actual declaration"), Catalog->SupportsExceptionCancellationToken(),
			Declared.Contains(TEXT("error.cancellation_token_value@1")));
	}
	TestFalse(TEXT("IR36 requires its catalog"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
		VoidOwnerModule(nullptr), ModuleId, Catalog, Error));
	TestFalse(TEXT("IR36 binds the loaded module identity"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
		VoidOwnerModule(&Metadata), TEXT("other_module"), Catalog, Error));
	const FString LegacyMetadata = Json(Document(35));
	TestFalse(TEXT("IR35 catalog cannot authorize IR36 bytes"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
		VoidOwnerModule(&LegacyMetadata), ModuleId, Catalog, Error));
	for (const FString& Invalid : {
		VoidOwnerProvenance(TEXT("")), VoidOwnerProvenance(TEXT("async.await_readiness@1")),
		VoidOwnerProvenance(TEXT("error.async_void_owner@2")),
		VoidOwnerProvenance(TEXT("error.async_void_owner@1,error.async_void_owner@1")),
		VoidOwnerProvenance(TEXT("managed.static_storage@1,error.async_void_owner@1")),
		VoidOwnerProvenance(TEXT("error.async_void_owner@1,")),
		VoidOwnerProvenance(TEXT("error.async_void_owner@1,error.exception_values@1")),
		VoidOwnerProvenance().Replace(TEXT("guest_ir=36/1.35"), TEXT("guest_ir=36/1.36")),
		VoidOwnerProvenance().Replace(TEXT("semantic=55/1.64"), TEXT("semantic=54/1.63")),
		VoidOwnerProvenance().Replace(TEXT("execution_base=29/1.28"), TEXT("execution_base=26/1.25")),
		VoidOwnerProvenance().Replace(TEXT("task_local_exception_model=cancellation"), TEXT("task_local_exception_model=none")),
		VoidOwnerProvenance().Replace(TEXT("source_language=csharp"), TEXT("source_language=guest-ir")),
		VoidOwnerProvenance().Replace(*(TEXT("source_sha256=") + SourceSha256), TEXT("source_sha256=bad")),
		VoidOwnerProvenance() + TEXT("\nunknown=1")})
	{
		TestFalse(TEXT("IR36 rejects malformed capability, version, provenance and ownership contracts"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(VoidOwnerModule(&Metadata, Invalid), ModuleId, Catalog, Error));
		TestFalse(TEXT("Rejected contract publishes no catalog"), Catalog != nullptr);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptAsyncVoidCompositionCatalogTest,
	"AvidScript.Runtime.LanguageErrorCatalog.AsyncVoidCompositionAdmission",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptAsyncVoidCompositionCatalogTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptLanguageErrorCatalogTests;
	const FString Metadata = Json(Document(37));
	TUniquePtr<FAvidScriptLanguageErrorCatalog> Catalog;
	FString Error;
	for (const TCHAR* Capabilities : {
		TEXT("error.async_void_owner@1,managed.static_storage@1"),
		TEXT("error.async_void_owner@1,error.cancellation_token_value@1"),
		TEXT("async.await_readiness@1,async.cancellation_identity@1,error.async_void_owner@1,error.cancellation_token_value@1,error.exception_values@1,managed.static_storage@1")})
	{
		if (!TestTrue(TEXT("IR37 admits exact static, token and full composition contracts"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
				VoidOwnerModule(&Metadata, VoidCompositionProvenance(Capabilities)), ModuleId, Catalog, Error))) return false;
		const FString Declared(Capabilities);
		TestTrue(TEXT("Composition retains checked owner and Task cancellation"),
			Catalog->SupportsAsyncVoidErrorOwner() && Catalog->SupportsTaskCancellationError());
		TestEqual(TEXT("IR37 identity requires actual declaration"), Catalog->SupportsTaskCancellationIdentity(),
			Declared.Contains(TEXT("async.cancellation_identity@1")));
		TestEqual(TEXT("IR37 token requires actual declaration"), Catalog->SupportsExceptionCancellationToken(),
			Declared.Contains(TEXT("error.cancellation_token_value@1")));
	}
	for (const FString& Invalid : {
		VoidCompositionProvenance(TEXT("error.async_void_owner@1")),
		VoidCompositionProvenance(TEXT("managed.static_storage@1")),
		VoidCompositionProvenance(TEXT("error.async_void_owner@1,managed.static_storage@2")),
		VoidCompositionProvenance(TEXT("error.async_void_owner@1,error.async_void_owner@1,managed.static_storage@1")),
		VoidCompositionProvenance().Replace(TEXT("guest_ir=37/1.36"), TEXT("guest_ir=37/1.35")),
		VoidCompositionProvenance().Replace(TEXT("semantic=56/1.65"), TEXT("semantic=55/1.64")),
		VoidCompositionProvenance().Replace(TEXT("\nsource_execution=55/1.64"), TEXT("")),
		VoidCompositionProvenance().Replace(TEXT("source_execution=55/1.64"), TEXT("source_execution=50/1.59")),
		VoidCompositionProvenance().Replace(TEXT("execution_base=29/1.28"), TEXT("execution_base=30/1.29")),
		VoidCompositionProvenance() + TEXT("\nunknown=1")})
	{
		TestFalse(TEXT("IR37 rejects malformed composition identities and capabilities"),
			FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(VoidOwnerModule(&Metadata, Invalid), ModuleId, Catalog, Error));
		TestFalse(TEXT("Rejected IR37 publishes no catalog"), Catalog != nullptr);
	}
	const FString OldMetadata = Json(Document(36));
	const FString Downgrade = VoidCompositionProvenance()
		.Replace(TEXT("guest_ir=37/1.36"), TEXT("guest_ir=36/1.35"))
		.Replace(TEXT("semantic=56/1.65"), TEXT("semantic=55/1.64"));
	TestFalse(TEXT("Version-only downgrade retaining composition marker is rejected by IR36"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(VoidOwnerModule(&OldMetadata, Downgrade), ModuleId, Catalog, Error));
	TestFalse(TEXT("IR36 catalog cannot authorize IR37 execution"),
		FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(VoidOwnerModule(&OldMetadata, VoidCompositionProvenance()), ModuleId, Catalog, Error));
	TestFalse(TEXT("IR37 binds loaded module identity"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
		VoidOwnerModule(&Metadata, VoidCompositionProvenance()), TEXT("other_module"), Catalog, Error));
	TestFalse(TEXT("IR37 requires catalog"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
		VoidOwnerModule(nullptr, VoidCompositionProvenance()), ModuleId, Catalog, Error));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptStaticAsyncValueCatalogTest,
    "AvidScript.Runtime.LanguageErrorCatalog.StaticAsyncValueAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptStaticAsyncValueCatalogTest::RunTest(const FString& Parameters)
{
    using namespace AvidScriptLanguageErrorCatalogTests;
    const FString Metadata = Json(Document(38));
    TUniquePtr<FAvidScriptLanguageErrorCatalog> Catalog;
    FString Error;
    auto Provenance = [](const TCHAR* Base, const TCHAR* Capabilities) {
        return VoidOwnerProvenance(Capabilities).Replace(TEXT("guest_ir=36/1.35"), TEXT("guest_ir=38/1.37"))
            .Replace(TEXT("semantic=55/1.64"), TEXT("semantic=57/1.66")) + TEXT("\nsource_execution=") + Base;
    };
    const FString Named = Provenance(TEXT("52/1.61"), TEXT("async.cancellation_identity@1,error.exception_values@1,managed.static_storage@1"));
    const FString Token = Provenance(TEXT("53/1.62"), TEXT("async.await_readiness@1,async.cancellation_identity@1,error.cancellation_token_value@1,managed.static_storage@1"));
    for (const FString& Valid : {Named, Token})
    {
        if (!TestTrue(TEXT("IR38 admits only actual static async value capabilities"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
            VoidOwnerModule(&Metadata, Valid), ModuleId, Catalog, Error))) return false;
        TestTrue(TEXT("IR38 preserves Task cancellation identity"), Catalog->SupportsTaskCancellationIdentity());
        TestFalse(TEXT("IR38 cannot report async void"), Catalog->SupportsAsyncVoidErrorOwner());
        TestEqual(TEXT("Token import authority follows actual declaration"), Catalog->SupportsExceptionCancellationToken(), Valid == Token);
    }
    for (const FString& Invalid : {
        Named.Replace(TEXT("guest_ir=38/1.37"), TEXT("guest_ir=38/1.36")),
        Named.Replace(TEXT("semantic=57/1.66"), TEXT("semantic=56/1.65")),
        Named.Replace(TEXT("\nsource_execution=52/1.61"), TEXT("")),
        Named.Replace(TEXT("source_execution=52/1.61"), TEXT("source_execution=53/1.62")),
        Token.Replace(TEXT("source_execution=53/1.62"), TEXT("source_execution=52/1.61")),
        Named.Replace(TEXT("execution_base=29/1.28"), TEXT("execution_base=30/1.29")),
        Named.Replace(TEXT("error.exception_values@1,"), TEXT("")),
        Token.Replace(TEXT("error.cancellation_token_value@1,"), TEXT("")),
        Named.Replace(TEXT("managed.static_storage@1"), TEXT("managed.static_storage@2")),
        Named.Replace(TEXT("error.exception_values@1"), TEXT("error.async_void_owner@1")),
        Named.Replace(TEXT("async.cancellation_identity@1"), TEXT("async.cancellation_identity@1,async.cancellation_identity@1")),
        Named + TEXT("\nunknown=1") })
    {
        TestFalse(TEXT("IR38 rejects incomplete, forged and mismatched composition contracts"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
            VoidOwnerModule(&Metadata, Invalid), ModuleId, Catalog, Error));
        TestFalse(TEXT("Rejected IR38 publishes no catalog"), Catalog != nullptr);
    }
    const FString OldMetadata = Json(Document(35));
    TestFalse(TEXT("Older envelope cannot retain an IR38 source marker"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
        VoidOwnerModule(&OldMetadata, Named.Replace(TEXT("guest_ir=38/1.37"), TEXT("guest_ir=35/1.34"))), ModuleId, Catalog, Error));
    TestFalse(TEXT("IR38 binds the loaded module identity"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
        VoidOwnerModule(&Metadata, Named), TEXT("other_module"), Catalog, Error));
    TestFalse(TEXT("IR38 requires its error catalog"), FAvidScriptLanguageErrorCatalog::ReadFromCanonicalWasm(
        VoidOwnerModule(nullptr, Named), ModuleId, Catalog, Error));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptAsyncVoidReportTest,
	"AvidScript.Runtime.LanguageErrorCatalog.AsyncVoidCheckedReport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptAsyncVoidReportTest::RunTest(const FString& Parameters)
{
	using namespace AvidScriptLanguageErrorCatalogTests;
	using namespace AvidScript::Managed;
	for (const auto& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		const FString Metadata = Json(Document(36));
		const auto Wasm = VoidOwnerModule(&Metadata);
		if (!TestTrue(TEXT("Checked-report module loads"), Runtime.LoadModule(Wasm.GetData(), Wasm.Num(), ModuleId, Result)))
		{ AddError(Result.ErrorMessage); return false; }
		auto* Heap = Runtime.GetManagedHeapForTesting();
		const std::array<FHeapLayout, 1> Layouts{{{1, 8, {}}}};
		if (!TestTrue(TEXT("Report heap configures"), Heap && Heap->Configure(Layouts) == EHeapError::Ok)) return false;
		auto Report = [&](FToken Object, int32 Type = 1, int32 Source = 1) {
			FAvidScriptHostCall Call;
			Call.BindingId = EAvidScriptHostBindingId::LanguageErrorReportV1;
			Call.IntArgs[0] = Type; Call.IntArgs[1] = Source; Call.Int64Args[0] = static_cast<int64>(Object);
			FAvidScriptHostCallResult CallResult;
			(void)Runtime.DispatchHostCall(Call, CallResult);
			return CallResult;
		};
		auto Allocate = [&]() -> FToken {
			FToken Frame = 0, Root = 0, Object = 0;
			TestEqual(TEXT("Report frame starts"), Heap->PushFrame(Frame), EHeapError::Ok);
			TestEqual(TEXT("Report root starts"), Heap->CreateRoot(Frame, 0, Root), EHeapError::Ok);
			TestEqual(TEXT("Report object allocates"), Heap->Allocate(1, Root, Object), EHeapError::Ok);
			return Object;
		};
		const uint64 Invocation = Runtime.BeginVmInvocation();
		const auto Object = Allocate();
		TestEqual(TEXT("An arbitrary VM entry cannot defer an error without a typed callback"),
			Report(Object).ErrorCategory, FString(TEXT("language_error_report_context")));
		Runtime.BeginTypedCallbackEpochForTesting();
		TestEqual(TEXT("Unknown type report fails immediately"), Report(Object, 2).ErrorCategory,
			FString(TEXT("language_error_invalid_report")));
		TestEqual(TEXT("Unknown source report fails immediately"), Report(Object, 1, 2).ErrorCategory,
			FString(TEXT("language_error_invalid_report")));
		TestEqual(TEXT("An unrooted object fails immediately"), Report(0).ErrorCategory,
			FString(TEXT("language_error_invalid_report")));
		const auto Accepted = Report(Object);
		TestTrue(TEXT("Valid report accepts exactly once before Guest cleanup"), Accepted.bSucceeded && Accepted.ReturnValue == 1);
		Runtime.EndVmInvocation(Invocation);
		TestEqual(TEXT("Diagnostic holds no managed roots"), Heap->GetStats().LiveRoots, 0u);
		TestEqual(TEXT("Diagnostic holds no managed frames"), Heap->GetStats().ActiveFrames, 0u);
		TestEqual(TEXT("Reported object can collect before diagnostic delivery"), Heap->Collect(), EHeapError::Ok);
		TestEqual(TEXT("Reported object is not retained by the callback"), Heap->GetStats().LiveObjects, 0u);
		FAvidScriptVmError Error;
		TestFalse(TEXT("Accepted error makes the complete callback fail"), Runtime.EndTypedCallbackEpochForTesting(Error));
		TestEqual(TEXT("Uncaught category survives delayed delivery"), Error.Category, FString(TEXT("language_error_uncaught")));
		TestTrue(TEXT("Delayed diagnostic retains type and source span"), Error.Details.Contains(TEXT("System.Exception"))
			&& Error.Details.Contains(TEXT("Scripts/SourceThrow.cs:1:5")));
		TestEqual(TEXT("Delayed diagnostic names the report import"), Error.ImportName, FString(TEXT("avid_language_error_report_v1")));
		Runtime.BeginTypedCallbackEpochForTesting();
		Error.Reset();
		TestTrue(TEXT("Following callback cannot inherit a stale report"), Runtime.EndTypedCallbackEpochForTesting(Error));
		TestTrue(TEXT("Following callback diagnostic is empty"), Error.Category.IsEmpty());

		const uint64 DuplicateInvocation = Runtime.BeginVmInvocation();
		const auto DuplicateObject = Allocate();
		Runtime.BeginTypedCallbackEpochForTesting();
		TestTrue(TEXT("First duplicate probe report accepts"), Report(DuplicateObject).bSucceeded);
		TestEqual(TEXT("Second report is a hard Host failure"), Report(DuplicateObject).ErrorCategory,
			FString(TEXT("language_error_duplicate_report")));
		Runtime.EndVmInvocation(DuplicateInvocation);
		Error.Category = TEXT("language_error_duplicate_report");
		TestFalse(TEXT("Duplicate probe callback cannot commit"), Runtime.EndTypedCallbackEpochForTesting(Error));
		TestEqual(TEXT("A later hard failure is not masked by the accepted report"), Error.Category,
			FString(TEXT("language_error_duplicate_report")));
		TestEqual(TEXT("Duplicate unwind collects"), Heap->Collect(), EHeapError::Ok);
		Runtime.Unload();
	}
	return true;
}

#endif
