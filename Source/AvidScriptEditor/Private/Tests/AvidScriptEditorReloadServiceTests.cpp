#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCompileService.h"
#include "AvidScriptEditorReloadService.h"

#include "Misc/AutomationTest.h"

namespace
{
const uint8 GAvidScriptEditorReloadServiceCompatibleWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x07,
	0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

FAvidScriptEditorCompileResult MakeAvidScriptEditorReloadServiceCompileResult(
	const FString& ModuleId,
	const bool bReloadable)
{
	FAvidScriptEditorCompileResult CompileResult;
	CompileResult.bSucceeded = true;
	CompileResult.bReloadable = bReloadable;
	CompileResult.Status = bReloadable
		? EAvidScriptEditorCompileStatus::SucceededReloadable
		: EAvidScriptEditorCompileStatus::SucceededGeneratedOnly;
	CompileResult.Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(ModuleId);
	if (bReloadable)
	{
		CompileResult.Bytecode.Append(
			GAvidScriptEditorReloadServiceCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptEditorReloadServiceCompatibleWasmModule));
	}
	return CompileResult;
}

FAvidScriptEditorCompileResult MakeAvidScriptEditorReloadServiceFailedCompileResult()
{
	FAvidScriptEditorCompileResult CompileResult;
	CompileResult.bSucceeded = false;
	CompileResult.bReloadable = false;
	CompileResult.Status = EAvidScriptEditorCompileStatus::FailedDiagnostics;
	CompileResult.ErrorCategory = TEXT("frontend_diagnostics");
	CompileResult.ErrorMessage = TEXT("Unknown binding actor.teleport");
	CompileResult.NextAction = TEXT("fix the frontend diagnostic before reloading");
	return CompileResult;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorReloadServiceGeneratedOnlySkipsReloadTest,
	"AvidScript.Editor.ReloadService.GeneratedOnlySkipsReloadSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorReloadServiceGeneratedOnlySkipsReloadTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorReloadApplyResult ApplyResult;

	TestTrue(
		TEXT("Generated-only compile result is handled successfully"),
		FAvidScriptEditorReloadService::ApplyCompileResult(
			MakeAvidScriptEditorReloadServiceCompileResult(TEXT("generated_only"), false),
			Session,
			ApplyResult));

	TestEqual(TEXT("Generated-only status"), ApplyResult.Status, EAvidScriptEditorReloadApplyStatus::SkippedGeneratedOnly);
	TestFalse(TEXT("Generated-only does not apply reload"), ApplyResult.bApplied);
	TestFalse(TEXT("Generated-only leaves session unloaded"), Session.IsLiveLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorReloadServiceInitialLoadAppliesTest,
	"AvidScript.Editor.ReloadService.InitialLoadAppliesSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorReloadServiceInitialLoadAppliesTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorReloadApplyResult ApplyResult;

	TestTrue(
		TEXT("Reloadable compile result applies as initial load"),
		FAvidScriptEditorReloadService::ApplyCompileResult(
			MakeAvidScriptEditorReloadServiceCompileResult(TEXT("initial_load"), true),
			Session,
			ApplyResult));

	TestEqual(TEXT("Initial load status"), ApplyResult.Status, EAvidScriptEditorReloadApplyStatus::AppliedInitialLoad);
	TestTrue(TEXT("Initial load is applied"), ApplyResult.bApplied);
	TestTrue(TEXT("Initial load flag"), ApplyResult.bInitialLoad);
	TestFalse(TEXT("Initial load is not reload flag"), ApplyResult.bReload);
	TestEqual(TEXT("Live module id"), Session.GetLiveModuleId(), FString(TEXT("initial_load")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorReloadServiceLiveReloadAppliesTest,
	"AvidScript.Editor.ReloadService.LiveReloadAppliesSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorReloadServiceLiveReloadAppliesTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorReloadApplyResult ApplyResult;

	TestTrue(
		TEXT("Initial compile result applies"),
		FAvidScriptEditorReloadService::ApplyCompileResult(
			MakeAvidScriptEditorReloadServiceCompileResult(TEXT("reload_v1"), true),
			Session,
			ApplyResult));

	TestTrue(
		TEXT("Second compile result reloads live session"),
		FAvidScriptEditorReloadService::ApplyCompileResult(
			MakeAvidScriptEditorReloadServiceCompileResult(TEXT("reload_v2"), true),
			Session,
			ApplyResult));

	TestEqual(TEXT("Reload status"), ApplyResult.Status, EAvidScriptEditorReloadApplyStatus::AppliedReload);
	TestTrue(TEXT("Reload is applied"), ApplyResult.bApplied);
	TestFalse(TEXT("Reload is not initial load flag"), ApplyResult.bInitialLoad);
	TestTrue(TEXT("Reload flag"), ApplyResult.bReload);
	TestEqual(TEXT("Live module id switches"), Session.GetLiveModuleId(), FString(TEXT("reload_v2")));
	TestEqual(TEXT("Successful reload count"), Session.GetSuccessfulReloadCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorReloadServiceFailedCompileRejectedTest,
	"AvidScript.Editor.ReloadService.FailedCompileRejectedSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorReloadServiceFailedCompileRejectedTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorReloadApplyResult ApplyResult;

	TestFalse(
		TEXT("Failed compile result is rejected"),
		FAvidScriptEditorReloadService::ApplyCompileResult(
			MakeAvidScriptEditorReloadServiceFailedCompileResult(),
			Session,
			ApplyResult));

	TestEqual(TEXT("Rejected status"), ApplyResult.Status, EAvidScriptEditorReloadApplyStatus::RejectedCompileResult);
	TestEqual(TEXT("Compile error category is copied"), ApplyResult.ErrorCategory, FString(TEXT("frontend_diagnostics")));
	TestFalse(TEXT("Rejected compile does not load runtime"), Session.IsLiveLoaded());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
