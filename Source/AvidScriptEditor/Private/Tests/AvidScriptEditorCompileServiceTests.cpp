#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCompileService.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
FAvidScriptFrontendInvocationResult MakeAvidScriptCompileServiceInvocation(
	const bool bInvocationSucceeded,
	const bool bReportSucceeded,
	const int32 ExitCode,
	const FString& BuildResult)
{
	FAvidScriptFrontendInvocationResult InvocationResult;
	InvocationResult.bSucceeded = bInvocationSucceeded;
	InvocationResult.ProcessExitCode = ExitCode;
	InvocationResult.ReportLoadResult.bSucceeded = true;
	InvocationResult.Report.SchemaVersion = 1;
	InvocationResult.Report.ExitCode = ExitCode;
	InvocationResult.Report.bSucceeded = bReportSucceeded;

	FAvidScriptFrontendBuildEvent BuildEvent;
	BuildEvent.Result = BuildResult;
	InvocationResult.Report.BuildEvents.Add(MoveTemp(BuildEvent));

	return InvocationResult;
}

void AddAvidScriptCompileServiceDiagnostic(FAvidScriptFrontendInvocationResult& InvocationResult)
{
	FAvidScriptFrontendDiagnostic Diagnostic;
	Diagnostic.Code = TEXT("ASL1202");
	Diagnostic.Severity = TEXT("error");
	Diagnostic.Line = 6;
	Diagnostic.Column = 5;
	Diagnostic.Message = TEXT("unknown binding 'teleport_actor'");
	InvocationResult.Report.Diagnostics.Add(MoveTemp(Diagnostic));
	InvocationResult.ErrorCategory = TEXT("frontend_failed");
}

FString GetAvidScriptCompileServiceMissingManifestPath()
{
	FString ManifestPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptEditorTests"),
		TEXT("CompileService"),
		TEXT("missing_manifest.avidscript.json"));
	ManifestPath = FPaths::ConvertRelativePathToFull(ManifestPath);
	FPaths::NormalizeFilename(ManifestPath);
	IFileManager::Get().Delete(*ManifestPath);
	return ManifestPath;
}

FString GetAvidScriptCompileServiceExistingManifestPath()
{
	FString ManifestPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptGenerated"),
		TEXT("actor_set_location"),
		TEXT("actor_set_location.avidscript.json"));
	ManifestPath = FPaths::ConvertRelativePathToFull(ManifestPath);
	FPaths::NormalizeFilename(ManifestPath);
	return ManifestPath;
}

