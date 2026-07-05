#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorSettingsService.h"

#include "AvidScriptEditorModule.h"
#include "AvidScriptEditorSourceConfig.h"

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
FString GetAvidScriptEditorSettingsSampleSourcePath()
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

FString GetAvidScriptEditorSettingsTestRoot()
{
	FString TestRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptEditorTests"), TEXT("Settings"));
	TestRoot = FPaths::ConvertRelativePathToFull(TestRoot);
	FPaths::NormalizeFilename(TestRoot);
	return TestRoot;
}

FString NormalizeAvidScriptEditorSettingsTestPath(const FString& Path)
{
	FString Normalized = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Normalized);
	return Normalized;
}

bool MakeAvidScriptEditorSettingsBaseConfig(
	FAvidScriptEditorCommandLaunchConfig& OutConfig,
	FAvidScriptEditorSourceConfigResult& OutSourceResult)
{
	FAvidScriptEditorSourceConfigRequest Request;
	Request.SourcePath = GetAvidScriptEditorSettingsSampleSourcePath();
	return FAvidScriptEditorSourceConfigService::BuildLaunchConfig(Request, OutSourceResult)
		&& OutSourceResult.bSucceeded
		&& (OutConfig = OutSourceResult.LaunchConfig, true);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorSettingsEmptySettingsNoopTest,
	"AvidScript.Editor.Settings.EmptySettingsNoopSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorSettingsEmptySettingsNoopTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandLaunchConfig Config;
	FAvidScriptEditorSourceConfigResult SourceResult;
	TestTrue(TEXT("Base config builds"), MakeAvidScriptEditorSettingsBaseConfig(Config, SourceResult));

	const FAvidScriptEditorCommandLaunchConfig OriginalConfig = Config;
	const FAvidScriptEditorToolchainSettings EmptySettings;
	FAvidScriptEditorSettingsService::ApplySettings(EmptySettings, Config);

	TestEqual(TEXT("Source path unchanged"), Config.SourcePath, OriginalConfig.SourcePath);
	TestEqual(TEXT("Bindings path unchanged"), Config.BindingsPath, OriginalConfig.BindingsPath);
	TestEqual(TEXT("Output root unchanged"), Config.OutputRoot, OriginalConfig.OutputRoot);
	TestEqual(TEXT("Report path unchanged"), Config.ReportPath, OriginalConfig.ReportPath);
	TestTrue(TEXT("Ldc2 path stays empty"), Config.Ldc2Path.IsEmpty());
	TestTrue(TEXT("Toolchain root stays empty"), Config.ToolchainRoot.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorSettingsToolchainApplyTest,
	"AvidScript.Editor.Settings.ToolchainApplySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorSettingsToolchainApplyTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandLaunchConfig Config;
	FAvidScriptEditorSourceConfigResult SourceResult;
	TestTrue(TEXT("Base config builds"), MakeAvidScriptEditorSettingsBaseConfig(Config, SourceResult));

	FAvidScriptEditorToolchainSettings Settings;
	Settings.ToolchainRoot = NormalizeAvidScriptEditorSettingsTestPath(FPaths::Combine(GetAvidScriptEditorSettingsTestRoot(), TEXT("ToolchainRoot")));
	Settings.Ldc2Path = NormalizeAvidScriptEditorSettingsTestPath(FPaths::Combine(Settings.ToolchainRoot, TEXT("bin"), TEXT("ldc2.exe")));

	FAvidScriptEditorSettingsService::ApplySettings(Settings, Config);

	TestEqual(TEXT("Toolchain root applied"), Config.ToolchainRoot, Settings.ToolchainRoot);
	TestEqual(TEXT("Ldc2 path applied"), Config.Ldc2Path, Settings.Ldc2Path);
	TestEqual(TEXT("Source path still unchanged"), Config.SourcePath, SourceResult.NormalizedSourcePath);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorSettingsOutputRootsApplyTest,
	"AvidScript.Editor.Settings.OutputRootsApplySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorSettingsOutputRootsApplyTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandLaunchConfig Config;
	FAvidScriptEditorSourceConfigResult SourceResult;
	TestTrue(TEXT("Base config builds"), MakeAvidScriptEditorSettingsBaseConfig(Config, SourceResult));

	FAvidScriptEditorToolchainSettings Settings;
	Settings.OutputRoot = NormalizeAvidScriptEditorSettingsTestPath(FPaths::Combine(GetAvidScriptEditorSettingsTestRoot(), TEXT("GeneratedRoot")));
	Settings.ReportRoot = NormalizeAvidScriptEditorSettingsTestPath(FPaths::Combine(GetAvidScriptEditorSettingsTestRoot(), TEXT("ReportRoot")));

	FAvidScriptEditorSettingsService::ApplySettings(Settings, Config);

	TestEqual(TEXT("Output root uses settings root plus source id"), Config.OutputRoot, FPaths::Combine(Settings.OutputRoot, SourceResult.SourceId));
	TestEqual(TEXT("Report path uses settings root plus source id"), Config.ReportPath, FPaths::Combine(Settings.ReportRoot, SourceResult.SourceId + TEXT(".frontend.report.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorSettingsModuleCommandConfigTest,
	"AvidScript.Editor.Settings.ModuleCommandConfigSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorSettingsModuleCommandConfigTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorToolchainSettings Settings;
	Settings.OutputRoot = NormalizeAvidScriptEditorSettingsTestPath(FPaths::Combine(GetAvidScriptEditorSettingsTestRoot(), TEXT("ModuleGeneratedRoot")));
	Settings.ReportRoot = NormalizeAvidScriptEditorSettingsTestPath(FPaths::Combine(GetAvidScriptEditorSettingsTestRoot(), TEXT("ModuleReportRoot")));

	FAvidScriptEditorCommandLaunchConfig Config;
	FString ErrorMessage;
	TestTrue(
		TEXT("Module generic command config builds"),
		FAvidScriptEditorModule::MakeCommandConfigForSource(
			GetAvidScriptEditorSettingsSampleSourcePath(),
			Settings,
			Config,
			ErrorMessage));
	TestTrue(TEXT("No error on module config success"), ErrorMessage.IsEmpty());
	TestEqual(TEXT("Module config output root"), Config.OutputRoot, FPaths::Combine(Settings.OutputRoot, TEXT("actor_set_location")));
	TestEqual(TEXT("Module config report path"), Config.ReportPath, FPaths::Combine(Settings.ReportRoot, TEXT("actor_set_location.frontend.report.json")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
