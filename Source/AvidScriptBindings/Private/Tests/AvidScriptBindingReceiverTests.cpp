#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptBindingReceiver.h"

#include "AvidScriptBindingInvocation.h"
#include "AvidScriptBindingsTestTypes.h"
#include "AvidScriptObjectOwnership.h"
#include "Async/Async.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

namespace
{
class FAvidScriptReceiverTestOwnership final : public IAvidScriptObjectOwnershipDomain
{
public:
	bool Adopt(FAvidScriptObjectRegistry&, UObject&, const FAvidScriptObjectHandle&,
		EAvidScriptObjectFactoryKind, FAvidScriptObjectHandleResult&) override { return false; }
	bool Borrow(FAvidScriptObjectRegistry&, UObject&, FAvidScriptObjectHandleResult&) override { return false; }
	bool Release(const FAvidScriptObjectHandle&, FAvidScriptObjectRegistry&,
		FAvidScriptObjectHandleResult&) override { return false; }
	void Cleanup(FAvidScriptObjectRegistry&) override {}

	bool Owns(const FAvidScriptObjectHandle& Handle, const UObject* ExpectedObject) const override
	{
		const auto* Object = Owned.Find(Handle.ToUInt64());
		return Object != nullptr && (ExpectedObject == nullptr || *Object == ExpectedObject);
	}

	bool HasCapability(const FAvidScriptObjectHandle& Handle, const UObject* ExpectedObject) const override
	{
		const auto* Object = Borrowed.Find(Handle.ToUInt64());
		return Owns(Handle, ExpectedObject)
			|| (Object != nullptr && (ExpectedObject == nullptr || *Object == ExpectedObject));
	}

