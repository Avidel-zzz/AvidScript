#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptBorrowedOwnershipGcCompactionTest,
	"AvidScript.Architecture.Session.BorrowedGcStableCompaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBorrowedOwnershipGcCompactionTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	FAvidScriptSessionObjectOwnership Ownership;
	FAvidScriptSessionObjectOwnership Peer;
	TStrongObjectPtr<UObject> First(NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage()));
	TStrongObjectPtr<UObject> Last(NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage()));
	FAvidScriptObjectHandleResult Result;
	Ownership.Borrow(Registry, *First.Get(), Result);
	const FAvidScriptObjectHandle FirstHandle = Result.Handle;
	Peer.Borrow(Registry, *First.Get(), Result);
	TArray<FAvidScriptObjectHandle> DeadHandles;
	for (int32 Index = 0; Index < 32; ++Index)
	{
		UObject* Dead = NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
		TestTrue(TEXT("Dead entry is borrowed"), Ownership.Borrow(Registry, *Dead, Result));
		DeadHandles.Add(Result.Handle);
		if (Index == 15)
		{
			TestTrue(TEXT("Later survivor is borrowed"), Ownership.Borrow(Registry, *Last.Get(), Result));
		}
	}
	const FAvidScriptObjectHandle LastHandle = Registry.AcquireBorrowedObject(Last.Get(), Result, false);
	UObject* Owned = NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	TWeakObjectPtr<UObject> WeakOwned(Owned);
	const FAvidScriptObjectHandle OwnedHandle = Registry.RegisterObject(Owned, Result, false);
	TestTrue(TEXT("Owned object is adopted"), Ownership.Adopt(
		Registry, *Owned, OwnedHandle, EAvidScriptObjectFactoryKind::NewObject, Result));
	Owned = nullptr;

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	Ownership.PruneInvalidBorrowedHandles(Registry);
	TestEqual(TEXT("Compaction retains both live borrowed entries"), Ownership.GetBorrowedHandleCount(), 2);
	TestEqual(TEXT("Only two live borrowed slots and the owned slot remain"), Registry.GetLiveHandleCount(), 3);
	TestEqual(TEXT("Owned journal is untouched"), Ownership.Num(), 1);
	TestTrue(TEXT("Owned strong reference survives GC"), WeakOwned.IsValid());
	TestTrue(TEXT("Peer retains its live capability"), Peer.HasCapability(FirstHandle, First.Get()));
	TestTrue(TEXT("First handle identity survives compaction"), Ownership.HasCapability(FirstHandle, First.Get()));
	TestTrue(TEXT("Last handle identity survives compaction"), Ownership.HasCapability(LastHandle, Last.Get()));
	for (const FAvidScriptObjectHandle& Handle : DeadHandles)
	{
		TestFalse(TEXT("Dead entry capability is removed"), Ownership.HasCapability(Handle));
		TestNull(TEXT("Dead handle is invalid"), Registry.ResolveObject(Handle, Result, false));
	}
	const uint64 Revision = Registry.GetRevision();
	Ownership.PruneInvalidBorrowedHandles(Registry);
	TestEqual(TEXT("Repeated pruning does not mutate surviving registry identities"), Registry.GetRevision(), Revision);
	TestTrue(TEXT("Borrowing a survivor remains deduplicated"), Ownership.Borrow(Registry, *Last.Get(), Result));
	TestEqual(TEXT("Compacted object lookup has no duplicate entry"), Ownership.GetBorrowedHandleCount(), 2);

	FString Error;
	TestTrue(TEXT("A checkpoint after compaction retains the original first survivor"),
		Ownership.RollbackBorrowedHandles(Registry, 1, Error));
	TestTrue(TEXT("Stable order preserves first capability"), Ownership.HasCapability(FirstHandle));
	TestFalse(TEXT("Stable order removes last capability"), Ownership.HasCapability(LastHandle));
	TestEqual(TEXT("Another lease still resolves the last survivor"), Registry.ResolveObject(LastHandle, Result, false), Last.Get());
	Registry.ReleaseBorrowedHandle(LastHandle, Result, false);
	Ownership.Cleanup(Registry);
	TestEqual(TEXT("Cleanup does not revoke the peer's lease"), Registry.ResolveObject(FirstHandle, Result, false), First.Get());
	Peer.Cleanup(Registry);
	TestEqual(TEXT("All leases are eventually released"), Registry.GetLiveHandleCount(), 0);

	UObject* Stale = NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage());
	Ownership.Borrow(Registry, *Stale, Result);
	const FAvidScriptObjectHandle StaleHandle = Result.Handle;
	Registry.ReleaseHandle(StaleHandle, Result, false);
	TStrongObjectPtr<UObject> Replacement(NewObject<UAvidScriptObjectRegistryTestObject>(GetTransientPackage()));
	const FAvidScriptObjectHandle ReplacementHandle = Registry.AcquireBorrowedObject(Replacement.Get(), Result, false);
	TestEqual(TEXT("Fixture reuses the released slot"), ReplacementHandle.Slot, StaleHandle.Slot);
	TestNotEqual(TEXT("Reused slot advances generation"), ReplacementHandle.Generation, StaleHandle.Generation);
	Stale = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	Ownership.PruneInvalidBorrowedHandles(Registry);
	TestEqual(TEXT("Stale journal entry is removed"), Ownership.GetBorrowedHandleCount(), 0);
	TestEqual(TEXT("Pruning an old generation cannot release the replacement"),
		Registry.ResolveObject(ReplacementHandle, Result, false), Replacement.Get());
	TestTrue(TEXT("Replacement lease remains releasable exactly once"),
		Registry.ReleaseBorrowedHandle(ReplacementHandle, Result, false));
	return true;
}

#endif
