#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptActorBinding.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptSceneComponentBinding.h"

#include "AvidScriptBindingsTestTypes.h"

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAvidScriptBindingsBoundarySmokeTest,
    "AvidScript.Architecture.Bindings.ModuleBoundary",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBindingsBoundarySmokeTest::RunTest(const FString& Parameters)
{
    FAvidScriptObjectRegistry Registry;
    TestEqual(TEXT("New binding registry has no live handles"), Registry.GetLiveHandleCount(), 0);

    UObject* Object = NewObject<UAvidScriptBindingsTestObject>();
    FAvidScriptObjectHandleResult FirstResult;
    const FAvidScriptObjectHandle FirstHandle = Registry.RegisterObject(Object, FirstResult);
    FAvidScriptObjectHandleResult DuplicateResult;
    const FAvidScriptObjectHandle DuplicateHandle = Registry.RegisterObject(Object, DuplicateResult);
    TestTrue(TEXT("First registration succeeds"), FirstResult.bSucceeded);
    TestTrue(TEXT("Duplicate registration succeeds"), DuplicateResult.bSucceeded);
    TestEqual(TEXT("Duplicate registration reuses slot"), DuplicateHandle.Slot, FirstHandle.Slot);
    TestEqual(TEXT("Duplicate registration reuses generation"), DuplicateHandle.Generation, FirstHandle.Generation);
    TestEqual(TEXT("Duplicate registration keeps one live handle"), Registry.GetLiveHandleCount(), 1);

    FAvidScriptObjectHandleResult ReleaseResult;
    TestTrue(TEXT("Release removes reverse index entry"), Registry.ReleaseHandle(FirstHandle, ReleaseResult));
    TestEqual(TEXT("Release clears live handle count"), Registry.GetLiveHandleCount(), 0);

    FAvidScriptObjectHandleResult ReregisterResult;
    const FAvidScriptObjectHandle ReregisteredHandle = Registry.RegisterObject(Object, ReregisterResult);
    TestEqual(TEXT("Reregister reuses free slot"), ReregisteredHandle.Slot, FirstHandle.Slot);
    TestNotEqual(TEXT("Reregister advances generation"), ReregisteredHandle.Generation, FirstHandle.Generation);
    TestEqual(TEXT("Reregister restores one live handle"), Registry.GetLiveHandleCount(), 1);
    return true;
}

#endif
