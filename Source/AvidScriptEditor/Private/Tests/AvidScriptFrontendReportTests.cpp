#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptFrontendReport.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString GetAvidScriptReportTestRoot()
{
	FString TestRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptEditorTests"), TEXT("Reports"));
	TestRoot = FPaths::ConvertRelativePathToFull(TestRoot);
	FPaths::NormalizeFilename(TestRoot);
	return TestRoot;
}

FString GetAvidScriptReportFixturePath(const TCHAR* FileName)
{
	return FPaths::Combine(GetAvidScriptReportTestRoot(), FileName);
}

bool WriteAvidScriptReportFixture(const FString& ReportPath, const FString& ReportJson)
{
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true);
	return FFileHelper::SaveStringToFile(ReportJson, *ReportPath);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFrontendReportSuccessLoadTest,
	"AvidScript.Editor.Report.SuccessLoadSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFrontendReportSuccessLoadTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = GetAvidScriptReportFixturePath(TEXT("success.report.json"));
	const FString ReportJson = TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"diagnostic_schema_version\": 1,\n")
		TEXT("  \"source\": \"Samples/ActorSetLocation/actor_set_location.avid\",\n")
		TEXT("  \"bindings\": \"Bindings/ActorHostBindings.avidscript.json\",\n")
		TEXT("  \"output_root\": \"Saved/AvidScriptGenerated/actor_set_location\",\n")
		TEXT("  \"exit_code\": 0,\n")
		TEXT("  \"succeeded\": true,\n")
		TEXT("  \"diagnostics\": [],\n")
		TEXT("  \"build_events\": [\n")
		TEXT("    { \"result\": \"generated\", \"fields\": { \"source\": \"Saved/AvidScriptGenerated/actor_set_location/actor_set_location.generated.d\" } }\n")
		TEXT("  ],\n")
		TEXT("  \"raw_output\": [\"[AvidScript][Frontend][Build] result=generated\"]\n")
		TEXT("}\n");

	TestTrue(TEXT("Success fixture writes"), WriteAvidScriptReportFixture(ReportPath, ReportJson));

	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult LoadResult;
	TestTrue(TEXT("Success report loads"), FAvidScriptFrontendReportReader::LoadFromFile(ReportPath, Report, LoadResult));
	TestTrue(TEXT("Load result succeeded"), LoadResult.bSucceeded);
	TestEqual(TEXT("Schema version"), Report.SchemaVersion, 1);
	TestEqual(TEXT("Source"), Report.Source, FString(TEXT("Samples/ActorSetLocation/actor_set_location.avid")));
	TestEqual(TEXT("Bindings"), Report.Bindings, FString(TEXT("Bindings/ActorHostBindings.avidscript.json")));
	TestEqual(TEXT("Output root"), Report.OutputRoot, FString(TEXT("Saved/AvidScriptGenerated/actor_set_location")));
	TestEqual(TEXT("Exit code"), Report.ExitCode, 0);
	TestTrue(TEXT("Report succeeded"), Report.bSucceeded);
	TestEqual(TEXT("Diagnostic count"), Report.Diagnostics.Num(), 0);
	TestFalse(TEXT("No error diagnostics"), Report.HasErrorDiagnostics());
	TestEqual(TEXT("Build event count"), Report.BuildEvents.Num(), 1);

	const FAvidScriptFrontendBuildEvent* LastEvent = Report.GetLastBuildEvent();
	TestNotNull(TEXT("Last build event exists"), LastEvent);
	if (LastEvent != nullptr)
	{
		TestEqual(TEXT("Last build result"), LastEvent->Result, FString(TEXT("generated")));
		const FString* GeneratedSource = LastEvent->Fields.Find(TEXT("source"));
		TestNotNull(TEXT("Generated source field exists"), GeneratedSource);
		if (GeneratedSource != nullptr)
		{
			TestEqual(TEXT("Generated source field"), *GeneratedSource, FString(TEXT("Saved/AvidScriptGenerated/actor_set_location/actor_set_location.generated.d")));
		}
	}

	TestEqual(TEXT("Raw output count"), Report.RawOutput.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFrontendReportDiagnosticLoadTest,
	"AvidScript.Editor.Report.DiagnosticLoadSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFrontendReportDiagnosticLoadTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = GetAvidScriptReportFixturePath(TEXT("unknown_binding.report.json"));
	const FString ReportJson = TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"diagnostic_schema_version\": 1,\n")
		TEXT("  \"result\": \"unknown_binding\",\n")
		TEXT("  \"source\": \"Saved/AvidScriptGenerated/NegativeTests/p9_2_unknown_binding.avid\",\n")
		TEXT("  \"bindings\": \"Bindings/ActorHostBindings.avidscript.json\",\n")
		TEXT("  \"output_root\": \"Saved/AvidScriptGenerated\",\n")
		TEXT("  \"exit_code\": 1,\n")
		TEXT("  \"succeeded\": false,\n")
		TEXT("  \"diagnostics\": [\n")
		TEXT("    { \"schema_version\": 1, \"code\": \"ASL1202\", \"severity\": \"error\", \"stage\": \"frontend\", \"source_id\": \"Scripts/Broken.cs\", \"source_sha256\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\", \"module_id\": \"broken\", \"start\": 29, \"length\": 4, \"line\": 7, \"column\": 6, \"end_line\": 7, \"end_column\": 10, \"line_base\": 1, \"message\": \"unknown binding 'teleport_actor'\" }\n")
		TEXT("  ],\n")
		TEXT("  \"build_events\": [\n")
		TEXT("    { \"result\": \"unknown_binding\", \"fields\": { \"code\": \"ASL1202\", \"binding\": \"teleport_actor\" } }\n")
		TEXT("  ],\n")
		TEXT("  \"raw_output\": [\"[AvidScript][Frontend][Diagnostic] code=ASL1202\"]\n")
		TEXT("}\n");

	TestTrue(TEXT("Diagnostic fixture writes"), WriteAvidScriptReportFixture(ReportPath, ReportJson));

	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult LoadResult;
	TestTrue(TEXT("Diagnostic report loads"), FAvidScriptFrontendReportReader::LoadFromFile(ReportPath, Report, LoadResult));
	TestTrue(TEXT("Load result succeeded"), LoadResult.bSucceeded);
	TestEqual(TEXT("Report result"), Report.Result, FString(TEXT("unknown_binding")));
	TestEqual(TEXT("Diagnostic schema"), Report.DiagnosticSchemaVersion, 1);
	TestFalse(TEXT("Report did not succeed"), Report.bSucceeded);
	TestTrue(TEXT("Error diagnostics detected"), Report.HasErrorDiagnostics());
	TestEqual(TEXT("Diagnostic count"), Report.Diagnostics.Num(), 1);

	if (Report.Diagnostics.Num() == 1)
	{
		const FAvidScriptFrontendDiagnostic& Diagnostic = Report.Diagnostics[0];
		TestEqual(TEXT("Diagnostic code"), Diagnostic.Code, FString(TEXT("ASL1202")));
		TestEqual(TEXT("Diagnostic severity"), Diagnostic.Severity, FString(TEXT("error")));
		TestEqual(TEXT("Diagnostic stage"), Diagnostic.Stage, FString(TEXT("frontend")));
		TestEqual(TEXT("Diagnostic file"), Diagnostic.File, FString(TEXT("Scripts/Broken.cs")));
		TestEqual(TEXT("Diagnostic source hash"), Diagnostic.SourceSha256, FString::ChrN(64, TEXT('a')));
		TestEqual(TEXT("Diagnostic module"), Diagnostic.ModuleId, FString(TEXT("broken")));
		TestEqual(TEXT("Diagnostic start"), Diagnostic.Start, 29);
		TestEqual(TEXT("Diagnostic length"), Diagnostic.Length, 4);
		TestEqual(TEXT("Diagnostic line"), Diagnostic.Line, 7);
		TestEqual(TEXT("Diagnostic column"), Diagnostic.Column, 6);
		TestEqual(TEXT("Diagnostic end line"), Diagnostic.EndLine, 7);
		TestEqual(TEXT("Diagnostic end column"), Diagnostic.EndColumn, 10);
		TestEqual(TEXT("Diagnostic line base"), Diagnostic.LineBase, 1);
		TestEqual(TEXT("Diagnostic display line"), Diagnostic.GetDisplayLine(), 7);
		TestEqual(TEXT("Diagnostic display column"), Diagnostic.GetDisplayColumn(), 6);
		TestTrue(TEXT("Diagnostic source location is navigable"), Diagnostic.HasSourceLocation());
		TestEqual(TEXT("Diagnostic message"), Diagnostic.Message, FString(TEXT("unknown binding 'teleport_actor'")));
		TestTrue(TEXT("Diagnostic reports error"), Diagnostic.IsError());
	}

	const FAvidScriptFrontendBuildEvent* LastEvent = Report.GetLastBuildEvent();
	TestNotNull(TEXT("Last build event exists"), LastEvent);
	if (LastEvent != nullptr)
	{
		TestEqual(TEXT("Last build result"), LastEvent->Result, FString(TEXT("unknown_binding")));
		const FString* Binding = LastEvent->Fields.Find(TEXT("binding"));
		TestNotNull(TEXT("Binding field exists"), Binding);
		if (Binding != nullptr)
		{
			TestEqual(TEXT("Binding field"), *Binding, FString(TEXT("teleport_actor")));
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFrontendReportCSharpStructuredLoadTest,
	"AvidScript.Editor.Report.CSharpStructuredLoadSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFrontendReportCSharpStructuredLoadTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = GetAvidScriptReportFixturePath(TEXT("csharp_structured.report.json"));
	const FString ReportJson = TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"diagnostic_schema_version\": 1,\n")
		TEXT("  \"source\": { \"file\": \"Scripts/ActorLifecycleScript.cs\", \"sha256\": \"abc123\", \"script_type\": \"ActorLifecycleScript\" },\n")
		TEXT("  \"output_root\": \"Saved/AvidScriptCSharpGuest\",\n")
		TEXT("  \"succeeded\": true,\n")
		TEXT("  \"artifacts\": { \"frontend_file\": \"Saved/AvidScriptCSharpGuest/actor.csharp.frontend.json\" },\n")
		TEXT("  \"frontend\": { \"schema_version\": 1, \"version\": \"1.0\" },\n")
		TEXT("  \"semantic_cache\": { \"schema_version\": 1, \"enabled\": true, \"key\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\", \"toolchain_fingerprint\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\", \"lookup\": \"hit\", \"entry_report_file\": \"Saved/AvidScript/CSharpSemanticCache/v1/aa/entry.csharp.report.json\", \"entry_report_sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\", \"published\": false, \"diagnostic_code\": \"\", \"diagnostic_message\": \"\" },\n")
		TEXT("  \"compilation_cache\": { \"schema_version\": 1, \"enabled\": true, \"key\": \"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd\", \"toolchain_fingerprint\": \"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\", \"lookup\": \"hit\", \"entry_report_file\": \"Saved/AvidScript/CSharpCompilationCache/v1/dd/entry.json\", \"entry_report_sha256\": \"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\", \"published\": false, \"diagnostic_code\": \"\", \"diagnostic_message\": \"\" },\n")
		TEXT("  \"tool_invocations\": { \"frontend\": 0, \"semantic\": 0, \"guest_ir\": 0, \"wasm_backend\": 0 },\n")
		TEXT("  \"diagnostics\": []\n")
		TEXT("}\n");

	TestTrue(TEXT("C# structured fixture writes"), WriteAvidScriptReportFixture(ReportPath, ReportJson));

	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult LoadResult;
	TestTrue(TEXT("C# structured report loads"), FAvidScriptFrontendReportReader::LoadFromFile(ReportPath, Report, LoadResult));
	TestEqual(TEXT("C# source file"), Report.Source, FString(TEXT("Scripts/ActorLifecycleScript.cs")));
	TestEqual(TEXT("C# source hash"), Report.SourceSha256, FString(TEXT("abc123")));
	TestEqual(TEXT("C# script type"), Report.ScriptType, FString(TEXT("ActorLifecycleScript")));
	TestEqual(TEXT("C# frontend artifact"), Report.FrontendArtifact, FString(TEXT("Saved/AvidScriptCSharpGuest/actor.csharp.frontend.json")));
	TestEqual(TEXT("C# frontend schema"), Report.FrontendSchemaVersion, 1);
	TestEqual(TEXT("C# frontend version"), Report.FrontendVersion, FString(TEXT("1.0")));
	TestTrue(TEXT("Legacy report has no semantic artifact"), Report.SemanticArtifact.IsEmpty());
	TestEqual(TEXT("Legacy report semantic schema defaults to zero"), Report.SemanticSchemaVersion, 0);
	TestFalse(TEXT("Legacy report semantic success defaults to false"), Report.bSemanticSucceeded);
	TestFalse(TEXT("Legacy report has no binding package provenance"), Report.BindingPackage.bPresent);
	TestEqual(TEXT("Legacy report has no used binding imports"), Report.BindingPackage.UsedImports.Num(), 0);
	TestTrue(TEXT("C# report has structured tool invocation metadata"), Report.bHasToolInvocations);
	TestTrue(TEXT("C# report tool invocation metadata is valid"), Report.bToolInvocationsValid);
	TestEqual(TEXT("C# report Frontend invocation count"), Report.FrontendInvocationCount, 0);
	TestEqual(TEXT("C# report Semantic invocation count"), Report.SemanticInvocationCount, 0);
	TestEqual(TEXT("C# report Guest IR invocation count"), Report.GuestIrInvocationCount, 0);
	TestEqual(TEXT("C# report WASM backend invocation count"), Report.WasmBackendInvocationCount, 0);
	TestTrue(TEXT("C# report has semantic cache metadata"), Report.bHasSemanticCache);
	TestTrue(TEXT("C# report semantic cache metadata is valid"), Report.bSemanticCacheValid);
	TestEqual(TEXT("C# report semantic cache schema"), Report.SemanticCacheSchemaVersion, 1);
	TestTrue(TEXT("C# report semantic cache is enabled"), Report.bSemanticCacheEnabled);
	TestEqual(TEXT("C# report semantic cache lookup"), Report.SemanticCacheLookup, FString(TEXT("hit")));
	TestEqual(TEXT("C# report semantic cache key"), Report.SemanticCacheKey, FString::ChrN(64, TEXT('a')));
	TestEqual(
		TEXT("C# report semantic cache toolchain fingerprint"),
		Report.SemanticCacheToolchainFingerprint,
		FString::ChrN(64, TEXT('b')));
	TestFalse(TEXT("C# report semantic cache hit was not published"), Report.bSemanticCachePublished);
	TestTrue(TEXT("C# report semantic cache diagnostic is empty"), Report.SemanticCacheDiagnosticCode.IsEmpty());
	TestTrue(TEXT("C# report has compilation cache metadata"), Report.bHasCompilationCache);
	TestTrue(TEXT("C# report compilation cache metadata is valid"), Report.bCompilationCacheValid);
	TestEqual(TEXT("C# report compilation cache schema"), Report.CompilationCacheSchemaVersion, 1);
	TestTrue(TEXT("C# report compilation cache is enabled"), Report.bCompilationCacheEnabled);
	TestEqual(TEXT("C# report compilation cache lookup"), Report.CompilationCacheLookup, FString(TEXT("hit")));
	TestEqual(TEXT("C# report compilation cache key"), Report.CompilationCacheKey, FString::ChrN(64, TEXT('d')));
	TestEqual(
		TEXT("C# report compilation cache toolchain fingerprint"),
		Report.CompilationCacheToolchainFingerprint,
		FString::ChrN(64, TEXT('e')));
	TestFalse(TEXT("C# report compilation cache hit was not published"), Report.bCompilationCachePublished);
	TestTrue(TEXT("C# report compilation cache diagnostic is empty"), Report.CompilationCacheDiagnosticCode.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFrontendReportCSharpSemanticLoadTest,
	"AvidScript.Editor.Report.CSharpSemanticLoadSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFrontendReportCSharpSemanticLoadTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = GetAvidScriptReportFixturePath(TEXT("csharp_semantic.report.json"));
	const FString ReportJson = TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"source\": { \"file\": \"Scripts/ActorLifecycleScript.cs\", \"sha256\": \"abc123\", \"script_type\": \"ActorLifecycleScript\" },\n")
		TEXT("  \"output_root\": \"Saved/AvidScriptCSharpGuest\",\n")
		TEXT("  \"succeeded\": true,\n")
		TEXT("  \"artifacts\": { \"frontend_file\": \"Saved/AvidScriptCSharpGuest/actor.csharp.frontend.json\", \"semantic_file\": \"Saved/AvidScriptCSharpGuest/actor.csharp.semantic.json\", \"guest_ir_file\": \"Saved/AvidScriptCSharpGuest/actor.guestir.json\" },\n")
		TEXT("  \"frontend\": { \"schema_version\": 1, \"version\": \"1.0\" },\n")
		TEXT("  \"semantic\": { \"schema_version\": 4, \"version\": \"1.4\", \"succeeded\": true, \"source_sha256\": \"abc123\", \"frontend_sha256\": \"abc123\", \"diagnostic_count\": 0 },\n")
		TEXT("  \"guest_ir\": { \"schema_version\": 1, \"version\": \"1.0\", \"succeeded\": true, \"semantic_sha256\": \"semantic456\", \"sha256\": \"guest789\" },\n")
		TEXT("  \"diagnostics\": []\n")
		TEXT("}\n");

	TestTrue(TEXT("C# semantic fixture writes"), WriteAvidScriptReportFixture(ReportPath, ReportJson));

	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult LoadResult;
	TestTrue(TEXT("C# semantic report loads"), FAvidScriptFrontendReportReader::LoadFromFile(ReportPath, Report, LoadResult));
	TestEqual(TEXT("C# semantic artifact"), Report.SemanticArtifact, FString(TEXT("Saved/AvidScriptCSharpGuest/actor.csharp.semantic.json")));
	TestEqual(TEXT("C# semantic schema"), Report.SemanticSchemaVersion, 4);
	TestEqual(TEXT("C# semantic version"), Report.SemanticVersion, FString(TEXT("1.4")));
	TestTrue(TEXT("C# semantic succeeded"), Report.bSemanticSucceeded);
	TestEqual(TEXT("C# semantic source hash"), Report.SemanticSourceSha256, FString(TEXT("abc123")));
	TestEqual(TEXT("C# semantic frontend hash"), Report.SemanticFrontendSha256, FString(TEXT("abc123")));
	TestEqual(TEXT("C# semantic diagnostic count"), Report.SemanticDiagnosticCount, 0);
	TestEqual(TEXT("C# Guest IR artifact"), Report.GuestIrArtifact, FString(TEXT("Saved/AvidScriptCSharpGuest/actor.guestir.json")));
	TestEqual(TEXT("C# Guest IR schema"), Report.GuestIrSchemaVersion, 1);
	TestEqual(TEXT("C# Guest IR version"), Report.GuestIrVersion, FString(TEXT("1.0")));
	TestTrue(TEXT("C# Guest IR succeeded"), Report.bGuestIrSucceeded);
	TestEqual(TEXT("C# Guest IR semantic hash"), Report.GuestIrSemanticSha256, FString(TEXT("semantic456")));
	TestEqual(TEXT("C# Guest IR artifact hash"), Report.GuestIrSha256, FString(TEXT("guest789")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFrontendReportCSharpBindingProvenanceLoadTest,
	"AvidScript.Editor.Report.CSharpBindingProvenanceSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFrontendReportCSharpBindingProvenanceLoadTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = GetAvidScriptReportFixturePath(TEXT("csharp_binding_provenance.report.json"));
	const FString ReportJson = TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"result\": \"direct_abi_built\",\n")
		TEXT("  \"succeeded\": true,\n")
		TEXT("  \"binding_package\": {\n")
		TEXT("    \"required\": true,\n")
		TEXT("    \"package_manifest\": \"Saved/Bindings/package.json\",\n")
		TEXT("    \"package_name\": \"avidscript.engine.gameplay\",\n")
		TEXT("    \"package_hash\": \"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\n")
		TEXT("    \"descriptor_file\": \"Saved/Bindings/descriptor.json\",\n")
		TEXT("    \"descriptor_sha256\": \"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\",\n")
		TEXT("    \"reference_source_file\": \"Saved/Bindings/AvidScript.Bindings.generated.cs\",\n")
		TEXT("    \"reference_source_sha256\": \"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\",\n")
		TEXT("    \"profile_import_count\": 115,\n")
		TEXT("    \"used_import_count\": 2,\n")
		TEXT("    \"used_imports\": [\n")
		TEXT("      { \"stable_id\": \"1111111111111111111111111111111111111111111111111111111111111111\", \"ordinal\": 10, \"module\": \"avidscript\", \"name\": \"avid_ue_1111111111111111\", \"signature\": \"(ii)i\" },\n")
		TEXT("      { \"stable_id\": \"2222222222222222222222222222222222222222222222222222222222222222\", \"ordinal\": 20, \"module\": \"avidscript\", \"name\": \"avid_ue_2222222222222222\", \"signature\": \"(i)i\" }\n")
		TEXT("    ],\n")
		TEXT("    \"used_object_type_count\": 2,\n")
		TEXT("    \"used_object_type_ordinals\": [3, 7]\n")
		TEXT("  },\n")
		TEXT("  \"diagnostics\": []\n")
		TEXT("}\n");

	TestTrue(TEXT("C# binding provenance fixture writes"), WriteAvidScriptReportFixture(ReportPath, ReportJson));

	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult LoadResult;
	TestTrue(TEXT("C# binding provenance report loads"), FAvidScriptFrontendReportReader::LoadFromFile(ReportPath, Report, LoadResult));
	TestTrue(TEXT("Binding package provenance is present"), Report.BindingPackage.bPresent);
	TestEqual(TEXT("Binding package manifest"), Report.BindingPackage.PackageManifest, FString(TEXT("Saved/Bindings/package.json")));
	TestEqual(TEXT("Binding package name"), Report.BindingPackage.PackageName, FString(TEXT("avidscript.engine.gameplay")));
	TestEqual(TEXT("Binding package descriptor"), Report.BindingPackage.DescriptorFile, FString(TEXT("Saved/Bindings/descriptor.json")));
	TestEqual(TEXT("Binding package profile import count"), Report.BindingPackage.ProfileImportCount, 115);
	TestEqual(TEXT("Binding package declared used import count"), Report.BindingPackage.UsedImportCount, 2);
	TestEqual(TEXT("Binding package parsed used import count"), Report.BindingPackage.UsedImports.Num(), 2);
	TestEqual(
		TEXT("Binding package declared used object-type count"),
		Report.BindingPackage.UsedObjectTypeCount,
		2);
	TestTrue(
		TEXT("Binding package preserves used object-type ordinals"),
		Report.BindingPackage.UsedObjectTypeOrdinals == TArray<int32>{ 3, 7 });
	if (Report.BindingPackage.UsedImports.Num() == 2)
	{
		const FAvidScriptFrontendBindingImport& First = Report.BindingPackage.UsedImports[0];
		const FAvidScriptFrontendBindingImport& Second = Report.BindingPackage.UsedImports[1];
		TestEqual(TEXT("First used stable ID preserves order"), First.StableId, FString::ChrN(64, TEXT('1')));
		TestEqual(TEXT("First used ordinal"), First.Ordinal, 10);
		TestEqual(TEXT("First used import name"), First.Name, FString(TEXT("avid_ue_1111111111111111")));
		TestEqual(TEXT("Second used stable ID preserves order"), Second.StableId, FString::ChrN(64, TEXT('2')));
		TestEqual(TEXT("Second used signature"), Second.Signature, FString(TEXT("(i)i")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFrontendReportMissingFileTest,
	"AvidScript.Editor.Report.MissingFileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFrontendReportMissingFileTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = GetAvidScriptReportFixturePath(TEXT("missing.report.json"));
	IFileManager::Get().Delete(*ReportPath);

	FAvidScriptFrontendReport Report;
	FAvidScriptFrontendReportLoadResult LoadResult;
	TestFalse(TEXT("Missing report fails"), FAvidScriptFrontendReportReader::LoadFromFile(ReportPath, Report, LoadResult));
	TestFalse(TEXT("Load result did not succeed"), LoadResult.bSucceeded);
	TestEqual(TEXT("Missing report category"), LoadResult.ErrorCategory, FString(TEXT("report_missing")));
	TestEqual(TEXT("Missing report path"), LoadResult.ReportPath, ReportPath);

	return true;
}

#endif
