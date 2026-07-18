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
		"    [string]$ArtifactStem, [string]$ReportPath, [string]$ManifestPath,\n"
		"    [string]$PreparedBuildReportPath, [string]$SemanticCacheRoot,\n"
		"    [string]$BindingPackagePath, [string]$RuntimeBindingPackagePath,\n"
		"    [switch]$OmitRuntimeBindingPackage, [switch]$DisableSemanticCache)\n"
		"$ErrorActionPreference = 'Stop'\n")) + Body + TEXT("\nexit 0\n");
}

FString GetCSharpContractReportMetadataJson()
{
	return TEXT(
		"\"semantic_cache\":{\"schema_version\":1,\"enabled\":true,"
		"\"key\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
		"\"toolchain_fingerprint\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
		"\"lookup\":\"miss\",\"entry_report_file\":\"\",\"entry_report_sha256\":\"\","
		"\"published\":false,\"diagnostic_code\":\"\",\"diagnostic_message\":\"\"},"
		"\"tool_invocations\":{\"frontend\":1,\"semantic\":1,\"guest_ir\":1,\"wasm_backend\":1},");
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
	Config.SemanticCacheRoot = NormalizeCSharpContractTestPath(FPaths::Combine(OutputRoot, TEXT("SemanticCache")));
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

	const FString FailedReportBody = FString::Printf(TEXT(
		"$Json = '{\"schema_version\":1,\"result\":\"phase42_binding_required\",\"succeeded\":false,%s"
		"\"diagnostics\":[{\"code\":\"ASBI4201\",\"severity\":\"error\",\"message\":\"bindings required\"}]}'\n"
		"[System.IO.File]::WriteAllText($ReportPath, $Json)"),
		*GetCSharpContractReportMetadataJson());
	const FAvidScriptEditorCSharpBuildConfig FailedReportConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("FailedReport"), FailedReportBody);
	FAvidScriptEditorCSharpBuildResult FailedReportResult;
	TestFalse(TEXT("Exit 0 with failed structured report fails"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(FailedReportConfig, FailedReportResult));
	TestEqual(TEXT("Failed structured report category"),
		FailedReportResult.ErrorCategory, FString(TEXT("phase42_binding_required")));
	TestEqual(TEXT("Failed report preserves Frontend count"), FailedReportResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Failed report preserves Semantic count"), FailedReportResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Failed report preserves Guest IR count"), FailedReportResult.GuestIrInvocationCount, 1);
	TestEqual(TEXT("Failed report preserves WASM count"), FailedReportResult.WasmBackendInvocationCount, 1);
	TestEqual(TEXT("Failed report preserves cache lookup"), FailedReportResult.SemanticCacheLookup, FString(TEXT("miss")));

	const FString SuccessJson = FString::Printf(TEXT(
		"{\"schema_version\":1,\"result\":\"direct_abi_built\",\"succeeded\":true,%s\"diagnostics\":[]}"),
		*GetCSharpContractReportMetadataJson());
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

	const FString MissingMetadataJson = TEXT(
		"{\"schema_version\":1,\"result\":\"direct_abi_built\",\"succeeded\":true,\"diagnostics\":[]}");
	const FString MissingMetadataBody = FString::Printf(TEXT(
		"[System.IO.File]::WriteAllText($ReportPath, '%s')"), *MissingMetadataJson);
	const FAvidScriptEditorCSharpBuildConfig MissingMetadataConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("MissingMetadata"), MissingMetadataBody);
	FAvidScriptEditorCSharpBuildResult MissingMetadataResult;
	TestFalse(TEXT("Missing invocation/cache metadata fails"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(MissingMetadataConfig, MissingMetadataResult));
	TestEqual(TEXT("Missing metadata category"),
		MissingMetadataResult.ErrorCategory, FString(TEXT("report_contract_invalid")));
	TestEqual(TEXT("Missing metadata does not speculate Frontend count"), MissingMetadataResult.FrontendInvocationCount, 0);
	TestEqual(TEXT("Missing metadata does not speculate Semantic count"), MissingMetadataResult.SemanticInvocationCount, 0);

	const FString NegativeCountBody = TEXT(
		"$Json = '{\"schema_version\":1,\"result\":\"direct_abi_built\",\"succeeded\":true,"
		"\"semantic_cache\":{\"schema_version\":1,\"enabled\":false,\"key\":\"\",\"toolchain_fingerprint\":\"\",\"lookup\":\"disabled\",\"entry_report_file\":\"\",\"entry_report_sha256\":\"\",\"published\":false,\"diagnostic_code\":\"\",\"diagnostic_message\":\"\"},"
		"\"tool_invocations\":{\"frontend\":-1,\"semantic\":0,\"guest_ir\":0,\"wasm_backend\":0},\"diagnostics\":[]}'\n"
		"[System.IO.File]::WriteAllText($ReportPath, $Json)");
	const FAvidScriptEditorCSharpBuildConfig NegativeCountConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("NegativeCount"), NegativeCountBody);
	FAvidScriptEditorCSharpBuildResult NegativeCountResult;
	TestFalse(TEXT("Negative tool invocation count fails"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(NegativeCountConfig, NegativeCountResult));
	TestEqual(TEXT("Negative count category"),
		NegativeCountResult.ErrorCategory, FString(TEXT("report_contract_invalid")));

	FString UnknownLookupJson = SuccessJson;
	UnknownLookupJson.ReplaceInline(TEXT("\"lookup\":\"miss\""), TEXT("\"lookup\":\"warm\""));
	const FString UnknownLookupBody = FString::Printf(TEXT(
		"[System.IO.File]::WriteAllText($ReportPath, '%s')"), *UnknownLookupJson);
	const FAvidScriptEditorCSharpBuildConfig UnknownLookupConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("UnknownLookup"), UnknownLookupBody);
	FAvidScriptEditorCSharpBuildResult UnknownLookupResult;
	TestFalse(TEXT("Unknown semantic cache lookup fails"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(UnknownLookupConfig, UnknownLookupResult));
	TestEqual(TEXT("Unknown lookup category"),
		UnknownLookupResult.ErrorCategory, FString(TEXT("report_contract_invalid")));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
