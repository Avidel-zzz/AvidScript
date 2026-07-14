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
		TEXT("  \"source\": \"Saved/AvidScriptGenerated/NegativeTests/p9_2_unknown_binding.avid\",\n")
		TEXT("  \"bindings\": \"Bindings/ActorHostBindings.avidscript.json\",\n")
		TEXT("  \"output_root\": \"Saved/AvidScriptGenerated\",\n")
		TEXT("  \"exit_code\": 1,\n")
		TEXT("  \"succeeded\": false,\n")
		TEXT("  \"diagnostics\": [\n")
		TEXT("    { \"code\": \"ASL1202\", \"severity\": \"error\", \"file\": \"Scripts/Broken.cs\", \"start\": 29, \"length\": 4, \"line\": 6, \"column\": 5, \"end_line\": 6, \"end_column\": 9, \"message\": \"unknown binding 'teleport_actor'\" }\n")
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
	TestFalse(TEXT("Report did not succeed"), Report.bSucceeded);
	TestTrue(TEXT("Error diagnostics detected"), Report.HasErrorDiagnostics());
	TestEqual(TEXT("Diagnostic count"), Report.Diagnostics.Num(), 1);

	if (Report.Diagnostics.Num() == 1)
	{
		const FAvidScriptFrontendDiagnostic& Diagnostic = Report.Diagnostics[0];
		TestEqual(TEXT("Diagnostic code"), Diagnostic.Code, FString(TEXT("ASL1202")));
		TestEqual(TEXT("Diagnostic severity"), Diagnostic.Severity, FString(TEXT("error")));
		TestEqual(TEXT("Diagnostic file"), Diagnostic.File, FString(TEXT("Scripts/Broken.cs")));
		TestEqual(TEXT("Diagnostic start"), Diagnostic.Start, 29);
		TestEqual(TEXT("Diagnostic length"), Diagnostic.Length, 4);
		TestEqual(TEXT("Diagnostic line"), Diagnostic.Line, 6);
		TestEqual(TEXT("Diagnostic column"), Diagnostic.Column, 5);
		TestEqual(TEXT("Diagnostic end line"), Diagnostic.EndLine, 6);
		TestEqual(TEXT("Diagnostic end column"), Diagnostic.EndColumn, 9);
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
		TEXT("  \"source\": { \"file\": \"Scripts/ActorLifecycleScript.cs\", \"sha256\": \"abc123\", \"script_type\": \"ActorLifecycleScript\" },\n")
		TEXT("  \"output_root\": \"Saved/AvidScriptCSharpGuest\",\n")
		TEXT("  \"succeeded\": true,\n")
		TEXT("  \"artifacts\": { \"frontend_file\": \"Saved/AvidScriptCSharpGuest/actor.csharp.frontend.json\" },\n")
		TEXT("  \"frontend\": { \"schema_version\": 1, \"version\": \"1.0\" },\n")
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
		TEXT("  \"artifacts\": { \"frontend_file\": \"Saved/AvidScriptCSharpGuest/actor.csharp.frontend.json\", \"semantic_file\": \"Saved/AvidScriptCSharpGuest/actor.csharp.semantic.json\" },\n")
		TEXT("  \"frontend\": { \"schema_version\": 1, \"version\": \"1.0\" },\n")
		TEXT("  \"semantic\": { \"schema_version\": 4, \"version\": \"1.4\", \"succeeded\": true, \"source_sha256\": \"abc123\", \"frontend_sha256\": \"abc123\", \"diagnostic_count\": 0 },\n")
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