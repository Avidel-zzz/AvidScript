#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectFactoryBinding.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"

namespace
{
class FRejectingObjectFactoryOwnership final
	: public IAvidScriptObjectOwnershipDomain
{
public:
	bool Adopt(
		FAvidScriptObjectRegistry&,
		UObject&,
		const FAvidScriptObjectHandle& Handle,
		EAvidScriptObjectFactoryKind,
		FAvidScriptObjectHandleResult& OutResult) override
	{
		++AdoptCount;
		OutResult = FAvidScriptObjectHandleResult();
		OutResult.Handle = Handle;
		OutResult.ErrorCategory = TEXT("injected_ownership_rejection");
		OutResult.ErrorMessage = TEXT("Injected ownership rejection.");
		return false;
	}

	bool AdoptSpawnedActor(
		FAvidScriptObjectRegistry&,
		AActor&,
		const FAvidScriptObjectHandle&,
		FAvidScriptObjectHandleResult&) override
	{
		return false;
	}

	bool Release(
		const FAvidScriptObjectHandle&,
		FAvidScriptObjectRegistry&,
		FAvidScriptObjectHandleResult&) override
	{
		return false;
	}

	bool Borrow(
		FAvidScriptObjectRegistry&,
		UObject&,
		FAvidScriptObjectHandleResult&) override
	{
		return false;
	}

	bool Owns(const FAvidScriptObjectHandle&, const UObject*) const override
	{
		return false;
	}

	void Cleanup(FAvidScriptObjectRegistry&) override
	{
	}

	int32 AdoptCount = 0;
};

bool CreateObjectFactoryBindingWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptObjectFactoryBindingWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyObjectFactoryBindingWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

FAvidScriptObjectFactoryPlan MakePlainObjectFactoryPlan()
{
	FAvidScriptObjectFactoryPlan Plan;
	Plan.Kind = EAvidScriptObjectFactoryKind::NewObject;
	Plan.ObjectClass = UAvidScriptObjectRegistryTestObject::StaticClass();
	Plan.RequiredOuterClass = UObject::StaticClass();
	Plan.ResultObjectTypeOrdinal = 0;
	Plan.Ownership = EAvidScriptObjectOwnershipPolicy::Session;
	Plan.Registration = EAvidScriptComponentRegistrationPolicy::None;
	return Plan;
}

FAvidScriptObjectFactoryPlan MakeComponentFactoryPlan()
{
	FAvidScriptObjectFactoryPlan Plan;
	Plan.Kind = EAvidScriptObjectFactoryKind::ActorComponent;
	Plan.ObjectClass = UAvidScriptSessionOwnershipTestComponent::StaticClass();
	Plan.RequiredOuterClass = AActor::StaticClass();
	Plan.ResultObjectTypeOrdinal = 1;
	Plan.Ownership = EAvidScriptObjectOwnershipPolicy::Session;
	Plan.Registration =
		EAvidScriptComponentRegistrationPolicy::RegisterInstance;
	return Plan;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptObjectFactoryBindingTest,
	"AvidScript.Runtime.Binding.ObjectFactory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptObjectFactoryBindingTest::RunTest(const FString& Parameters)
{
	FAvidScriptObjectRegistry Registry;
	FAvidScriptSessionObjectOwnership Ownership;
	FAvidScriptObjectHandleResult Result;
	const FAvidScriptObjectHandle PackageHandle = Registry.RegisterObject(
		GetTransientPackage(),
		Result,
		false);
	if (!TestTrue(
		TEXT("Transient package registers as a valid UObject Outer"),
		Result.bSucceeded && PackageHandle.IsValid()))
	{
		return false;
	}

	const FAvidScriptObjectFactoryPlan PlainPlan = MakePlainObjectFactoryPlan();
	if (!TestTrue(
		TEXT("Generic factory constructs a session-owned UObject"),
		FAvidScriptObjectFactoryBinding::Construct(
			PlainPlan,
			Registry,
			Ownership,
			PackageHandle,
			Result)))
	{
		AddError(Result.ErrorMessage);
		return false;
	}
	const FAvidScriptObjectHandle PlainHandle = Result.Handle;
	UAvidScriptObjectRegistryTestObject* PlainObject =
		Registry.ResolveObject<UAvidScriptObjectRegistryTestObject>(
			PlainHandle,
			Result,
			false);
	TWeakObjectPtr<UAvidScriptObjectRegistryTestObject> WeakPlainObject(PlainObject);
	TestNotNull(TEXT("Constructed UObject resolves through its handle"), PlainObject);
	TestTrue(
		TEXT("Constructed UObject belongs to the session ownership domain"),
		Ownership.Owns(PlainHandle, PlainObject));
	PlainObject = nullptr;
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestTrue(TEXT("Session ownership keeps constructed UObject alive"), WeakPlainObject.IsValid());

	UObject* ForeignObject = NewObject<UAvidScriptObjectRegistryTestObject>();
	const FAvidScriptObjectHandle ForeignHandle = Registry.RegisterObject(
		ForeignObject,
		Result,
		false);
	TestFalse(
		TEXT("Release rejects an object not created by this session"),
		FAvidScriptObjectFactoryBinding::Release(
			Registry,
			Ownership,
			ForeignHandle,
			Result));
	TestEqual(
		TEXT("Release authority failure has a stable category"),
		Result.ErrorCategory,
		FString(TEXT("ownership_violation")));

	const int32 LiveHandlesBeforeRejectedAdopt = Registry.GetLiveHandleCount();
	FRejectingObjectFactoryOwnership RejectingOwnership;
	TestFalse(
		TEXT("Construct rolls back a registered handle when ownership adoption fails"),
		FAvidScriptObjectFactoryBinding::Construct(
			PlainPlan,
			Registry,
			RejectingOwnership,
			PackageHandle,
			Result));
	TestEqual(TEXT("Ownership rejection is reached once"), RejectingOwnership.AdoptCount, 1);
	TestEqual(
		TEXT("Rejected adoption leaves no registry handle behind"),
		Registry.GetLiveHandleCount(),
		LiveHandlesBeforeRejectedAdopt);

	TestTrue(
		TEXT("Explicit Release removes a session-owned UObject"),
		FAvidScriptObjectFactoryBinding::Release(
			Registry,
			Ownership,
			PlainHandle,
			Result));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
	TestFalse(TEXT("Released UObject becomes collectable"), WeakPlainObject.IsValid());

	UWorld* World = nullptr;
	if (!CreateObjectFactoryBindingWorld(World))
	{
		AddError(TEXT("Failed to create object factory test world."));
		return false;
	}
	AActor* Owner = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	AActor* OtherOwner = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	if (!TestNotNull(TEXT("Component owner spawns"), Owner)
		|| !TestNotNull(TEXT("Query-miss owner spawns"), OtherOwner))
	{
		Ownership.Cleanup(Registry);
		DestroyObjectFactoryBindingWorld(World);
		return false;
	}
	const FAvidScriptObjectHandle OwnerHandle = Registry.RegisterObject(
		Owner,
		Result,
		false);
	const FAvidScriptObjectHandle OtherOwnerHandle = Registry.RegisterObject(
		OtherOwner,
		Result,
		false);

	const FAvidScriptObjectFactoryPlan ComponentPlan = MakeComponentFactoryPlan();
	if (!TestTrue(
		TEXT("Generic factory creates and registers an ActorComponent"),
		FAvidScriptObjectFactoryBinding::Construct(
			ComponentPlan,
			Registry,
			Ownership,
			OwnerHandle,
			Result)))
	{
		AddError(Result.ErrorMessage);
		Ownership.Cleanup(Registry);
		DestroyObjectFactoryBindingWorld(World);
		return false;
	}
	const FAvidScriptObjectHandle ComponentHandle = Result.Handle;
	UActorComponent* Component = Registry.ResolveObject<UActorComponent>(
		ComponentHandle,
		Result,
		false);
	TestNotNull(TEXT("Constructed component resolves"), Component);
	TestTrue(TEXT("Constructed component is registered"), Component->IsRegistered());
	TestEqual(TEXT("Constructed component uses instance creation"),
		Component->CreationMethod, EComponentCreationMethod::Instance);
	TestTrue(TEXT("Owner tracks the dynamic instance component"),
		Owner->GetInstanceComponents().Contains(Component));

	TestTrue(
		TEXT("FindComponent returns an existing matching component"),
		FAvidScriptObjectFactoryBinding::FindComponent(
			Registry,
			Ownership,
			OwnerHandle,
			*UAvidScriptSessionOwnershipTestComponent::StaticClass(),
			Result));
	TestEqual(TEXT("FindComponent reuses the existing registry handle"),
		Result.Handle, ComponentHandle);
	TestTrue(
		TEXT("A missing component is a successful empty query"),
		FAvidScriptObjectFactoryBinding::FindComponent(
			Registry,
			Ownership,
			OtherOwnerHandle,
			*UAvidScriptSessionOwnershipTestComponent::StaticClass(),
			Result));
	TestFalse(TEXT("Query miss returns an invalid packed handle"), Result.Handle.IsValid());
	UAvidScriptSessionOwnershipTestComponent* BorrowedComponent =
		NewObject<UAvidScriptSessionOwnershipTestComponent>(OtherOwner);
	OtherOwner->AddInstanceComponent(BorrowedComponent);
	BorrowedComponent->RegisterComponent();
	const int32 LiveHandlesBeforeBorrow = Registry.GetLiveHandleCount();
	TestTrue(
		TEXT("FindComponent acquires a borrowed handle for an existing component"),
		FAvidScriptObjectFactoryBinding::FindComponent(
			Registry,
			Ownership,
			OtherOwnerHandle,
			*UAvidScriptSessionOwnershipTestComponent::StaticClass(),
			Result));
	const FAvidScriptObjectHandle BorrowedHandle = Result.Handle;
	TestTrue(TEXT("Borrowed component handle is valid"), BorrowedHandle.IsValid());
	TestEqual(TEXT("First borrowed component adds one registry slot"),
		Registry.GetLiveHandleCount(),
		LiveHandlesBeforeBorrow + 1);
	TestTrue(
		TEXT("Repeated FindComponent reuses the session borrowed lease"),
		FAvidScriptObjectFactoryBinding::FindComponent(
			Registry,
			Ownership,
			OtherOwnerHandle,
			*UAvidScriptSessionOwnershipTestComponent::StaticClass(),
			Result));
	TestEqual(TEXT("Repeated FindComponent returns the same handle"), Result.Handle, BorrowedHandle);
	TestEqual(TEXT("Repeated FindComponent does not grow the registry"),
		Registry.GetLiveHandleCount(),
		LiveHandlesBeforeBorrow + 1);

	TestFalse(
		TEXT("Component factory rejects a non-Actor Outer"),
		FAvidScriptObjectFactoryBinding::Construct(
			ComponentPlan,
			Registry,
			Ownership,
			PackageHandle,
			Result));
	TestEqual(TEXT("Wrong Outer failure has a stable category"),
		Result.ErrorCategory, FString(TEXT("binding_factory_outer_mismatch")));

	TestTrue(
		TEXT("Explicit Release destroys the owned component"),
		FAvidScriptObjectFactoryBinding::Release(
			Registry,
			Ownership,
			ComponentHandle,
			Result));
	TestTrue(TEXT("Released component enters destruction"), Component->IsBeingDestroyed());
	TestFalse(TEXT("Released component leaves the Actor instance list"),
		Owner->GetInstanceComponents().Contains(Component));

	Ownership.Cleanup(Registry);
	TestEqual(TEXT("Session cleanup releases the borrowed component slot"),
		Registry.GetLiveHandleCount(),
		LiveHandlesBeforeBorrow - 1);
	DestroyObjectFactoryBindingWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
