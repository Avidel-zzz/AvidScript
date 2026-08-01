#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptActorBinding.h"
#include "AvidScriptBindingInvocation.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptSceneComponentBinding.h"

#include "AvidScriptBindingsTestTypes.h"

#include "Misc/AutomationTest.h"
#include "UObject/UObjectGlobals.h"

namespace
{
class FAvidScriptBoundaryGuestMemory final : public IAvidScriptVmGuestMemory
{
public:
	bool ReadBytes(
		uint32 GuestAddress,
		TArrayView<uint8> OutBytes,
		FString& OutError) override
	{
		OutError.Reset();
		FMemory::Memzero(OutBytes.GetData(), OutBytes.Num());
		return true;
	}

	bool WriteBytes(
		uint32 GuestAddress,
		TConstArrayView<uint8> Bytes,
		FString& OutError) override
	{
		OutError.Reset();
		return true;
	}
};
} // namespace

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPreparedDynamicBoundaryTest,
	"AvidScript.Bindings.PreparedDynamic.Boundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPreparedDynamicBoundaryTest::RunTest(
	const FString& Parameters)
{
	UClass* ExpectedClass = UAvidScriptBindingsTestObject::StaticClass();
	UFunction* Function = ExpectedClass->FindFunctionByName(
		GET_FUNCTION_NAME_CHECKED(
			UAvidScriptBindingsTestObject,
			ReflectionFallbackAddFloat));
	const TSharedPtr<const FAvidScriptBindingPackage> Package =
		FAvidScriptBindingPackage::MakePreparedDynamicPlanForTesting(
			TEXT("prepared-dynamic-test"),
			TEXT("avid_prepared_dynamic_test"),
			TEXT("(ii)i"),
			ExpectedClass,
			Function,
			2,
			64,
			true,
			true);
	if (!TestTrue(TEXT("Prepared test package is created"), Package.IsValid()))
	{
		return false;
	}

	TArray<FAvidScriptPreparedDynamicBinding> Bindings;
	FString Error;
	if (!TestTrue(
			TEXT("Prepared cells publish from the package"),
			Package->BuildPreparedDynamicBindings(Bindings, Error))
		|| !TestEqual(TEXT("One prepared cell is published"), Bindings.Num(), 1))
	{
		AddError(Error);
		return false;
	}
	const FAvidScriptPreparedDynamicBinding& Binding = Bindings[0];
	TestEqual(TEXT("Prepared ordinal is stable"), Binding.BindingOrdinal, 0u);
	TestEqual(
		TEXT("Prepared import identity is stable"),
		Binding.ImportName,
		FString(TEXT("avid_prepared_dynamic_test")));
	TestNotNull(
		TEXT("Prepared cell borrows package-owned immutable storage"),
		Binding.ImmutableInvocationCell);
	TestNotNull(TEXT("Prepared cell has an invoke entry"), Binding.Invoke);

	FAvidScriptBoundaryGuestMemory GuestMemory;
	FAvidScriptBindingInvocationContext InvocationContext;
	TArray<uint8> Scratch;
	Scratch.SetNumUninitialized(64);
	FAvidScriptDynamicHostCallResult Result;
	UAvidScriptBindingsTestObject* Receiver =
		NewObject<UAvidScriptBindingsTestObject>();
	const uint64 ValidArguments[] = { 0, 0 };
	const uint64 ShortArguments[] = { 0 };
	TestFalse(
		TEXT("Prepared invocation rejects an argument-count mismatch"),
		Binding.Invoke(
			Binding.ImmutableInvocationCell,
			*Receiver,
			MakeArrayView(ShortArguments),
			&GuestMemory,
			InvocationContext,
			Scratch,
			Result));
	TestTrue(
		TEXT("Argument mismatch reports the frame contract"),
		Result.Details.Contains(TEXT("binding_frame_mismatch")));

	TestFalse(
		TEXT("Prepared invocation rejects missing guest memory"),
		Binding.Invoke(
			Binding.ImmutableInvocationCell,
			*Receiver,
			MakeArrayView(ValidArguments),
			nullptr,
			InvocationContext,
			Scratch,
			Result));
	TestTrue(
		TEXT("Missing guest memory reports the frame contract"),
		Result.Details.Contains(TEXT("binding_frame_mismatch")));

	TArray<uint8> ShortScratch;
	ShortScratch.SetNumUninitialized(63);
	TestFalse(
		TEXT("Prepared invocation rejects insufficient scratch"),
		Binding.Invoke(
			Binding.ImmutableInvocationCell,
			*Receiver,
			MakeArrayView(ValidArguments),
			&GuestMemory,
			InvocationContext,
			ShortScratch,
			Result));
	TestTrue(
		TEXT("Insufficient scratch reports its contract"),
		Result.Details.Contains(TEXT("binding_scratch_too_small")));

	UObject* StaleOwner = UObject::StaticClass();
	TestFalse(
		TEXT("Prepared invocation rejects a stale owner type"),
		Binding.Invoke(
			Binding.ImmutableInvocationCell,
			*StaleOwner,
			MakeArrayView(ValidArguments),
			&GuestMemory,
			InvocationContext,
			Scratch,
			Result));
	TestTrue(
		TEXT("Stale owner reports the target contract"),
		Result.Details.Contains(TEXT("binding_target_invalid")));
	return true;
}

#endif
