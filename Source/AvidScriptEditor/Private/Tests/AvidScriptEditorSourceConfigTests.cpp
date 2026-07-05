#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorSourceConfig.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
FString GetAvidScriptEditorSourceConfigSampleSourcePath()
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

FString GetAvidScriptEditorSourceConfigTestRoot()
{
	FString TestRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptEditorTests"), TEXT("SourceConfig"));
	TestRoot = FPaths::ConvertRelativePathToFull(TestRoot);
	FPaths::NormalizeFilename(TestRoot);
	return TestRoot;
}

FString NormalizeAvidScriptEditorSourceConfigTestPath(const FString& Path)
{
	FString Normalized = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Normalized);
	return Normalized;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorSourceConfigValidSourceTest,
	"AvidScript.Editor.SourceConfig.ValidSourceSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorSourceConfigValidSourceTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorSourceConfigRequest Request;
	Request.SourcePath = GetAvidScriptEditorSourceConfigSampleSourcePath();

	FAvidScriptEditorSourceConfigResult Result;
	TestTrue(TEXT("Valid source config builds"), FAvidScriptEditorSourceConfigService::BuildLaunchConfig(Request, Result));
	TestTrue(TEXT("Result succeeds"), Result.bSucceeded);
	TestTrue(TEXT("Error category is empty"), Result.ErrorCategory.IsEmpty());
	TestEqual(TEXT("Normalized source path"), Result.NormalizedSourcePath, Request.SourcePath);
	TestEqual(TEXT("Source id"), Result.SourceId, FString(TEXT("actor_set_location")));
	TestEqual(TEXT("Launch source path"), Result.LaunchConfig.SourcePath, Request.SourcePath);
	TestTrue(TEXT("Default bindings path"), Result.LaunchConfig.BindingsPath.EndsWith(TEXT("Bindings/ActorHostBindings.avidscript.json")));
	TestTrue(TEXT("Default output root"), Result.LaunchConfig.OutputRoot.EndsWith(TEXT("Saved/AvidScriptGenerated/actor_set_location")));
	TestTrue(TEXT("Default report path"), Result.LaunchConfig.ReportPath.EndsWith(TEXT("Saved/AvidScriptReports/actor_set_location.frontend.report.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorSourceConfigMissingSourceTest,
	"AvidScript.Editor.SourceConfig.MissingSourceFailsSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorSourceConfigMissingSourceTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorSourceConfigRequest Request;
	Request.SourcePath = NormalizeAvidScriptEditorSourceConfigTestPath(FPaths::Combine(
		GetAvidScriptEditorSourceConfigTestRoot(),
		TEXT("missing_source.avid")));

	FAvidScriptEditorSourceConfigResult Result;
	TestFalse(TEXT("Missing source config fails"), FAvidScriptEditorSourceConfigService::BuildLaunchConfig(Request, Result));
	TestFalse(TEXT("Result does not succeed"), Result.bSucceeded);
	TestEqual(TEXT("Error category"), Result.ErrorCategory, FString(TEXT("source_missing")));
	TestTrue(TEXT("Error message includes missing source"), Result.ErrorMessage.Contains(TEXT("does not exist")));
	TestTrue(TEXT("Next action is actionable"), Result.NextAction.Contains(TEXT("choose")));
	TestEqual(TEXT("Normalized source is still returned"), Result.NormalizedSourcePath, Request.SourcePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorSourceConfigNonAvidSourceTest,
	"AvidScript.Editor.SourceConfig.NonAvidSourceFailsSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorSourceConfigNonAvidSourceTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorSourceConfigRequest Request;
	Request.SourcePath = NormalizeAvidScriptEditorSourceConfigTestPath(FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Config"),
		TEXT("DefaultEngine.ini")));

	FAvidScriptEditorSourceConfigResult Result;
	TestFalse(TEXT("Non-.avid source config fails"), FAvidScriptEditorSourceConfigService::BuildLaunchConfig(Request, Result));
	TestFalse(TEXT("Result does not succeed"), Result.bSucceeded);
	TestEqual(TEXT("Error category"), Result.ErrorCategory, FString(TEXT("source_not_avid")));
	TestTrue(TEXT("Next action mentions .avid"), Result.NextAction.Contains(TEXT(".avid")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorSourceConfigCustomPathsTest,
	"AvidScript.Editor.SourceConfig.CustomPathsSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorSourceConfigCustomPathsTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = GetAvidScriptEditorSourceConfigTestRoot();

	FAvidScriptEditorSourceConfigRequest Request;
	Request.SourcePath = GetAvidScriptEditorSourceConfigSampleSourcePath();
	Request.BindingsPath = NormalizeAvidScriptEditorSourceConfigTestPath(FPaths::Combine(TestRoot, TEXT("CustomBindings.avidscript.json")));
	Request.OutputRoot = NormalizeAvidScriptEditorSourceConfigTestPath(FPaths::Combine(TestRoot, TEXT("Generated"), TEXT("CustomActor")));
	Request.ReportPath = NormalizeAvidScriptEditorSourceConfigTestPath(FPaths::Combine(TestRoot, TEXT("Reports"), TEXT("custom.report.json")));

	FAvidScriptEditorSourceConfigResult Result;
	TestTrue(TEXT("Custom path source config builds"), FAvidScriptEditorSourceConfigService::BuildLaunchConfig(Request, Result));
	TestTrue(TEXT("Result succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Custom bindings path"), Result.LaunchConfig.BindingsPath, Request.BindingsPath);
	TestEqual(TEXT("Custom output root"), Result.LaunchConfig.OutputRoot, Request.OutputRoot);
	TestEqual(TEXT("Custom report path"), Result.LaunchConfig.ReportPath, Request.ReportPath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
