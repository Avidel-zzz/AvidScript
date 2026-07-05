#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCommandService.h"

#include "Misc/AutomationTest.h"

namespace
{
const uint8 GAvidScriptEditorCommandServiceCompatibleWasmModule[] = {
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

FAvidScriptEditorCompileResult MakeAvidScriptEditorCommandServiceReloadableCompileResult(
	const FString& ModuleId)
{
	FAvidScriptEditorCompileResult CompileResult;
	CompileResult.bSucceeded = true;
	CompileResult.bReloadable = true;
	CompileResult.Status = EAvidScriptEditorCompileStatus::SucceededReloadable;
	CompileResult.Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(ModuleId);
	CompileResult.Bytecode.Append(
		GAvidScriptEditorCommandServiceCompatibleWasmModule,
		UE_ARRAY_COUNT(GAvidScriptEditorCommandServiceCompatibleWasmModule));
	return CompileResult;
}

FAvidScriptEditorCompileResult MakeAvidScriptEditorCommandServiceGeneratedOnlyCompileResult()
{
	FAvidScriptEditorCompileResult CompileResult;
	CompileResult.bSucceeded = true;
	CompileResult.bReloadable = false;
	CompileResult.Status = EAvidScriptEditorCompileStatus::SucceededGeneratedOnly;
	return CompileResult;
}

FAvidScriptEditorCompileResult MakeAvidScriptEditorCommandServiceFailedCompileResult()
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
	FAvidScriptEditorCommandServiceApplyEvaluatedReloadableTest,
	"AvidScript.Editor.CommandService.ApplyEvaluatedReloadableSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceApplyEvaluatedReloadableTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorCommandResult CommandResult;

	TestTrue(
		TEXT("Reloadable compile result applies through command facade"),
		FAvidScriptEditorCommandService::ApplyEvaluatedCompileResult(
			MakeAvidScriptEditorCommandServiceReloadableCompileResult(TEXT("command_reloadable")),
			Session,
			CommandResult));

	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::ReloadApplied);
	TestTrue(TEXT("Command marks reload applied"), CommandResult.bReloadApplied);
	TestTrue(TEXT("Session has live module"), Session.IsLiveLoaded());
	TestEqual(TEXT("Live module id"), Session.GetLiveModuleId(), FString(TEXT("command_reloadable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandServiceApplyGeneratedOnlyTest,
	"AvidScript.Editor.CommandService.ApplyGeneratedOnlySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceApplyGeneratedOnlyTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorCommandResult CommandResult;

	TestTrue(
		TEXT("Generated-only compile result succeeds without reload"),
		FAvidScriptEditorCommandService::ApplyEvaluatedCompileResult(
			MakeAvidScriptEditorCommandServiceGeneratedOnlyCompileResult(),
			Session,
			CommandResult));

	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::GeneratedOnly);
	TestFalse(TEXT("Generated-only does not apply reload"), CommandResult.bReloadApplied);
	TestFalse(TEXT("Generated-only keeps session unloaded"), Session.IsLiveLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandServiceApplyFailedCompileTest,
	"AvidScript.Editor.CommandService.ApplyFailedCompileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceApplyFailedCompileTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorCommandResult CommandResult;

	TestFalse(
		TEXT("Failed compile result fails command facade"),
		FAvidScriptEditorCommandService::ApplyEvaluatedCompileResult(
			MakeAvidScriptEditorCommandServiceFailedCompileResult(),
			Session,
			CommandResult));

	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::CompileFailed);
	TestEqual(TEXT("Command category"), CommandResult.ErrorCategory, FString(TEXT("frontend_diagnostics")));
	TestFalse(TEXT("Failed compile keeps session unloaded"), Session.IsLiveLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandServiceMissingReloadSessionTest,
	"AvidScript.Editor.CommandService.MissingReloadSessionSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceMissingReloadSessionTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandConfig Config;
	FAvidScriptEditorCommandResult CommandResult;

	TestFalse(
		TEXT("Missing reload session fails command facade"),
		FAvidScriptEditorCommandService::CompileAndApply(Config, CommandResult));

	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::CompileFailed);
	TestEqual(TEXT("Command category"), CommandResult.ErrorCategory, FString(TEXT("reload_session_missing")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
