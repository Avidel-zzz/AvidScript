#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"

#include "AvidScriptLifecycleState.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptRuntimeLifecycleStateIntegrationTest,
	"AvidScript.Architecture.RuntimeLifecycle.StateIntegration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptRuntimeLifecycleStateIntegrationTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;

	TestEqual(TEXT("New runtime is Empty"), Runtime.GetLifecycleState(), EAvidScriptLifecycleState::Empty);
	TestTrue(TEXT("Embedded module loads"), Runtime.LoadEmbeddedSmokeModule(Result));
	TestEqual(TEXT("Loaded module enters Loaded"), Runtime.GetLifecycleState(), EAvidScriptLifecycleState::Loaded);
	TestTrue(TEXT("BeginPlay succeeds"), Runtime.BeginPlay(Result));
	TestEqual(TEXT("BeginPlay enters Running"), Runtime.GetLifecycleState(), EAvidScriptLifecycleState::Running);
	TestTrue(TEXT("EndPlay succeeds"), Runtime.EndPlay(Result));
	TestEqual(TEXT("EndPlay enters Stopped"), Runtime.GetLifecycleState(), EAvidScriptLifecycleState::Stopped);
	Runtime.Unload(Result);
	TestEqual(TEXT("Unload resets to Empty"), Runtime.GetLifecycleState(), EAvidScriptLifecycleState::Empty);
	return true;
}

#endif
