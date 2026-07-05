#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorResultPresentation.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationReloadSuccessTest,
	"AvidScript.Editor.Presentation.ReloadSuccessSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationReloadSuccessTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandLaunchResult Result;
	Result.bSucceeded = true;
	Result.bReloadApplied = true;
	Result.SourcePath = TEXT("C:/Project/Scripts/actor_set_location.avid");
	Result.ManifestPath = TEXT("C:/Project/Saved/AvidScriptGenerated/actor_set_location/actor_set_location.avidscript.json");
	Result.Summary = TEXT("reload_applied: manifest=C:/Project/Saved/AvidScriptGenerated/actor_set_location/actor_set_location.avidscript.json module=actor_set_location");
	Result.CommandResult.Status = EAvidScriptEditorCommandStatus::ReloadApplied;
	Result.CommandResult.CompileResult.Manifest.ModuleId = TEXT("actor_set_location");

	const FAvidScriptEditorCommandPresentation Presentation = FAvidScriptEditorResultPresenter::MakePresentation(Result);
	TestEqual(TEXT("Reload success severity"), Presentation.Severity, EAvidScriptEditorPresentationSeverity::Info);
	TestTrue(TEXT("Reload success title"), Presentation.Title.Contains(TEXT("Reload applied")));
	TestTrue(TEXT("Reload success body mentions module"), Presentation.Body.Contains(TEXT("actor_set_location")));
	TestEqual(TEXT("Source path copied"), Presentation.SourcePath, Result.SourcePath);
	TestEqual(TEXT("Manifest path copied"), Presentation.ManifestPath, Result.ManifestPath);
	TestTrue(TEXT("Details include summary"), Presentation.Details.Contains(TEXT("reload_applied")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationGeneratedOnlyTest,
	"AvidScript.Editor.Presentation.GeneratedOnlySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationGeneratedOnlyTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandLaunchResult Result;
	Result.bSucceeded = true;
	Result.bReloadApplied = false;
	Result.SourcePath = TEXT("C:/Project/Scripts/actor_set_location.avid");
	Result.ReportPath = TEXT("C:/Project/Saved/AvidScriptReports/actor_set_location.frontend.report.json");
	Result.Summary = TEXT("generated: source=C:/Project/Scripts/actor_set_location.avid report=C:/Project/Saved/AvidScriptReports/actor_set_location.frontend.report.json");
	Result.CommandResult.Status = EAvidScriptEditorCommandStatus::GeneratedOnly;

	const FAvidScriptEditorCommandPresentation Presentation = FAvidScriptEditorResultPresenter::MakePresentation(Result);
	TestEqual(TEXT("Generated-only severity"), Presentation.Severity, EAvidScriptEditorPresentationSeverity::Warning);
	TestTrue(TEXT("Generated-only title"), Presentation.Title.Contains(TEXT("Generated")));
	TestTrue(TEXT("Generated-only body mentions no reload"), Presentation.Body.Contains(TEXT("No live reload")));
	TestEqual(TEXT("Generated-only source path copied"), Presentation.SourcePath, Result.SourcePath);
	TestTrue(TEXT("Generated-only details include report"), Presentation.Details.Contains(TEXT("frontend.report.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationFailureTest,
	"AvidScript.Editor.Presentation.FailureSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationFailureTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandLaunchResult Result;
	Result.bSucceeded = false;
	Result.bReloadApplied = false;
	Result.SourcePath = TEXT("C:/Project/Scripts/missing.avid");
	Result.Summary = TEXT("source_missing: AvidScript source does not exist: C:/Project/Scripts/missing.avid");
	Result.CommandResult.ErrorCategory = TEXT("source_missing");
	Result.CommandResult.ErrorMessage = TEXT("AvidScript source does not exist: C:/Project/Scripts/missing.avid");
	Result.CommandResult.NextAction = TEXT("choose an existing .avid source file");

	const FAvidScriptEditorCommandPresentation Presentation = FAvidScriptEditorResultPresenter::MakePresentation(Result);
	TestEqual(TEXT("Failure severity"), Presentation.Severity, EAvidScriptEditorPresentationSeverity::Error);
	TestTrue(TEXT("Failure title"), Presentation.Title.Contains(TEXT("failed")));
	TestTrue(TEXT("Failure body mentions category"), Presentation.Body.Contains(TEXT("source_missing")));
	TestTrue(TEXT("Failure body mentions message"), Presentation.Body.Contains(TEXT("does not exist")));
	TestTrue(TEXT("Failure details include next action"), Presentation.Details.Contains(TEXT("choose an existing")));
	TestEqual(TEXT("Failure source path copied"), Presentation.SourcePath, Result.SourcePath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
