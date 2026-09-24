#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptLanguageErrorCatalog.h"
#include "AvidScriptRuntimeBackendTestLanes.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "Memory/AvidScriptManagedHeap.h"

#include "Containers/StringConv.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

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

FString Provenance()
{
	return FString::Printf(TEXT("module_id=%s\nsource_id=Scripts/SourceThrow.cs\nsource_sha256=%s\n")
		TEXT("frontend_sha256=%s\nsemantic_sha256=%s\nguest_ir=17/1.16"),
		*ModuleId, *SourceSha256, *FString::ChrN(64, 'b'), *FString::ChrN(64, 'c'));
}

TSharedRef<FJsonObject> Document()
{
	auto Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetNumberField(TEXT("guest_ir_schema_version"), 17);
	Root->SetStringField(TEXT("guest_ir_version"), TEXT("1.16"));
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

TArray<uint8> Module(const FString* Metadata, bool bProvenance = true, bool bDuplicate = false)
{
	TArray<uint8> Wasm;
	Wasm.Append(BaseWasm, UE_ARRAY_COUNT(BaseWasm));
	if (bProvenance) Custom(Wasm, "avidscript.provenance", Provenance());
	if (Metadata)
	{
		Custom(Wasm, "avidscript.language_errors", *Metadata);
		if (bDuplicate) Custom(Wasm, "avidscript.language_errors", *Metadata);
	}
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
		TEXT("catch-variable.wasm")};
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

#endif
