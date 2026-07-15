#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpBuildService.h"

#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
FString NormalizeCSharpContractTestPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString MakeCSharpContractScript(const FString& Body)
{
	return FString(TEXT(
		"param(\n"
		"    [string]$DotNetPath, [string]$OutputRoot, [string]$Configuration,\n"
		"    [string]$SourcePath, [string]$ProjectPath, [string]$ModuleId,\n"
		"    [string]$ArtifactStem, [string]$ReportPath, [string]$ManifestPath)\n"
		"$ErrorActionPreference = 'Stop'\n")) + Body + TEXT("\nexit 0\n");
}

FAvidScriptEditorCSharpBuildConfig MakeCSharpContractConfig(
	const FString& TestRoot,
	const FString& CaseName,
	const FString& ScriptBody)
{
	const FString OutputRoot = NormalizeCSharpContractTestPath(FPaths::Combine(TestRoot, CaseName));
	IFileManager::Get().DeleteDirectory(*OutputRoot, false, true);
	IFileManager::Get().MakeDirectory(*OutputRoot, true);
	const FString BuildScriptPath = NormalizeCSharpContractTestPath(FPaths::Combine(OutputRoot, TEXT("BuildContract.ps1")));
	FFileHelper::SaveStringToFile(MakeCSharpContractScript(ScriptBody), *BuildScriptPath);

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath = BuildScriptPath;
	Config.SourcePath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleSourcePath();
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.OutputRoot = OutputRoot;
	Config.ModuleId = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleModuleId();
	Config.ArtifactStem = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleArtifactStem();
	Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(OutputRoot, Config.ArtifactStem);
	Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(OutputRoot, Config.ArtifactStem);
	return Config;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildSuccessContractTest,
	"AvidScript.Editor.CSharpBuildService.SuccessContractSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildSuccessContractTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeCSharpContractTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(), TEXT("AvidScriptTests"), TEXT("CSharpBuildSuccessContract")));
	TestTrue(TEXT("C# success contract test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString InvalidJsonBody = TEXT(
		"[System.IO.File]::WriteAllText($ReportPath, '{invalid')");
	const FAvidScriptEditorCSharpBuildConfig InvalidJsonConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("InvalidJson"), InvalidJsonBody);
	FAvidScriptEditorCSharpBuildResult InvalidJsonResult;
	TestFalse(TEXT("Exit 0 with invalid JSON fails"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(InvalidJsonConfig, InvalidJsonResult));
	TestEqual(TEXT("Invalid JSON category"), InvalidJsonResult.ErrorCategory, FString(TEXT("report_invalid")));

	const FString FailedReportBody = TEXT(
		"$Json = '{\"schema_version\":1,\"result\":\"phase42_binding_required\",\"succeeded\":false,"
		"\"diagnostics\":[{\"code\":\"ASBI4201\",\"severity\":\"error\",\"message\":\"bindings required\"}]}'\n"
		"[System.IO.File]::WriteAllText($ReportPath, $Json)");
	const FAvidScriptEditorCSharpBuildConfig FailedReportConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("FailedReport"), FailedReportBody);
	FAvidScriptEditorCSharpBuildResult FailedReportResult;
	TestFalse(TEXT("Exit 0 with failed structured report fails"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(FailedReportConfig, FailedReportResult));
	TestEqual(TEXT("Failed structured report category"),
		FailedReportResult.ErrorCategory, FString(TEXT("phase42_binding_required")));

	const FString SuccessJson = TEXT(
		"{\"schema_version\":1,\"result\":\"direct_abi_built\",\"succeeded\":true,\"diagnostics\":[]}");
	const FString MissingManifestBody = FString::Printf(TEXT(
		"[System.IO.File]::WriteAllText($ReportPath, '%s')"), *SuccessJson);
	const FAvidScriptEditorCSharpBuildConfig MissingManifestConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("MissingManifest"), MissingManifestBody);
	FAvidScriptEditorCSharpBuildResult MissingManifestResult;
	TestFalse(TEXT("Exit 0 without manifest fails"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(MissingManifestConfig, MissingManifestResult));
	TestEqual(TEXT("Missing manifest category"),
		MissingManifestResult.ErrorCategory, FString(TEXT("manifest_missing")));

	const FString MissingWasmBody = FString::Printf(TEXT(
		"[System.IO.File]::WriteAllText($ManifestPath, '{}')\n"
		"[System.IO.File]::WriteAllText($ReportPath, '%s')"), *SuccessJson);
	const FAvidScriptEditorCSharpBuildConfig MissingWasmConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("MissingWasm"), MissingWasmBody);
	FAvidScriptEditorCSharpBuildResult MissingWasmResult;
	TestFalse(TEXT("Exit 0 without WASM fails"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(MissingWasmConfig, MissingWasmResult));
	TestEqual(TEXT("Missing WASM category"),
		MissingWasmResult.ErrorCategory, FString(TEXT("wasm_missing")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
