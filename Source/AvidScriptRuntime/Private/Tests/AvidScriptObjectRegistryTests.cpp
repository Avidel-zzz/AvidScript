#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"

#include "AvidScriptObjectRegistryTestTypes.h"

#include "Misc/AutomationTest.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

struct FAvidScriptObjectRegistryTestAccessor
{
	static void SetRevision(FAvidScriptObjectRegistry& Registry, const uint64 Revision)
	{
		Registry.Revision = Revision;
	}
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryHandleSmokeTest,
	"AvidScript.Binding.ObjectRegistry.HandleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryHandleSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	UObject* Object = NewObject<UAvidScriptObjectRegistryTestObject>();

	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(Object, RegisterResult);

	TestTrue(TEXT("Registering a live UObject succeeds"), RegisterResult.bSucceeded);
	TestTrue(TEXT("Generated object handle is valid"), Handle.IsValid());
	TestFalse(TEXT("Default registration preserves the diagnostic object path"), RegisterResult.ObjectPath.IsEmpty());
	TestNotEqual(TEXT("Guest-visible handle is not the raw UObject pointer"), Handle.ToUInt64(), reinterpret_cast<uint64>(Object));
	FAvidScriptObjectHandleResult DiagnosticRegisterResult;
	const FAvidScriptObjectHandle DiagnosticHandle = Registry.RegisterObject(Object, DiagnosticRegisterResult, false);
	TestEqual(TEXT("Fast registration reuses the same handle"), DiagnosticHandle, Handle);
	TestTrue(TEXT("Explicit fast registration omits the object path"), DiagnosticRegisterResult.ObjectPath.IsEmpty());

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* ResolvedObject = Registry.ResolveObject<UObject>(Handle, ResolveResult, false);

	TestTrue(TEXT("Resolving a valid handle succeeds"), ResolveResult.bSucceeded);
	TestTrue(TEXT("Resolved object is the registered UObject"), ResolvedObject == Object);
	TestTrue(TEXT("Successful hot-path resolve does not construct an object path"), ResolveResult.ObjectPath.IsEmpty());
	FAvidScriptObjectHandleResult DefaultResolveResult;
	TestTrue(
		TEXT("Default resolve preserves the registered object"),
		Registry.ResolveObject<UObject>(Handle, DefaultResolveResult) == Object);
	TestFalse(TEXT("Default resolve preserves the diagnostic object path"), DefaultResolveResult.ObjectPath.IsEmpty());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryGenerationSmokeTest,
	"AvidScript.Binding.ObjectRegistry.GenerationSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryGenerationSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;

	UObject* FirstObject = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult FirstRegisterResult;
	const FAvidScriptObjectHandle FirstHandle = Registry.RegisterObject(FirstObject, FirstRegisterResult);
	TestTrue(TEXT("First object registers"), FirstRegisterResult.bSucceeded);

	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(TEXT("Releasing a live handle succeeds"), Registry.ReleaseHandle(FirstHandle, ReleaseResult));

	UObject* SecondObject = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult SecondRegisterResult;
	const FAvidScriptObjectHandle SecondHandle = Registry.RegisterObject(SecondObject, SecondRegisterResult);
	TestTrue(TEXT("Second object registers"), SecondRegisterResult.bSucceeded);
	TestEqual(TEXT("Released slot is reused"), SecondHandle.Slot, FirstHandle.Slot);
	TestNotEqual(TEXT("Reused slot receives a new generation"), SecondHandle.Generation, FirstHandle.Generation);

	FAvidScriptObjectHandleResult OldResolveResult;
	UObject* OldResolvedObject = Registry.ResolveObject<UObject>(FirstHandle, OldResolveResult);
	TestNull(TEXT("Old generation does not resolve a reused slot"), OldResolvedObject);
	TestEqual(TEXT("Old generation reports generation mismatch"), OldResolveResult.ErrorCategory, FString(TEXT("generation_mismatch")));
	FAvidScriptObjectHandleResult FastOldResolveResult;
	TestNull(
		TEXT("Hot-path stale generation remains rejected"),
		Registry.ResolveObject<UObject>(FirstHandle, FastOldResolveResult, false));
	TestTrue(
		TEXT("Hot-path failure diagnostics do not construct an object path"),
		FastOldResolveResult.ObjectPath.IsEmpty());

	FAvidScriptObjectHandleResult NewResolveResult;
	UObject* NewResolvedObject = Registry.ResolveObject<UObject>(SecondHandle, NewResolveResult);
	TestTrue(TEXT("New generation resolves"), NewResolveResult.bSucceeded);
	TestTrue(TEXT("New generation resolves the second object"), NewResolvedObject == SecondObject);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryInvalidationSmokeTest,
	"AvidScript.Binding.ObjectRegistry.InvalidationSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryInvalidationSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;

