#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptHash.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

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

FString GetCSharpContractZeroInvocationReportMetadataJson()
{
	return TEXT(
		"\"semantic_cache\":{\"schema_version\":1,\"enabled\":true,"
		"\"key\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
		"\"toolchain_fingerprint\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
		"\"lookup\":\"miss\",\"entry_report_file\":\"\",\"entry_report_sha256\":\"\","
		"\"published\":false,\"diagnostic_code\":\"\",\"diagnostic_message\":\"\"},"
		"\"tool_invocations\":{\"frontend\":0,\"semantic\":0,\"guest_ir\":0,\"wasm_backend\":0},");
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

bool LoadCSharpContractJsonObject(
	const FString& Path,
	TSharedPtr<FJsonObject>& OutObject)
{
	FString Json;
	if (!FFileHelper::LoadFileToString(Json, *Path))
	{
		return false;
	}
	return FJsonSerializer::Deserialize(
		TJsonReaderFactory<>::Create(Json),
		OutObject)
		&& OutObject.IsValid();
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
		"\"diagnostics\":[{\"code\":\"ASBI4201\",\"severity\":\"error\","
		"\"file\":\"Scripts/Profile.cs\",\"start\":42,\"length\":3,\"line\":6,\"column\":9,"
		"\"end_line\":6,\"end_column\":12,\"message\":\"bindings required\"}]}'\n"
		"[System.IO.File]::WriteAllText($ReportPath, $Json)"),
		*GetCSharpContractReportMetadataJson());
	const FAvidScriptEditorCSharpBuildConfig FailedReportConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("FailedReport"), FailedReportBody);
	FAvidScriptEditorCSharpBuildResult FailedReportResult;
	TestFalse(TEXT("Exit 0 with failed structured report fails"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(FailedReportConfig, FailedReportResult));
	TestEqual(TEXT("Failed structured report category"),
		FailedReportResult.ErrorCategory, FString(TEXT("phase42_binding_required")));
	TestEqual(
		TEXT("Failed structured report preserves source location"),
		FailedReportResult.ErrorMessage,
		FString(TEXT("Scripts/Profile.cs(7,10): ASBI4201: bindings required")));
	TestEqual(TEXT("Failed report preserves Frontend count"), FailedReportResult.FrontendInvocationCount, 1);
	TestEqual(TEXT("Failed report preserves Semantic count"), FailedReportResult.SemanticInvocationCount, 1);
	TestEqual(TEXT("Failed report preserves Guest IR count"), FailedReportResult.GuestIrInvocationCount, 1);
	TestEqual(TEXT("Failed report preserves WASM count"), FailedReportResult.WasmBackendInvocationCount, 1);
	TestEqual(TEXT("Failed report preserves cache lookup"), FailedReportResult.SemanticCacheLookup, FString(TEXT("miss")));

	const FString FrontendNotInvokedBody = FString::Printf(TEXT(
		"$Json = '{\"schema_version\":1,\"result\":\"frontend_failed\",\"succeeded\":false,%s"
		"\"diagnostics\":[{\"code\":\"ASCS1001\",\"severity\":\"error\",\"message\":\"frontend was not invoked\"}]}'\n"
		"[System.IO.File]::WriteAllText($ReportPath, $Json)"),
		*GetCSharpContractZeroInvocationReportMetadataJson());
	const FAvidScriptEditorCSharpBuildConfig FrontendNotInvokedConfig = MakeCSharpContractConfig(
		TestRoot, TEXT("FrontendNotInvoked"), FrontendNotInvokedBody);
	FAvidScriptEditorCSharpBuildResult FrontendNotInvokedResult;
	TestFalse(TEXT("Started process can report failure before Frontend invocation"),
		FAvidScriptEditorCSharpBuildService::BuildProfile(FrontendNotInvokedConfig, FrontendNotInvokedResult));
	TestEqual(TEXT("Pre-Frontend failure category"),
		FrontendNotInvokedResult.ErrorCategory, FString(TEXT("frontend_failed")));
	TestEqual(TEXT("Pre-Frontend failure records one build process"), FrontendNotInvokedResult.BuildInvocationCount, 1);
	TestEqual(TEXT("Pre-Frontend failure does not speculate Frontend"), FrontendNotInvokedResult.FrontendInvocationCount, 0);
	TestEqual(TEXT("Pre-Frontend failure does not speculate Semantic"), FrontendNotInvokedResult.SemanticInvocationCount, 0);
	TestEqual(TEXT("Pre-Frontend failure does not speculate Guest IR"), FrontendNotInvokedResult.GuestIrInvocationCount, 0);
	TestEqual(TEXT("Pre-Frontend failure does not speculate WASM"), FrontendNotInvokedResult.WasmBackendInvocationCount, 0);

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

	const uint8 MalformedWasmBytes[] = { 0, 97, 115, 109 };
	const FString MalformedWasmSha256 =
		FAvidScriptHash::Sha256Hex(MakeArrayView(MalformedWasmBytes));
	const FString MalformedArtifactBody = FString::Printf(TEXT(
		"$WasmPath = Join-Path $OutputRoot ($ArtifactStem + '.wasm')\n"
		"[byte[]]$WasmBytes = @(0, 97, 115, 109)\n"
		"[System.IO.File]::WriteAllBytes($WasmPath, $WasmBytes)\n"
		"$Sha = '%s'\n"
		"$ManifestJson = '{\"schema_version\":1,\"wasm\":{\"file\":\"' + $ArtifactStem + '.wasm\",\"sha256\":\"' + $Sha + '\"},\"execution\":{\"format\":\"stale\"}}'\n"
		"[System.IO.File]::WriteAllText($ManifestPath, $ManifestJson)\n"
		"[System.IO.File]::WriteAllText($ReportPath, '%s')"),
		*MalformedWasmSha256,
		*SuccessJson);

	FAvidScriptEditorCSharpBuildConfig PreferConfig = MakeCSharpContractConfig(
		TestRoot,
		TEXT("PreferPrecompiledFallback"),
		MalformedArtifactBody);
	const FString PreferArtifactPath = FPaths::Combine(
		PreferConfig.OutputRoot,
		PreferConfig.ArtifactStem + TEXT(".wasmtime.cwasm"));
	TestTrue(
		TEXT("Prefer fallback stale artifact fixture writes"),
		FFileHelper::SaveStringToFile(
			TEXT("stale-precompiled-artifact"),
			*PreferArtifactPath));
	FAvidScriptEditorCSharpBuildResult PreferResult;
	const bool bPreferBuildSucceeded =
		FAvidScriptEditorCSharpBuildService::BuildProfile(
			PreferConfig,
			PreferResult);
	if (!bPreferBuildSucceeded)
	{
		AddInfo(FString::Printf(
			TEXT("P57.8 PreferPrecompiled diagnostic | exit=%d | category=%s | message=%s | stdout=%s | stderr=%s"),
			PreferResult.ProcessExitCode,
			*PreferResult.ErrorCategory,
			*PreferResult.ErrorMessage,
			*PreferResult.Stdout,
			*PreferResult.Stderr));
	}
	TestTrue(
		TEXT("PreferPrecompiled keeps a valid canonical build on precompile failure"),
		bPreferBuildSucceeded);
	TestTrue(
		TEXT("PreferPrecompiled fallback result succeeds"),
		PreferResult.bSucceeded);
	TestFalse(
		TEXT("PreferPrecompiled fallback does not publish serialized bytes"),
		PreferResult.bVmArtifactPublished);
	TestEqual(
		TEXT("PreferPrecompiled records the compiler failure category"),
		PreferResult.VmArtifactFallbackCategory,
		FString(TEXT("artifact_compile_failed")));
	TestEqual(
		TEXT("PreferPrecompiled selects Wasmtime JIT"),
		PreferResult.VmArtifactSelectedBackend,
		FString(TEXT("wasmtime.cranelift.jit")));
	TestFalse(
		TEXT("PreferPrecompiled removes stale cwasm on fallback"),
		FPaths::FileExists(PreferArtifactPath));
	TSharedPtr<FJsonObject> PreferManifest;
	TestTrue(
		TEXT("PreferPrecompiled fallback manifest remains valid"),
		LoadCSharpContractJsonObject(
			PreferConfig.ManifestPath,
			PreferManifest));
	if (PreferManifest.IsValid())
	{
		TestFalse(
			TEXT("PreferPrecompiled fallback removes stale execution metadata"),
			PreferManifest->HasField(TEXT("execution")));
	}

	FAvidScriptEditorCSharpBuildConfig RequireConfig = MakeCSharpContractConfig(
		TestRoot,
		TEXT("RequirePrecompiledRollback"),
		MalformedArtifactBody);
	RequireConfig.VmArtifactPolicy =
		EAvidScriptEditorVmArtifactPolicy::RequirePrecompiled;
	const FString RequireWasmPath = FPaths::Combine(
		RequireConfig.OutputRoot,
		RequireConfig.ArtifactStem + TEXT(".wasm"));
	const FString RequireArtifactPath = FPaths::Combine(
		RequireConfig.OutputRoot,
		RequireConfig.ArtifactStem + TEXT(".wasmtime.cwasm"));
	const FString PreviousReport = TEXT("previous-report");
	const FString PreviousManifest = TEXT("previous-manifest");
	const FString PreviousWasm = TEXT("previous-wasm");
	const FString PreviousArtifact = TEXT("previous-cwasm");
	TestTrue(
		TEXT("Require rollback previous report writes"),
		FFileHelper::SaveStringToFile(
			PreviousReport,
			*RequireConfig.ReportPath));
	TestTrue(
		TEXT("Require rollback previous manifest writes"),
		FFileHelper::SaveStringToFile(
			PreviousManifest,
			*RequireConfig.ManifestPath));
	TestTrue(
		TEXT("Require rollback previous WASM writes"),
		FFileHelper::SaveStringToFile(PreviousWasm, *RequireWasmPath));
	TestTrue(
		TEXT("Require rollback previous cwasm writes"),
		FFileHelper::SaveStringToFile(
			PreviousArtifact,
			*RequireArtifactPath));
	FAvidScriptEditorCSharpBuildResult RequireResult;
	const bool bRequireBuildSucceeded =
		FAvidScriptEditorCSharpBuildService::BuildProfile(
			RequireConfig,
			RequireResult);
	if (bRequireBuildSucceeded
		|| RequireResult.ErrorCategory != TEXT("vm_artifact_compile_failed"))
	{
		AddInfo(FString::Printf(
			TEXT("P57.8 RequirePrecompiled diagnostic | succeeded=%d | exit=%d | category=%s | message=%s | stdout=%s | stderr=%s"),
			bRequireBuildSucceeded ? 1 : 0,
			RequireResult.ProcessExitCode,
			*RequireResult.ErrorCategory,
			*RequireResult.ErrorMessage,
			*RequireResult.Stdout,
			*RequireResult.Stderr));
	}
	TestFalse(
		TEXT("RequirePrecompiled fails when artifact compilation fails"),
		bRequireBuildSucceeded);
	TestEqual(
		TEXT("RequirePrecompiled exposes a stable failure category"),
		RequireResult.ErrorCategory,
		FString(TEXT("vm_artifact_compile_failed")));
	FString RestoredText;
	TestTrue(
		TEXT("Require rollback report can be read"),
		FFileHelper::LoadFileToString(
			RestoredText,
			*RequireConfig.ReportPath));
	TestEqual(TEXT("Require rollback restores report"), RestoredText, PreviousReport);
	TestTrue(
		TEXT("Require rollback manifest can be read"),
		FFileHelper::LoadFileToString(
			RestoredText,
			*RequireConfig.ManifestPath));
	TestEqual(
		TEXT("Require rollback restores manifest"),
		RestoredText,
		PreviousManifest);
	TestTrue(
		TEXT("Require rollback WASM can be read"),
		FFileHelper::LoadFileToString(RestoredText, *RequireWasmPath));
	TestEqual(TEXT("Require rollback restores WASM"), RestoredText, PreviousWasm);
	TestTrue(
		TEXT("Require rollback cwasm can be read"),
		FFileHelper::LoadFileToString(RestoredText, *RequireArtifactPath));
	TestEqual(
		TEXT("Require rollback restores cwasm"),
		RestoredText,
		PreviousArtifact);

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