void AddAvidScriptCompileServiceManifestOutput(
	FAvidScriptFrontendInvocationResult& InvocationResult,
	const FString& ManifestPath)
{
	InvocationResult.Report.RawOutput.Add(FString::Printf(
		TEXT("[AvidScript][Frontend][Build] manifest=%s sha256=fixture"),
		*ManifestPath));
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCompileServiceGeneratedOnlyGateTest,
	"AvidScript.Editor.CompileService.GeneratedOnlyGateSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCompileServiceGeneratedOnlyGateTest::RunTest(const FString& Parameters)
{
	const FAvidScriptFrontendInvocationResult InvocationResult =
		MakeAvidScriptCompileServiceInvocation(true, true, 0, TEXT("generated"));

	FAvidScriptEditorCompileResult CompileResult;
	TestTrue(
		TEXT("Generated-only report succeeds"),
		FAvidScriptEditorCompileService::EvaluateInvocationResult(InvocationResult, CompileResult));
	TestTrue(TEXT("Compile result succeeded"), CompileResult.bSucceeded);
	TestFalse(TEXT("Generated-only result is not reloadable"), CompileResult.bReloadable);
	TestFalse(TEXT("Generated-only does not attempt manifest load"), CompileResult.bManifestLoadAttempted);
	TestEqual(
		TEXT("Generated-only status"),
		CompileResult.Status,
		EAvidScriptEditorCompileStatus::SucceededGeneratedOnly);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCompileServiceDiagnosticGateTest,
	"AvidScript.Editor.CompileService.DiagnosticGateSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCompileServiceDiagnosticGateTest::RunTest(const FString& Parameters)
{
	FAvidScriptFrontendInvocationResult InvocationResult =
		MakeAvidScriptCompileServiceInvocation(false, false, 1, TEXT("unknown_binding"));
	AddAvidScriptCompileServiceDiagnostic(InvocationResult);

	FAvidScriptEditorCompileResult CompileResult;
	TestFalse(
		TEXT("Diagnostic report fails compile gate"),
		FAvidScriptEditorCompileService::EvaluateInvocationResult(InvocationResult, CompileResult));
	TestFalse(TEXT("Compile result did not succeed"), CompileResult.bSucceeded);
	TestFalse(TEXT("Diagnostic result is not reloadable"), CompileResult.bReloadable);
	TestFalse(TEXT("Diagnostic result does not attempt manifest load"), CompileResult.bManifestLoadAttempted);
	TestEqual(TEXT("Diagnostic status"), CompileResult.Status, EAvidScriptEditorCompileStatus::FailedDiagnostics);
	TestEqual(TEXT("Diagnostic category"), CompileResult.ErrorCategory, FString(TEXT("frontend_diagnostics")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCompileServiceMissingManifestGateTest,
	"AvidScript.Editor.CompileService.MissingManifestGateSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCompileServiceMissingManifestGateTest::RunTest(const FString& Parameters)
{
	FAvidScriptFrontendInvocationResult InvocationResult =
		MakeAvidScriptCompileServiceInvocation(true, true, 0, TEXT("built"));
	AddAvidScriptCompileServiceManifestOutput(InvocationResult, GetAvidScriptCompileServiceMissingManifestPath());

	AddExpectedErrorPlain(TEXT("AvidScript reload manifest load error"), EAutomationExpectedErrorFlags::Contains, 1);

	FAvidScriptEditorCompileResult CompileResult;
	TestFalse(
		TEXT("Missing manifest fails compile gate"),
		FAvidScriptEditorCompileService::EvaluateInvocationResult(InvocationResult, CompileResult));
	TestFalse(TEXT("Compile result did not succeed"), CompileResult.bSucceeded);
	TestFalse(TEXT("Missing manifest result is not reloadable"), CompileResult.bReloadable);
	TestTrue(TEXT("Missing manifest attempts manifest load"), CompileResult.bManifestLoadAttempted);
	TestEqual(TEXT("Missing manifest status"), CompileResult.Status, EAvidScriptEditorCompileStatus::FailedManifest);
	TestEqual(TEXT("Missing manifest category"), CompileResult.ErrorCategory, FString(TEXT("manifest_file_missing")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCompileServiceExistingBuiltManifestGateTest,
	"AvidScript.Editor.CompileService.ExistingBuiltManifestGateSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCompileServiceExistingBuiltManifestGateTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath = GetAvidScriptCompileServiceExistingManifestPath();
	if (!FPaths::FileExists(ManifestPath))
	{
		AddWarning(FString::Printf(
			TEXT("Built AvidScript manifest is missing; run InvokeAvidScriptFrontend.ps1 with LDC before relying on this smoke. missing=%s"),
			*ManifestPath));
		return true;
	}

	FAvidScriptFrontendInvocationResult InvocationResult =
		MakeAvidScriptCompileServiceInvocation(true, true, 0, TEXT("built"));
	AddAvidScriptCompileServiceManifestOutput(InvocationResult, ManifestPath);

	FAvidScriptEditorCompileResult CompileResult;
	TestTrue(
		TEXT("Existing built manifest passes compile gate"),
		FAvidScriptEditorCompileService::EvaluateInvocationResult(InvocationResult, CompileResult));
	TestTrue(TEXT("Compile result succeeded"), CompileResult.bSucceeded);
	TestTrue(TEXT("Built manifest result is reloadable"), CompileResult.bReloadable);
	TestTrue(TEXT("Built manifest attempts manifest load"), CompileResult.bManifestLoadAttempted);
	TestEqual(TEXT("Built manifest status"), CompileResult.Status, EAvidScriptEditorCompileStatus::SucceededReloadable);
	TestTrue(TEXT("Manifest load succeeded"), CompileResult.ManifestLoadResult.bSucceeded);
	TestFalse(TEXT("Loaded bytecode is non-empty"), CompileResult.Bytecode.IsEmpty());

	return true;
}

#endif
