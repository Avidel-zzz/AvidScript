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

    UObject* BorrowedObject = NewObject<UAvidScriptBindingsTestObject>();
    FAvidScriptObjectHandleResult BorrowResult;
    const FAvidScriptObjectHandle FirstBorrow = Registry.AcquireBorrowedObject(
        BorrowedObject,
        BorrowResult,
        false);
    const FAvidScriptObjectHandle SecondBorrow = Registry.AcquireBorrowedObject(
        BorrowedObject,
        BorrowResult,
        false);
    TestEqual(TEXT("Borrowed leases reuse one handle"), SecondBorrow, FirstBorrow);
    TestEqual(TEXT("Borrowed leases add one live slot"), Registry.GetLiveHandleCount(), 2);
    TestTrue(TEXT("First borrowed release preserves the shared slot"),
        Registry.ReleaseBorrowedHandle(FirstBorrow, BorrowResult, false));
    TestEqual(TEXT("One borrowed lease keeps its slot live"), Registry.GetLiveHandleCount(), 2);
    TestTrue(TEXT("Second borrowed release frees an unanchored slot"),
        Registry.ReleaseBorrowedHandle(SecondBorrow, BorrowResult, false));
    TestEqual(TEXT("Borrowed-only slot returns to the free list"), Registry.GetLiveHandleCount(), 1);

    const FAvidScriptObjectHandle AnchoredBorrow = Registry.AcquireBorrowedObject(
        BorrowedObject,
        BorrowResult,
        false);
    FAvidScriptObjectHandleResult AnchorResult;
    TestEqual(TEXT("Registration anchors an existing borrowed slot"),
        Registry.RegisterObject(BorrowedObject, AnchorResult, false),
        AnchoredBorrow);
    TestTrue(TEXT("Borrow release preserves an anchored slot"),
        Registry.ReleaseBorrowedHandle(AnchoredBorrow, BorrowResult, false));
    TestEqual(TEXT("Anchored slot remains live after borrowed cleanup"), Registry.GetLiveHandleCount(), 2);
    TestTrue(TEXT("Ordinary release removes the anchored slot"),
        Registry.ReleaseHandle(AnchoredBorrow, AnchorResult, false));
    TestTrue(TEXT("Original fixture handle remains releasable"),
        Registry.ReleaseHandle(ReregisteredHandle, ReleaseResult, false));
    TestEqual(TEXT("Registry returns to zero live handles"), Registry.GetLiveHandleCount(), 0);
    return true;
}

#endif
