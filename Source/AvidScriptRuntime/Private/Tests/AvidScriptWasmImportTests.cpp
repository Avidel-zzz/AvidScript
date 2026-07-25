#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"

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
	FAvidScriptWasmSmokeResult Result;
	const bool bSucceeded = FAvidScriptWasmRuntime::RunEmbeddedHostImportSmokeTest(Result);

	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("WASM guest can call deterministic host import"), bSucceeded);
	TestTrue(TEXT("BeginPlay export succeeds after host import"), Result.bBeginPlayCalled);
	TestEqual(TEXT("Host import call count"), Result.HostImportCallCount, 1);
	TestEqual(TEXT("Host import input"), Result.LastHostImportInput, 41);
	TestEqual(TEXT("Host import result"), Result.LastHostImportResult, 42);
	TestTrue(TEXT("Host import timing is captured"), Result.Metrics.HostImportCallMs > 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmMissingImportSmokeTest,
	"AvidScript.Runtime.WasmMissingImportSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmMissingImportSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;

	const bool bLoaded = Runtime.LoadModule(
		GAvidScriptMissingImportWasmModule,
		UE_ARRAY_COUNT(GAvidScriptMissingImportWasmModule),
		TEXT("missing_import"),
		Result);

	TestFalse(TEXT("Missing host import is reported without crash"), bLoaded);
	TestEqual(TEXT("Missing import category"), Result.ErrorCategory, FString(TEXT("binding_package_missing")));
	TestEqual(TEXT("Missing import module"), Result.ImportModuleName, FString(TEXT("avidscript")));
	TestEqual(TEXT("Missing import name"), Result.ImportName, FString(TEXT("host_missing_i32")));
	TestTrue(TEXT("Missing import diagnostic names the import"), Result.ErrorMessage.Contains(TEXT("avidscript.host_missing_i32")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmFailingImportSmokeTest,
	"AvidScript.Runtime.WasmFailingImportSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmFailingImportSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;

	TestTrue(
		TEXT("Failing import module loads"),
		Runtime.LoadModule(GAvidScriptFailingImportWasmModule, UE_ARRAY_COUNT(GAvidScriptFailingImportWasmModule), TEXT("failing_import"), Result));
	TestFalse(TEXT("Host-side import failure is reported without crash"), Runtime.BeginPlay(Result));
	TestEqual(TEXT("Host failure category"), Result.ErrorCategory, FString(TEXT("host_import_failed")));
	TestEqual(TEXT("Host failure import module"), Result.ImportModuleName, FString(TEXT("avidscript")));
	TestEqual(TEXT("Host failure import name"), Result.ImportName, FString(TEXT("host_fail_i32")));
	TestTrue(TEXT("Host failure diagnostic names the import"), Result.ErrorMessage.Contains(TEXT("avidscript.host_fail_i32")));

	return true;
}

#endif
