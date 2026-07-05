#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptFrontendInvoker.h"

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString GetAvidScriptInvokerTestRoot()
{
	FString TestRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptEditorTests"), TEXT("Invoker"));
	TestRoot = FPaths::ConvertRelativePathToFull(TestRoot);
	FPaths::NormalizeFilename(TestRoot);
	return TestRoot;
}

FString GetAvidScriptInvokerReportPath(const TCHAR* FileName)
{
	return FPaths::Combine(GetAvidScriptInvokerTestRoot(), FileName);
}

FString GetAvidScriptInvokerSampleSourcePath()
{
	FString SourcePath = FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins"),
		TEXT("AvidScript"),
		TEXT("Samples"),
		TEXT("AvidScript"),
		TEXT("ActorSetLocation"),
		TEXT("actor_set_location.avid"));
	SourcePath = FPaths::ConvertRelativePathToFull(SourcePath);
	FPaths::NormalizeFilename(SourcePath);
	return SourcePath;
}

FString WriteAvidScriptInvokerUnknownBindingSource()
{
	const FString SourcePath = FPaths::Combine(GetAvidScriptInvokerTestRoot(), TEXT("unknown_binding.avid"));
	const FString SourceText = TEXT("module p9_invoker_bad_unknown\n\n")
		TEXT("use actor_set_location\n\n")
		TEXT("on begin_play {\n")
		TEXT("    teleport_actor(1, 1, 123.0, 456.0, 789.0)\n")
		TEXT("}\n\n")
		TEXT("on tick(delta_seconds) {\n")
		TEXT("}\n");

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourcePath), true);
	if (!FFileHelper::SaveStringToFile(SourceText, *SourcePath))
	{
		return FString();
	}

	return SourcePath;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFrontendInvokerSkipCompileSuccessTest,
	"AvidScript.Editor.Invoker.SkipCompileSuccessSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFrontendInvokerSkipCompileSuccessTest::RunTest(const FString& Parameters)
{
	FAvidScriptFrontendInvocationConfig Config;
	Config.SourcePath = GetAvidScriptInvokerSampleSourcePath();
	Config.ReportPath = GetAvidScriptInvokerReportPath(TEXT("success.report.json"));
	Config.bSkipCompile = true;

	FAvidScriptFrontendInvocationResult Result;
	TestTrue(TEXT("Invoker succeeds for skip-compile sample"), FAvidScriptFrontendInvoker::Invoke(Config, Result));
	TestTrue(TEXT("Invocation result succeeded"), Result.bSucceeded);
	TestEqual(TEXT("Process exit code"), Result.ProcessExitCode, 0);
	TestTrue(TEXT("Report load succeeded"), Result.ReportLoadResult.bSucceeded);
	TestTrue(TEXT("Frontend report succeeded"), Result.Report.bSucceeded);
	TestFalse(TEXT("No error diagnostics"), Result.Report.HasErrorDiagnostics());

	const FAvidScriptFrontendBuildEvent* LastEvent = Result.Report.GetLastBuildEvent();
	TestNotNull(TEXT("Last build event exists"), LastEvent);
	if (LastEvent != nullptr)
	{
		TestEqual(TEXT("Last build result"), LastEvent->Result, FString(TEXT("generated")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptFrontendInvokerUnknownBindingTest,
	"AvidScript.Editor.Invoker.UnknownBindingSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptFrontendInvokerUnknownBindingTest::RunTest(const FString& Parameters)
{
	const FString UnknownBindingSourcePath = WriteAvidScriptInvokerUnknownBindingSource();
	TestFalse(TEXT("Unknown binding source writes"), UnknownBindingSourcePath.IsEmpty());
	if (UnknownBindingSourcePath.IsEmpty())
	{
		return true;
	}

	FAvidScriptFrontendInvocationConfig Config;
	Config.SourcePath = UnknownBindingSourcePath;
	Config.ReportPath = GetAvidScriptInvokerReportPath(TEXT("unknown_binding.report.json"));
	Config.bSkipCompile = true;

	FAvidScriptFrontendInvocationResult Result;
	TestFalse(TEXT("Invoker fails for unknown binding"), FAvidScriptFrontendInvoker::Invoke(Config, Result));
	TestFalse(TEXT("Invocation result did not succeed"), Result.bSucceeded);
	TestEqual(TEXT("Process exit code"), Result.ProcessExitCode, 1);
	TestEqual(TEXT("Invocation category"), Result.ErrorCategory, FString(TEXT("frontend_failed")));
	TestTrue(TEXT("Report load succeeded"), Result.ReportLoadResult.bSucceeded);
	TestFalse(TEXT("Frontend report did not succeed"), Result.Report.bSucceeded);
	TestTrue(TEXT("Error diagnostics detected"), Result.Report.HasErrorDiagnostics());
	TestEqual(TEXT("Diagnostic count"), Result.Report.Diagnostics.Num(), 1);

	if (Result.Report.Diagnostics.Num() == 1)
	{
		const FAvidScriptFrontendDiagnostic& Diagnostic = Result.Report.Diagnostics[0];
		TestEqual(TEXT("Diagnostic code"), Diagnostic.Code, FString(TEXT("ASL1202")));
		TestEqual(TEXT("Diagnostic line"), Diagnostic.Line, 6);
		TestEqual(TEXT("Diagnostic column"), Diagnostic.Column, 5);
	}

	return true;
}

#endif