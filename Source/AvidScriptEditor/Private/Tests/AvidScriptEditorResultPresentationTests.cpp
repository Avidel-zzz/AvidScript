#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorResultPresentation.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "AvidScriptEditorCSharpWorkspaceService.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationRuntimeDiagnosticsTest,
	"AvidScript.Editor.Presentation.RuntimeDiagnostics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationRuntimeDiagnosticsTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandLaunchResult Result;
	Result.bSucceeded = false;
	Result.SourcePath = TEXT("C:/Project/Scripts/AvidScript/GameplayScript.cs");
	Result.CommandResult.ErrorCategory = TEXT("trap");
	Result.CommandResult.ErrorMessage = TEXT("Exception: unreachable");

	TArray<FAvidScriptWasmDiagnosticFrame>& Frames =
		Result.CommandResult.ReloadApplyResult.RuntimeResult.RuntimeResult.DiagnosticFrames;
	FAvidScriptWasmDiagnosticFrame& MappedFrame = Frames.AddDefaulted_GetRef();
	MappedFrame.FunctionIndex = 7;
	MappedFrame.FunctionOffset = 0x2a;
	MappedFrame.RawFunctionToken = TEXT("$f7");
	MappedFrame.FunctionName = TEXT("AvidScript.GameplayScript.Helper()");
	MappedFrame.SourceFile = TEXT("Scripts/AvidScript/GameplayScript.cs");
	MappedFrame.Line = 17;
	MappedFrame.Column = 5;
	MappedFrame.EndLine = 19;
	MappedFrame.EndColumn = 6;
	MappedFrame.bSourceMapped = true;
	FAvidScriptWasmDiagnosticFrame& RawFrame = Frames.AddDefaulted_GetRef();
	RawFrame.FunctionIndex = 8;
	RawFrame.FunctionOffset = 3;
	RawFrame.RawFunctionToken = TEXT("$f8");

	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakePresentation(Result);
	TestTrue(
		TEXT("mapped frame renders C# identity and one-based source position"),
		Presentation.Details.Contains(
			TEXT("at AvidScript.GameplayScript.Helper() (Scripts/AvidScript/GameplayScript.cs:17:5)")));
	TestTrue(
		TEXT("mapped frame preserves raw WASM evidence"),
		Presentation.Details.Contains(TEXT("wasm frame: function=7 offset=0x0000002a token=$f7")));
	TestTrue(
		TEXT("unmapped frame preserves raw WASM evidence"),
		Presentation.Details.Contains(TEXT("wasm frame: function=8 offset=0x00000003 token=$f8")));
	TestFalse(
		TEXT("unmapped frame does not fabricate a source identity"),
		Presentation.Details.Contains(TEXT("at $f8")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationCSharpWorkspaceTest,
	"AvidScript.Editor.Presentation.CSharpWorkspaceSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationCSharpWorkspaceTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpWorkspaceResult CreatedResult;
	CreatedResult.bSucceeded = true;
	CreatedResult.bFacadeRefreshed = true;
	CreatedResult.CreatedUserFileCount = 4;
	CreatedResult.WorkspaceRoot = TEXT("C:/Project/Scripts/AvidScript");
	CreatedResult.SourcePath = TEXT("C:/Project/Scripts/AvidScript/GameplayScript.cs");
	CreatedResult.ProjectPath = TEXT("C:/Project/Scripts/AvidScript/AvidScript.Gameplay.csproj");
	CreatedResult.ProfilePath = TEXT("C:/Project/Scripts/AvidScript/default.csharp-profile.json");
	CreatedResult.FacadePath = TEXT("C:/Project/Intermediate/AvidScript/CSharpWorkspace/AvidScript.Bindings.generated.cs");
	CreatedResult.BindingPackageManifestPath = TEXT("C:/Project/Intermediate/AvidScript/CSharpWorkspace/BindingPackages/package.json");
	CreatedResult.ManifestPath = TEXT("C:/Project/Saved/AvidScriptCSharpGuest/ProjectGameplay/project_gameplay.avidscript.json");
	CreatedResult.NextAction = TEXT("open GameplayScript.cs, then build and bind");

	const FAvidScriptEditorCommandPresentation CreatedPresentation =
		FAvidScriptEditorResultPresenter::MakeCSharpWorkspacePresentation(CreatedResult);
	TestEqual(TEXT("C# workspace created severity"), CreatedPresentation.Severity, EAvidScriptEditorPresentationSeverity::Info);
	TestTrue(TEXT("C# workspace created title"), CreatedPresentation.Title.Contains(TEXT("workspace ready")));
	TestTrue(TEXT("C# workspace created body count"), CreatedPresentation.Body.Contains(TEXT("created=4")));
	TestTrue(TEXT("C# workspace details include source"), CreatedPresentation.Details.Contains(TEXT("GameplayScript.cs")));
	TestTrue(TEXT("C# workspace details include facade"), CreatedPresentation.Details.Contains(TEXT("AvidScript.Bindings.generated.cs")));
	TestTrue(TEXT("C# workspace details include refreshed state"), CreatedPresentation.Details.Contains(TEXT("facade_refreshed=true")));

	FAvidScriptEditorCSharpWorkspaceResult PreservedResult = CreatedResult;
	PreservedResult.CreatedUserFileCount = 0;
	PreservedResult.PreservedUserFileCount = 4;
	const FAvidScriptEditorCommandPresentation PreservedPresentation =
		FAvidScriptEditorResultPresenter::MakeCSharpWorkspacePresentation(PreservedResult);
	TestTrue(TEXT("C# workspace preserved body count"), PreservedPresentation.Body.Contains(TEXT("preserved=4")));

	FAvidScriptEditorCSharpWorkspaceResult FailureResult;
	FailureResult.bSucceeded = false;
	FailureResult.ErrorCategory = TEXT("workspace_template_missing");
	FailureResult.ErrorMessage = TEXT("C# workspace template could not be read");
	FailureResult.NextAction = TEXT("restore the plugin templates and retry");
	FailureResult.SourcePath = TEXT("C:/Project/Scripts/AvidScript/GameplayScript.cs");
	const FAvidScriptEditorCommandPresentation FailurePresentation =
		FAvidScriptEditorResultPresenter::MakeCSharpWorkspacePresentation(FailureResult);
	TestEqual(TEXT("C# workspace failure severity"), FailurePresentation.Severity, EAvidScriptEditorPresentationSeverity::Error);
	TestTrue(TEXT("C# workspace failure title"), FailurePresentation.Title.Contains(TEXT("failed")));
	TestTrue(TEXT("C# workspace failure category"), FailurePresentation.Body.Contains(TEXT("workspace_template_missing")));
	TestTrue(TEXT("C# workspace failure next action"), FailurePresentation.Details.Contains(TEXT("restore")));
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationCSharpProfileTemplateCreatedTest,
	"AvidScript.Editor.Presentation.CSharpProfileTemplateCreatedSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationCSharpProfileTemplateCreatedTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpProfileTemplateResult Result;
	Result.bSucceeded = true;
	Result.bCreated = true;
	Result.NormalizedProfilePath = TEXT("C:/Project/Saved/AvidScriptCSharpProfiles/default.csharp-profile.json");
	Result.SourcePath = TEXT("C:/Project/Plugins/AvidScript/Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs");
	Result.ReportPath = TEXT("C:/Project/Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle/profile_actor_lifecycle.csharp.report.json");
	Result.ManifestPath = TEXT("C:/Project/Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle/profile_actor_lifecycle.avidscript.json");
	Result.ModuleId = TEXT("csharp_profile_actor_lifecycle");
	Result.NextAction = TEXT("edit source_path if needed, then run Build And Bind C# Profile Script");

	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakeCSharpProfileTemplatePresentation(Result);
	TestEqual(TEXT("C# profile template created severity"), Presentation.Severity, EAvidScriptEditorPresentationSeverity::Info);
	TestTrue(TEXT("C# profile template created title"), Presentation.Title.Contains(TEXT("C# profile ready")));
	TestTrue(TEXT("C# profile template created body mentions created"), Presentation.Body.Contains(TEXT("created")));
	TestTrue(TEXT("C# profile template created details include profile"), Presentation.Details.Contains(TEXT("default.csharp-profile.json")));
	TestTrue(TEXT("C# profile template created details include source"), Presentation.Details.Contains(TEXT("ActorLifecycleScript.cs")));
	TestTrue(TEXT("C# profile template created details include next action"), Presentation.Details.Contains(TEXT("Build And Bind C# Profile Script")));
	TestEqual(TEXT("C# profile template source copied"), Presentation.SourcePath, Result.SourcePath);
	TestEqual(TEXT("C# profile template manifest copied"), Presentation.ManifestPath, Result.ManifestPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationCSharpProfileTemplateFailureTest,
	"AvidScript.Editor.Presentation.CSharpProfileTemplateFailureSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationCSharpProfileTemplateFailureTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpProfileTemplateResult Result;
	Result.bSucceeded = false;
	Result.ErrorCategory = TEXT("profile_write_failed");
	Result.ErrorMessage = TEXT("C# profile template could not be written: C:/Project/Saved/AvidScriptCSharpProfiles/default.csharp-profile.json");
	Result.NextAction = TEXT("verify the destination path is writable and retry");
	Result.NormalizedProfilePath = TEXT("C:/Project/Saved/AvidScriptCSharpProfiles/default.csharp-profile.json");

	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakeCSharpProfileTemplatePresentation(Result);
	TestEqual(TEXT("C# profile template failure severity"), Presentation.Severity, EAvidScriptEditorPresentationSeverity::Error);
	TestTrue(TEXT("C# profile template failure title"), Presentation.Title.Contains(TEXT("failed")));
	TestTrue(TEXT("C# profile template failure body category"), Presentation.Body.Contains(TEXT("profile_write_failed")));
	TestTrue(TEXT("C# profile template failure body message"), Presentation.Body.Contains(TEXT("could not be written")));
	TestTrue(TEXT("C# profile template failure details next action"), Presentation.Details.Contains(TEXT("writable")));
	TestTrue(TEXT("C# profile template failure details profile"), Presentation.Details.Contains(TEXT("default.csharp-profile.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationCSharpProfileBuildAndBindSuccessTest,
	"AvidScript.Editor.Presentation.CSharpProfileBuildAndBindSuccessSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationCSharpProfileBuildAndBindSuccessTest::RunTest(const FString& Parameters)
{
	const FString ProfilePath = TEXT("C:/Project/Saved/AvidScriptCSharpProfiles/default.csharp-profile.json");

	FAvidScriptEditorCSharpBuildResult BuildResult;
	BuildResult.bSucceeded = true;
	BuildResult.SourcePath = TEXT("C:/Project/Plugins/AvidScript/Samples/CSharp/ActorLifecycle/ActorLifecycleScript.cs");
	BuildResult.ReportPath = TEXT("C:/Project/Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle/profile_actor_lifecycle.csharp.report.json");
	BuildResult.ManifestPath = TEXT("C:/Project/Saved/AvidScriptCSharpGuest/Profiles/profile_actor_lifecycle/profile_actor_lifecycle.avidscript.json");
	BuildResult.ModuleId = TEXT("csharp_profile_actor_lifecycle");

	FAvidScriptEditorComponentBindingResult BindingResult;
	BindingResult.bSucceeded = true;
	BindingResult.ActorPath = TEXT("/Temp/AvidScriptProfileActor.AvidScriptProfileActor");
	BindingResult.NormalizedManifestPath = BuildResult.ManifestPath;
	BindingResult.ReportPath = BuildResult.ReportPath;

	BuildResult.BuildInvocationCount = 2;
	BuildResult.FrontendInvocationCount = 0;
	BuildResult.SemanticInvocationCount = 0;
	BuildResult.GuestIrInvocationCount = 2;
	BuildResult.WasmBackendInvocationCount = 2;
	BuildResult.SemanticCacheLookup = TEXT("hit");
	BuildResult.CompilationCacheLookup = TEXT("miss");
	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakeCSharpProfileBuildAndBindPresentation(ProfilePath, BuildResult, BindingResult);
	TestEqual(TEXT("C# profile build-and-bind success severity"), Presentation.Severity, EAvidScriptEditorPresentationSeverity::Info);
	TestTrue(TEXT("C# profile build-and-bind success title"), Presentation.Title.Contains(TEXT("C# profile bound")));
	TestTrue(TEXT("C# profile build-and-bind success body module"), Presentation.Body.Contains(TEXT("csharp_profile_actor_lifecycle")));
	TestTrue(TEXT("C# profile build-and-bind success body actor"), Presentation.Body.Contains(TEXT("AvidScriptProfileActor")));
	TestTrue(TEXT("C# profile build-and-bind success details profile"), Presentation.Details.Contains(TEXT("default.csharp-profile.json")));
	TestTrue(TEXT("C# profile build-and-bind success details report"), Presentation.Details.Contains(TEXT("csharp.report.json")));
	TestTrue(TEXT("C# profile build-and-bind details include semantic cache"), Presentation.Details.Contains(TEXT("semantic_cache=hit")));
	TestTrue(TEXT("C# profile build-and-bind details include compilation cache"), Presentation.Details.Contains(TEXT("compilation_cache=miss")));
	TestTrue(TEXT("C# profile build-and-bind details include Frontend count"), Presentation.Details.Contains(TEXT("frontend=0")));
	TestTrue(TEXT("C# profile build-and-bind details include WASM count"), Presentation.Details.Contains(TEXT("wasm=2")));
	TestEqual(TEXT("C# profile build-and-bind source copied"), Presentation.SourcePath, BuildResult.SourcePath);
	TestEqual(TEXT("C# profile build-and-bind manifest copied"), Presentation.ManifestPath, BuildResult.ManifestPath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationCSharpProfileBuildFailureTest,
	"AvidScript.Editor.Presentation.CSharpProfileBuildFailureSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationCSharpProfileBuildFailureTest::RunTest(const FString& Parameters)
{
	const FString ProfilePath = TEXT("C:/Project/Saved/AvidScriptCSharpProfiles/default.csharp-profile.json");

	FAvidScriptEditorCSharpBuildResult BuildResult;
	BuildResult.bSucceeded = false;
	BuildResult.ErrorCategory = TEXT("source_missing");
	BuildResult.ErrorMessage = TEXT("C# source file does not exist: C:/Project/Scripts/Missing.cs");
	BuildResult.NextAction = TEXT("choose an existing C# source file or regenerate the default C# profile");
	BuildResult.SourcePath = TEXT("C:/Project/Scripts/Missing.cs");
	BuildResult.ReportPath = TEXT("C:/Project/Saved/AvidScriptCSharpGuest/Missing/missing.csharp.report.json");
	BuildResult.Stderr = TEXT("source file missing");

	FAvidScriptEditorComponentBindingResult BindingResult;

	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakeCSharpProfileBuildAndBindPresentation(ProfilePath, BuildResult, BindingResult);
	TestEqual(TEXT("C# profile build failure severity"), Presentation.Severity, EAvidScriptEditorPresentationSeverity::Error);
	TestTrue(TEXT("C# profile build failure title"), Presentation.Title.Contains(TEXT("build failed")));
	TestTrue(TEXT("C# profile build failure body category"), Presentation.Body.Contains(TEXT("source_missing")));
	TestTrue(TEXT("C# profile build failure details next action"), Presentation.Details.Contains(TEXT("existing C# source")));
	TestTrue(TEXT("C# profile build failure details stderr"), Presentation.Details.Contains(TEXT("source file missing")));
	TestEqual(TEXT("C# profile build failure source copied"), Presentation.SourcePath, BuildResult.SourcePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorPresentationCSharpProfileBindingFailureTest,
	"AvidScript.Editor.Presentation.CSharpProfileBindingFailureSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorPresentationCSharpProfileBindingFailureTest::RunTest(const FString& Parameters)
{
	const FString ProfilePath = TEXT("C:/Project/Saved/AvidScriptCSharpProfiles/default.csharp-profile.json");

	FAvidScriptEditorCSharpBuildResult BuildResult;
	BuildResult.bSucceeded = true;
	BuildResult.SourcePath = TEXT("C:/Project/Scripts/Mover.cs");
	BuildResult.ReportPath = TEXT("C:/Project/Saved/AvidScriptCSharpGuest/Mover/mover.csharp.report.json");
	BuildResult.ManifestPath = TEXT("C:/Project/Saved/AvidScriptCSharpGuest/Mover/mover.avidscript.json");
	BuildResult.ModuleId = TEXT("csharp_mover");

	FAvidScriptEditorComponentBindingResult BindingResult;
	BindingResult.bSucceeded = false;
	BindingResult.ErrorCategory = TEXT("selection_unavailable");
	BindingResult.ErrorMessage = TEXT("No Actor is selected.");
	BindingResult.NextAction = TEXT("select an Actor in the level and rerun Build And Bind C# Profile Script");
	BindingResult.ReportPath = BuildResult.ReportPath;
	BindingResult.NormalizedManifestPath = BuildResult.ManifestPath;

	const FAvidScriptEditorCommandPresentation Presentation =
		FAvidScriptEditorResultPresenter::MakeCSharpProfileBuildAndBindPresentation(ProfilePath, BuildResult, BindingResult);
	TestEqual(TEXT("C# profile binding failure severity"), Presentation.Severity, EAvidScriptEditorPresentationSeverity::Warning);
	TestTrue(TEXT("C# profile binding failure title"), Presentation.Title.Contains(TEXT("binding failed")));
	TestTrue(TEXT("C# profile binding failure body category"), Presentation.Body.Contains(TEXT("selection_unavailable")));
	TestTrue(TEXT("C# profile binding failure details next action"), Presentation.Details.Contains(TEXT("select an Actor")));
	TestEqual(TEXT("C# profile binding failure manifest copied"), Presentation.ManifestPath, BuildResult.ManifestPath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
