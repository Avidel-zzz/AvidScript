#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmReload.h"

#include "Misc/AutomationTest.h"

namespace
{
const uint8 GAvidScriptReloadCompatibleWasmModule[] = {
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

const uint8 GAvidScriptReloadMissingTickWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x16, 0x01, 0x12, 0x61, 0x76, 0x69, 0x64,
	0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69,
	0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00,
	0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadCompatibleSmokeTest,
	"AvidScript.Reload.CompatibleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadCompatibleSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;

	TestTrue(
		TEXT("Initial module loads"),
		Session.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v1")),
			Result));
	TestEqual(TEXT("Initial live module id"), Session.GetLiveModuleId(), FString(TEXT("reload_v1")));

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Initial live runtime ticks"), Session.TickLive(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Initial live tick count"), Session.GetLiveTickCallCount(), 1);

	TestTrue(
		TEXT("Compatible reload applies"),
		Session.ReloadModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v2")),
			Result));

	TestTrue(TEXT("Reload result reports applied"), Result.bReloadApplied);
	TestEqual(TEXT("Reload result previous module"), Result.PreviousModuleId, FString(TEXT("reload_v1")));
	TestEqual(TEXT("Reload result active module"), Result.ActiveModuleId, FString(TEXT("reload_v2")));
	TestEqual(TEXT("Live module id switches"), Session.GetLiveModuleId(), FString(TEXT("reload_v2")));
	TestEqual(TEXT("Successful reload count"), Session.GetSuccessfulReloadCount(), 1);

	TestTrue(TEXT("Reloaded live runtime ticks"), Session.TickLive(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Reloaded live tick count starts fresh"), Session.GetLiveTickCallCount(), 1);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadMissingExportRollbackSmokeTest,
	"AvidScript.Reload.MissingExportRollbackSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadMissingExportRollbackSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;

	TestTrue(
		TEXT("Initial module loads"),
		Session.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v1")),
			Result));

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Initial live runtime ticks"), Session.TickLive(1.0f / 60.0f, TickResult));

	TestFalse(
		TEXT("Reload with missing tick export is rejected"),
		Session.ReloadModule(
			GAvidScriptReloadMissingTickWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadMissingTickWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_missing_tick")),
			Result));

	TestTrue(TEXT("Rollback preserved live runtime"), Result.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("Missing export category"), Result.ErrorCategory, FString(TEXT("missing_export")));
	TestEqual(TEXT("Missing export name"), Result.ExportName, FString(TEXT("avid_on_tick")));
	TestEqual(TEXT("Live module id stays on previous runtime"), Session.GetLiveModuleId(), FString(TEXT("reload_v1")));
	TestEqual(TEXT("Rejected reload count"), Session.GetRejectedReloadCount(), 1);

	TestTrue(TEXT("Previous live runtime still ticks"), Session.TickLive(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Previous live tick count continues"), Session.GetLiveTickCallCount(), 2);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptReloadAbiMismatchRollbackSmokeTest,
	"AvidScript.Reload.AbiMismatchRollbackSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptReloadAbiMismatchRollbackSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptWasmReloadResult Result;

	TestTrue(
		TEXT("Initial module loads"),
		Session.LoadInitialModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_v1")),
			Result));

	FAvidScriptWasmReloadManifest MismatchedManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_abi_v2"));
	MismatchedManifest.AbiVersion = FAvidScriptWasmReloadManifest::SupportedAbiVersion + 1;

	TestFalse(
		TEXT("Reload with ABI mismatch is rejected"),
		Session.ReloadModule(
			GAvidScriptReloadCompatibleWasmModule,
			UE_ARRAY_COUNT(GAvidScriptReloadCompatibleWasmModule),
			MismatchedManifest,
			Result));

	TestTrue(TEXT("Rollback preserved live runtime"), Result.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("ABI mismatch category"), Result.ErrorCategory, FString(TEXT("abi_mismatch")));
	TestEqual(TEXT("Live module id stays on previous runtime"), Session.GetLiveModuleId(), FString(TEXT("reload_v1")));
	TestEqual(TEXT("Rejected reload count"), Session.GetRejectedReloadCount(), 1);

	FAvidScriptWasmSmokeResult TickResult;
	TestTrue(TEXT("Previous live runtime still ticks"), Session.TickLive(1.0f / 60.0f, TickResult));
	TestEqual(TEXT("Previous live tick count"), Session.GetLiveTickCallCount(), 1);

	return true;
}

#endif