	TMap<uint64, const UObject*> Owned;
	TMap<uint64, const UObject*> Borrowed;
};

void CheckAvidScriptReceiver(
	FAutomationTestBase& Test,
	const TCHAR* Label,
	const FAvidScriptObjectHandle& Handle,
	UClass* ExpectedClass,
	const FAvidScriptBindingInvocationContext& Context,
	UObject* ExpectedObject,
	const TCHAR* ExpectedFailure = nullptr)
{
	UObject* Object = GetTransientPackage();
	FString Details = TEXT("previous failure");
	const bool bSucceeded = ResolveAvidScriptBindingReceiver(
		Handle, ExpectedClass, Context, Object, Details);
	Test.TestEqual(FString::Printf(TEXT("%s result"), Label), bSucceeded, ExpectedObject != nullptr);
	Test.TestTrue(FString::Printf(TEXT("%s output"), Label), Object == ExpectedObject);
	if (ExpectedObject != nullptr)
	{
		Test.TestTrue(FString::Printf(TEXT("%s clears failure details"), Label), Details.IsEmpty());
	}
	else
	{
		Test.TestTrue(FString::Printf(TEXT("%s has failure details"), Label),
			!Details.IsEmpty() && Details != TEXT("previous failure"));
		if (ExpectedFailure != nullptr)
		{
			Test.TestTrue(FString::Printf(TEXT("%s failure category"), Label), Details.Contains(ExpectedFailure));
		}
	}
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptBindingReceiverCapabilitiesTest,
	"AvidScript.Bindings.Receiver.WorldlessCapabilities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBindingReceiverCapabilitiesTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	FAvidScriptReceiverTestOwnership Ownership;
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.ObjectOwnership = &Ownership;
	Context.World = NewObject<UWorld>();

	UAvidScriptBindingsTestObject* OwnedObject = NewObject<UAvidScriptBindingsTestObject>();
	UAvidScriptBindingsTestObject* BorrowedObject = NewObject<UAvidScriptBindingsTestObject>();
	FAvidScriptObjectHandleResult Result;
	const FAvidScriptObjectHandle OwnedHandle = Registry.RegisterObject(OwnedObject, Result, false);
	TestTrue(TEXT("Owned handle registered"), Result.bSucceeded);
	const FAvidScriptObjectHandle BorrowedHandle = Registry.AcquireBorrowedObject(BorrowedObject, Result, false);
	TestTrue(TEXT("Borrowed handle registered"), Result.bSucceeded);
	TestNull(TEXT("Owned UObject is worldless"), OwnedObject->GetWorld());
	TestNull(TEXT("Borrowed UObject is worldless"), BorrowedObject->GetWorld());
	Ownership.Owned.Add(OwnedHandle.ToUInt64(), OwnedObject);
	Ownership.Borrowed.Add(BorrowedHandle.ToUInt64(), BorrowedObject);
	TestFalse(TEXT("Borrowed capability is not ownership"), Ownership.Owns(BorrowedHandle, BorrowedObject));

	CheckAvidScriptReceiver(*this, TEXT("Owned worldless"), OwnedHandle, UAvidScriptBindingsTestObject::StaticClass(), Context, OwnedObject);
	CheckAvidScriptReceiver(*this, TEXT("Borrowed worldless"), BorrowedHandle, UObject::StaticClass(), Context, BorrowedObject);
	Context.World.Reset();
	CheckAvidScriptReceiver(*this, TEXT("Worldless without host world"), BorrowedHandle, UAvidScriptBindingsTestObject::StaticClass(), Context, BorrowedObject);

	Context.ObjectOwnership = nullptr;
	CheckAvidScriptReceiver(*this, TEXT("Missing domain"), OwnedHandle, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr,
		TEXT("binding_receiver_capability_denied"));
	Context.OwnerHandle = OwnedHandle;
	CheckAvidScriptReceiver(*this, TEXT("Owner without domain"), OwnedHandle, UAvidScriptBindingsTestObject::StaticClass(), Context, OwnedObject);

	Context.OwnerHandle = {};
	Context.ObjectOwnership = &Ownership;
	Ownership.Owned.Reset();
	CheckAvidScriptReceiver(*this, TEXT("Registered but ungranted"), OwnedHandle, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr,
		TEXT("binding_receiver_capability_denied"));
	Ownership.Owned.Add(OwnedHandle.ToUInt64(), BorrowedObject);
	CheckAvidScriptReceiver(*this, TEXT("Capability object mismatch"), OwnedHandle, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr,
		TEXT("binding_receiver_capability_denied"));
	Ownership.Borrowed.Reset();
	CheckAvidScriptReceiver(*this, TEXT("Borrowed capability revoked"), BorrowedHandle, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr,
		TEXT("binding_receiver_capability_denied"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptBindingReceiverInvalidHandleTest,
	"AvidScript.Bindings.Receiver.InvalidHandlesAndTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBindingReceiverInvalidHandleTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	UAvidScriptBindingsTestObject* Object = NewObject<UAvidScriptBindingsTestObject>();
	FAvidScriptObjectHandleResult Result;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(Object, Result, false);
	TestTrue(TEXT("Owner handle registered"), Result.bSucceeded);
	Context.OwnerHandle = Handle;

	CheckAvidScriptReceiver(*this, TEXT("Wrong expected class"), Handle, UWorld::StaticClass(), Context, nullptr);
	CheckAvidScriptReceiver(*this, TEXT("Invalid handle"), {}, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr);
	Context.ObjectRegistry = nullptr;
	CheckAvidScriptReceiver(*this, TEXT("Missing registry"), Handle, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr);
	Context.ObjectRegistry = &Registry;

	FAvidScriptObjectHandle WrongGeneration = Handle;
	++WrongGeneration.Generation;
	Context.OwnerHandle = WrongGeneration;
	CheckAvidScriptReceiver(*this, TEXT("Wrong generation owner"), WrongGeneration, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr);
	Context.OwnerHandle = Handle;
	Object->MarkAsGarbage();
	CheckAvidScriptReceiver(*this, TEXT("Invalid object owner"), Handle, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr);
	Object->ClearGarbage();
	TestTrue(TEXT("Owner handle released"), Registry.ReleaseHandle(Handle, Result, false));
	CheckAvidScriptReceiver(*this, TEXT("Stale owner handle"), Handle, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptBindingReceiverWorldAndThreadTest,
	"AvidScript.Bindings.Receiver.WorldAndThreadBoundaries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBindingReceiverWorldAndThreadTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	FAvidScriptReceiverTestOwnership Ownership;
	FAvidScriptBindingInvocationContext Context;
	Context.ObjectRegistry = &Registry;
	Context.ObjectOwnership = &Ownership;
	UWorld* HostWorld = NewObject<UWorld>();
	UWorld* ForeignWorld = NewObject<UWorld>();
	Context.World = HostWorld;
	FAvidScriptObjectHandleResult Result;
	const FAvidScriptObjectHandle HostHandle = Registry.RegisterObject(HostWorld, Result, false);
	TestTrue(TEXT("Host world registered"), Result.bSucceeded);
	const FAvidScriptObjectHandle ForeignHandle = Registry.RegisterObject(ForeignWorld, Result, false);
	TestTrue(TEXT("Foreign world registered"), Result.bSucceeded);
	Ownership.Borrowed.Add(HostHandle.ToUInt64(), HostWorld);
	Ownership.Owned.Add(ForeignHandle.ToUInt64(), ForeignWorld);

	CheckAvidScriptReceiver(*this, TEXT("Same world capability"), HostHandle, UWorld::StaticClass(), Context, HostWorld);
	Context.OwnerHandle = HostHandle;
	Context.ObjectOwnership = nullptr;
	CheckAvidScriptReceiver(*this, TEXT("Same world owner"), HostHandle, UWorld::StaticClass(), Context, HostWorld);
	Context.ObjectOwnership = &Ownership;
	CheckAvidScriptReceiver(*this, TEXT("Foreign world capability"), ForeignHandle, UWorld::StaticClass(), Context, nullptr,
		TEXT("binding_receiver_world_mismatch"));
	Context.OwnerHandle = ForeignHandle;
	CheckAvidScriptReceiver(*this, TEXT("Foreign world owner"), ForeignHandle, UWorld::StaticClass(), Context, nullptr,
		TEXT("binding_receiver_world_mismatch"));
	Context.World.Reset();
	CheckAvidScriptReceiver(*this, TEXT("Foreign world without host"), ForeignHandle, UWorld::StaticClass(), Context, nullptr,
		TEXT("binding_receiver_world_mismatch"));

	UAvidScriptBindingsTestObject* Object = NewObject<UAvidScriptBindingsTestObject>();
	const FAvidScriptObjectHandle ObjectHandle = Registry.RegisterObject(Object, Result, false);
	TestTrue(TEXT("Worldless receiver registered"), Result.bSucceeded);
	Ownership.Owned.Add(ObjectHandle.ToUInt64(), Object);
	Context.World = HostWorld;
	HostWorld->MarkAsGarbage();
	TestTrue(TEXT("Host world weak reference is stale"), Context.World.IsStale());
	CheckAvidScriptReceiver(*this, TEXT("Stale host with worldless capability"), ObjectHandle, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr,
		TEXT("binding_receiver_world_stale"));
	Context.OwnerHandle = ObjectHandle;
	CheckAvidScriptReceiver(*this, TEXT("Stale host with owner"), ObjectHandle, UAvidScriptBindingsTestObject::StaticClass(), Context, nullptr,
		TEXT("binding_receiver_world_stale"));
	HostWorld->ClearGarbage();

	UObject* Output = Object;
	FString Details;
	UClass* ExpectedClass = UAvidScriptBindingsTestObject::StaticClass();
	const bool bWorkerSucceeded = Async(EAsyncExecution::ThreadPool, [&]()
	{
		return ResolveAvidScriptBindingReceiver(ObjectHandle, ExpectedClass, Context, Output, Details);
	}).Get();
	TestFalse(TEXT("Worker thread receiver rejected"), bWorkerSucceeded);
	TestNull(TEXT("Worker failure clears output"), Output);
	TestTrue(TEXT("Worker failure reports thread boundary"), Details.Contains(TEXT("binding_receiver_thread_invalid")));
	return true;
}

#endif
