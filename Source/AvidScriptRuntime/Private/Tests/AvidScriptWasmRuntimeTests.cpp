#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptMinimalWasmSmokeTest,
	"AvidScript.Runtime.MinimalWasmSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptMinimalWasmSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmSmokeResult Result;
	const bool bSucceeded = FAvidScriptWasmRuntime::RunEmbeddedSmokeTest(Result);

	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("WAMR embedded smoke test succeeds"), bSucceeded);
	TestTrue(TEXT("avid_on_begin_play export is called"), Result.bBeginPlayCalled);
	TestTrue(TEXT("avid_on_tick export is called"), Result.bTickCalled);

	return true;
}

#endif

