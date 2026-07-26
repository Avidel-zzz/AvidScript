#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"

#include "AvidScriptRuntimeBackendTestLanes.h"
#include "Misc/AutomationTest.h"

namespace
{
const uint8 GAvidScriptMissingImportWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x0d, 0x03, 0x60, 0x01, 0x7f, 0x01, 0x7f,
	0x60, 0x00, 0x00, 0x60, 0x01, 0x7d, 0x00,
	0x02, 0x1f, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64,
	0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x10, 0x68,
	0x6f, 0x73, 0x74, 0x5f, 0x6d, 0x69, 0x73, 0x73,
	0x69, 0x6e, 0x67, 0x5f, 0x69, 0x33, 0x32, 0x00,
	0x00, 0x03, 0x03, 0x02, 0x01, 0x02, 0x07, 0x25,
	0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f,
	0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e, 0x5f,
	0x70, 0x6c, 0x61, 0x79, 0x00, 0x01, 0x0c, 0x61,
	0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f, 0x74,
	0x69, 0x63, 0x6b, 0x00, 0x02, 0x0a, 0x0c, 0x02,
	0x07, 0x00, 0x41, 0x29, 0x10, 0x00, 0x1a, 0x0b,
	0x02, 0x00, 0x0b
};

const uint8 GAvidScriptFailingImportWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x0d, 0x03, 0x60, 0x01, 0x7f, 0x01, 0x7f,
	0x60, 0x00, 0x00, 0x60, 0x01, 0x7d, 0x00,
	0x02, 0x1c, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64,
	0x73, 0x63, 0x72, 0x69, 0x70, 0x74, 0x0d, 0x68,
	0x6f, 0x73, 0x74, 0x5f, 0x66, 0x61, 0x69, 0x6c,
	0x5f, 0x69, 0x33, 0x32, 0x00, 0x00, 0x03, 0x03,
	0x02, 0x01, 0x02, 0x07, 0x25, 0x02, 0x12, 0x61,
	0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f, 0x62,
	0x65, 0x67, 0x69, 0x6e, 0x5f, 0x70, 0x6c, 0x61,
	0x79, 0x00, 0x01, 0x0c, 0x61, 0x76, 0x69, 0x64,
	0x5f, 0x6f, 0x6e, 0x5f, 0x74, 0x69, 0x63, 0x6b,
	0x00, 0x02, 0x0a, 0x0c, 0x02, 0x07, 0x00, 0x41,
	0x29, 0x10, 0x00, 0x1a, 0x0b, 0x02, 0x00, 0x0b
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmImportBridgeSmokeTest,
	"AvidScript.Runtime.WasmImportBridgeSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmImportBridgeSmokeTest::RunTest(const FString& Parameters)
{
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		const bool bSucceeded =
			Runtime.LoadEmbeddedHostImportModule(Result) && Runtime.BeginPlay(Result);
		if (!bSucceeded)
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("WASM guest calls deterministic host import")), bSucceeded);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("BeginPlay succeeds after host import")), Result.bBeginPlayCalled);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("host import call count")), Result.HostImportCallCount, 1);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("host import input")), Result.LastHostImportInput, 41);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("host import result")), Result.LastHostImportResult, 42);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("host import timing is captured")), Result.Metrics.HostImportCallMs > 0.0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmMissingImportSmokeTest,
	"AvidScript.Runtime.WasmMissingImportSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmMissingImportSmokeTest::RunTest(const FString& Parameters)
{
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		const bool bLoaded = Runtime.LoadModule(
			GAvidScriptMissingImportWasmModule,
			UE_ARRAY_COUNT(GAvidScriptMissingImportWasmModule),
			TEXT("missing_import"),
			Result);
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing host import is reported")), bLoaded);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing import category")), Result.ErrorCategory, FString(TEXT("binding_package_missing")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing import module")), Result.ImportModuleName, FString(TEXT("avidscript")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing import name")), Result.ImportName, FString(TEXT("host_missing_i32")));
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("missing import diagnostic names the import")),
			Result.ErrorMessage.Contains(TEXT("avidscript.host_missing_i32")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmFailingImportSmokeTest,
	"AvidScript.Runtime.WasmFailingImportSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmFailingImportSmokeTest::RunTest(const FString& Parameters)
{
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("failing import module loads")),
			Runtime.LoadModule(
				GAvidScriptFailingImportWasmModule,
				UE_ARRAY_COUNT(GAvidScriptFailingImportWasmModule),
				TEXT("failing_import"),
				Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("host-side import failure is reported")), Runtime.BeginPlay(Result));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("host failure category")), Result.ErrorCategory, FString(TEXT("host_import_failed")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("host failure import module")), Result.ImportModuleName, FString(TEXT("avidscript")));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("host failure import name")), Result.ImportName, FString(TEXT("host_fail_i32")));
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("host failure diagnostic names the import")),
			Result.ErrorMessage.Contains(TEXT("avidscript.host_fail_i32")));
	}

	return true;
}

#endif