	UObject* Object = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(Object, RegisterResult);
	TestTrue(TEXT("Object registers before GC"), RegisterResult.bSucceeded);

	Object = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);

	FAvidScriptObjectHandleResult ResolveResult;
	UObject* ResolvedObject = Registry.ResolveObject<UObject>(Handle, ResolveResult);
	TestNull(TEXT("Collected UObject does not resolve"), ResolvedObject);
	TestEqual(TEXT("Collected UObject reports invalid object"), ResolveResult.ErrorCategory, FString(TEXT("invalid_object")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryResetAndTypeSmokeTest,
	"AvidScript.Binding.ObjectRegistry.ResetAndTypeSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryResetAndTypeSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;

	UObject* ObjectBeforeReset = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult RegisterBeforeResetResult;
	const FAvidScriptObjectHandle HandleBeforeReset = Registry.RegisterObject(ObjectBeforeReset, RegisterBeforeResetResult);
	TestTrue(TEXT("Object before reset registers"), RegisterBeforeResetResult.bSucceeded);

	Registry.Reset();

	UObject* ObjectAfterReset = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult RegisterAfterResetResult;
	const FAvidScriptObjectHandle HandleAfterReset = Registry.RegisterObject(ObjectAfterReset, RegisterAfterResetResult);
	TestTrue(TEXT("Object after reset registers"), RegisterAfterResetResult.bSucceeded);
	TestEqual(TEXT("Reset can reuse slot numbers"), HandleAfterReset.Slot, HandleBeforeReset.Slot);
	TestNotEqual(TEXT("Reset advances generation domain"), HandleAfterReset.Generation, HandleBeforeReset.Generation);

	FAvidScriptObjectHandleResult OldResolveResult;
	UObject* OldResolvedObject = Registry.ResolveObject<UObject>(HandleBeforeReset, OldResolveResult);
	TestNull(TEXT("Handle from previous generation domain does not resolve"), OldResolvedObject);
	TestEqual(TEXT("Previous generation domain reports generation mismatch"), OldResolveResult.ErrorCategory, FString(TEXT("generation_mismatch")));

	FAvidScriptObjectHandleResult TypeResolveResult;
	UClass* WrongTypeObject = Registry.ResolveObject<UClass>(HandleAfterReset, TypeResolveResult);
	TestNull(TEXT("Wrong typed lookup does not resolve"), WrongTypeObject);
	TestEqual(TEXT("Wrong typed lookup reports type mismatch"), TypeResolveResult.ErrorCategory, FString(TEXT("type_mismatch")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryRevisionContractTest,
	"AvidScript.Binding.ObjectRegistry.RevisionContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryRevisionContractTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	const uint64 InitialRevision = Registry.GetRevision();
	TestNotEqual(TEXT("Registry revision zero remains reserved"), InitialRevision, static_cast<uint64>(0));

	FAvidScriptObjectHandleResult Result;
	Registry.RegisterObject(nullptr, Result, false);
	TestFalse(TEXT("Invalid registration fails"), Result.bSucceeded);
	TestEqual(TEXT("Failed registration preserves revision"), Registry.GetRevision(), InitialRevision);

	UObject* AnchoredObject = NewObject<UAvidScriptObjectRegistryTestObject>();
	const FAvidScriptObjectHandle AnchoredHandle = Registry.RegisterObject(AnchoredObject, Result, false);
	TestTrue(TEXT("New anchored registration succeeds"), Result.bSucceeded);
	TestEqual(TEXT("New anchored identity advances revision"), Registry.GetRevision(), InitialRevision + 1);

	TestEqual(
		TEXT("Duplicate registration reuses the anchored identity"),
		Registry.RegisterObject(AnchoredObject, Result, false),
		AnchoredHandle);
	TestEqual(TEXT("Duplicate registration preserves revision"), Registry.GetRevision(), InitialRevision + 1);

	FAvidScriptObjectHandleResult ResolveResult;
	TestTrue(
		TEXT("Resolve returns the anchored object"),
		Registry.ResolveObject<UObject>(AnchoredHandle, ResolveResult, false) == AnchoredObject);
	TestEqual(TEXT("Resolve preserves revision"), Registry.GetRevision(), InitialRevision + 1);

	const FAvidScriptObjectHandle AnchoredBorrow =
		Registry.AcquireBorrowedObject(AnchoredObject, Result, false);
	TestEqual(TEXT("Borrowing an anchored object reuses its identity"), AnchoredBorrow, AnchoredHandle);
	TestEqual(TEXT("Borrowing an existing identity preserves revision"), Registry.GetRevision(), InitialRevision + 1);
	TestTrue(
		TEXT("Releasing an anchored borrow succeeds"),
		Registry.ReleaseBorrowedHandle(AnchoredBorrow, Result, false));
	TestEqual(TEXT("Anchored borrowed release preserves revision"), Registry.GetRevision(), InitialRevision + 1);

	UObject* BorrowedObject = NewObject<UAvidScriptObjectRegistryTestObject>();
	const FAvidScriptObjectHandle FirstBorrow =
		Registry.AcquireBorrowedObject(BorrowedObject, Result, false);
	TestTrue(TEXT("First borrowed acquisition succeeds"), Result.bSucceeded);
	TestEqual(TEXT("New borrowed identity advances revision"), Registry.GetRevision(), InitialRevision + 2);

	const FAvidScriptObjectHandle SecondBorrow =
		Registry.AcquireBorrowedObject(BorrowedObject, Result, false);
	TestEqual(TEXT("Repeated borrow reuses the identity"), SecondBorrow, FirstBorrow);
	TestEqual(TEXT("Repeated borrow preserves revision"), Registry.GetRevision(), InitialRevision + 2);
	TestTrue(
		TEXT("Non-final borrowed release succeeds"),
		Registry.ReleaseBorrowedHandle(FirstBorrow, Result, false));
	TestEqual(TEXT("Non-final borrowed release preserves revision"), Registry.GetRevision(), InitialRevision + 2);
	TestTrue(
		TEXT("Final borrowed release succeeds"),
		Registry.ReleaseBorrowedHandle(SecondBorrow, Result, false));
	TestEqual(TEXT("Final borrowed release advances revision"), Registry.GetRevision(), InitialRevision + 3);

	TestFalse(
		TEXT("Duplicate borrowed release fails"),
		Registry.ReleaseBorrowedHandle(SecondBorrow, Result, false));
	TestEqual(TEXT("Failed borrowed release preserves revision"), Registry.GetRevision(), InitialRevision + 3);

	UObject* PromotedObject = NewObject<UAvidScriptObjectRegistryTestObject>();
	const FAvidScriptObjectHandle PromotedHandle =
		Registry.AcquireBorrowedObject(PromotedObject, Result, false);
	TestEqual(TEXT("New promoted identity advances revision"), Registry.GetRevision(), InitialRevision + 4);
	TestEqual(
		TEXT("Registration anchors the borrowed identity"),
		Registry.RegisterObject(PromotedObject, Result, false),
		PromotedHandle);
	TestEqual(TEXT("Anchoring an existing identity preserves revision"), Registry.GetRevision(), InitialRevision + 4);
	TestTrue(
		TEXT("Promoted borrowed release succeeds"),
		Registry.ReleaseBorrowedHandle(PromotedHandle, Result, false));
	TestEqual(TEXT("Promoted borrowed release preserves revision"), Registry.GetRevision(), InitialRevision + 4);
	TestTrue(TEXT("Promoted anchored release succeeds"), Registry.ReleaseHandle(PromotedHandle, Result, false));
	TestEqual(TEXT("Anchored release advances revision"), Registry.GetRevision(), InitialRevision + 5);

	Registry.Reset();
	TestEqual(TEXT("Registry reset advances revision"), Registry.GetRevision(), InitialRevision + 6);
	TestNull(
		TEXT("Reset invalidates the remaining anchored identity"),
		Registry.ResolveObject<UObject>(AnchoredHandle, ResolveResult, false));
	TestEqual(TEXT("Failed post-reset resolve preserves revision"), Registry.GetRevision(), InitialRevision + 6);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectRegistryRevisionWrapContractTest,
	"AvidScript.Binding.ObjectRegistry.RevisionWrapContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectRegistryRevisionWrapContractTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectRegistryTestAccessor::SetRevision(Registry, MAX_uint64);

	UObject* Object = NewObject<UAvidScriptObjectRegistryTestObject>();
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(Object, RegisterResult, false);
	TestTrue(TEXT("Registration at revision wrap succeeds"), RegisterResult.bSucceeded);
	TestEqual(TEXT("Revision wrap skips zero"), Registry.GetRevision(), static_cast<uint64>(1));

	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(TEXT("Post-wrap release succeeds"), Registry.ReleaseHandle(Handle, ReleaseResult, false));
	TestEqual(TEXT("Revision continues after wrap"), Registry.GetRevision(), static_cast<uint64>(2));

	return true;
}

#endif
