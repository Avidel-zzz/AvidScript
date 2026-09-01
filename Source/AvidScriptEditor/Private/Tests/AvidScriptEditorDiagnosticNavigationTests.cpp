#include "AvidScriptEditorDiagnosticNavigation.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorDiagnosticNavigationResolveTest,
	"AvidScript.Editor.Diagnostics.NavigationResolve",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorDiagnosticNavigationResolveTest::RunTest(const FString& Parameters)
{
	const FString SourcePath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P61/A3/Navigation.cs"));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(SourcePath), true);
	TestTrue(TEXT("Diagnostic navigation fixture writes"), FFileHelper::SaveStringToFile(
		TEXT("public static class Navigation { }\n"),
		*SourcePath));

	FString SourceId = SourcePath;
	FPaths::MakePathRelativeTo(SourceId, *FPaths::ProjectDir());
	FPaths::NormalizeFilename(SourceId);
	FAvidScriptFrontendDiagnostic Diagnostic;
	Diagnostic.Code = TEXT("CS1002");
	Diagnostic.Severity = TEXT("error");
	Diagnostic.Stage = TEXT("semantic");
	Diagnostic.File = SourceId;
	Diagnostic.Start = 1;
	Diagnostic.Length = 1;
	Diagnostic.Line = 1;
	Diagnostic.Column = 8;
	Diagnostic.LineBase = 1;
	Diagnostic.SourceSha256 = TEXT("628b655847aedec3c0063b8bac1e7ae2c0a4e61245fa90441b3e0a4541868b44");

	FAvidScriptEditorDiagnosticNavigationResult Result;
	TestTrue(TEXT("Project-relative diagnostic resolves"),
		FAvidScriptEditorDiagnosticNavigation::Resolve(Diagnostic, FPaths::ProjectDir(), Result));
	TestTrue(TEXT("Resolved navigation succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Resolved source path"), Result.AbsoluteSourcePath, FPaths::ConvertRelativePathToFull(SourcePath));
	TestEqual(TEXT("Resolved line"), Result.Line, 1);
	TestEqual(TEXT("Resolved column"), Result.Column, 8);

	Diagnostic.SourceSha256 = FString::ChrN(64, TEXT('0'));
	TestFalse(TEXT("Changed source hash is rejected"),
		FAvidScriptEditorDiagnosticNavigation::Resolve(Diagnostic, FPaths::ProjectDir(), Result));
	TestEqual(TEXT("Changed source category"), Result.ErrorCategory, FString(TEXT("diagnostic_source_changed")));

	Diagnostic.SourceSha256.Reset();
	Diagnostic.File = TEXT("../Outside.cs");
	TestFalse(TEXT("Traversal source id is rejected"),
		FAvidScriptEditorDiagnosticNavigation::Resolve(Diagnostic, FPaths::ProjectDir(), Result));
	TestEqual(
		TEXT("Traversal category"),
		Result.ErrorCategory,
		FString(TEXT("diagnostic_source_outside_project")));

	IFileManager::Get().Delete(*SourcePath, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
