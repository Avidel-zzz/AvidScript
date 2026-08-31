#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

namespace
{
class FAvidScriptEventSubscriptionHostSpy final
	: public IAvidScriptEventSubscriptionHost
{
public:
	virtual int64 Subscribe(
		UObject& Source,
		const uint32 EventOrdinal,
		FString& OutError) override
	{
		++SubscribeCallCount;
		LastSource = &Source;
		LastEventOrdinal = EventOrdinal;
		OutError.Reset();
		return ExpectedToken;
	}

	virtual bool Unsubscribe(
		const int64 SubscriptionToken,
		FString& OutError) override
	{
		++UnsubscribeCallCount;
		LastUnsubscribeToken = SubscriptionToken;
		OutError.Reset();
		return SubscriptionToken == ExpectedToken;
	}

	static constexpr int64 ExpectedToken = 0x0000000100000001LL;
	TWeakObjectPtr<UObject> LastSource;
	uint32 LastEventOrdinal = MAX_uint32;
	int64 LastUnsubscribeToken = 0;
	int32 SubscribeCallCount = 0;
	int32 UnsubscribeCallCount = 0;
};

bool CreateEventSubscriptionWorld(const TCHAR* Name, UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, Name);
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext =
		GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyEventSubscriptionWorld(UWorld*& World)
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEventSubscriptionHostBoundaryTest,
	"AvidScript.Runtime.DelegateSubscription.HostBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEventSubscriptionHostBoundaryTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	UWorld* OtherWorld = nullptr;
	if (!CreateEventSubscriptionWorld(
			TEXT("AvidScriptEventSubscriptionWorld"),
			World)
		|| !CreateEventSubscriptionWorld(
			TEXT("AvidScriptEventSubscriptionOtherWorld"),
			OtherWorld))
	{
		AddError(TEXT("Failed to create event subscription test worlds."));
		DestroyEventSubscriptionWorld(OtherWorld);
		DestroyEventSubscriptionWorld(World);
		return false;
	}

	AActor* const Owner = World->SpawnActor<AActor>();
	AActor* const Source = World->SpawnActor<AActor>();
	AActor* const Unauthorized = World->SpawnActor<AActor>();
	AActor* const ForeignWorldSource = OtherWorld->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Owner actor spawns"), Owner)
		|| !TestNotNull(TEXT("Source actor spawns"), Source)
		|| !TestNotNull(TEXT("Unauthorized actor spawns"), Unauthorized)
		|| !TestNotNull(TEXT("Foreign-world actor spawns"), ForeignWorldSource))
	{
		DestroyEventSubscriptionWorld(OtherWorld);
		DestroyEventSubscriptionWorld(World);
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult HandleResult;
	const FAvidScriptObjectHandle OwnerHandle =
		Registry.RegisterObject(Owner, HandleResult, false);
	const FAvidScriptObjectHandle SourceHandle =
		Registry.RegisterObject(Source, HandleResult, false);
	const FAvidScriptObjectHandle UnauthorizedHandle =
		Registry.RegisterObject(Unauthorized, HandleResult, false);
	const FAvidScriptObjectHandle ForeignWorldHandle =
		Registry.RegisterObject(ForeignWorldSource, HandleResult, false);

	FAvidScriptSessionObjectOwnership Ownership;
	TestTrue(
		TEXT("Source enters the session capability domain"),
		Ownership.Borrow(Registry, *Source, HandleResult));
	TestTrue(
		TEXT("Foreign-world source can be capability-authorized before world rejection"),
		Ownership.Borrow(Registry, *ForeignWorldSource, HandleResult));

	FAvidScriptEventSubscriptionHostSpy HostSpy;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ObjectOwnership = &Ownership;
	HostContext.OwnerHandle = OwnerHandle;
	HostContext.World = World;
	HostContext.EventSubscriptions = &HostSpy;

	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);
	FAvidScriptWasmSmokeResult LoadResult;
	if (!TestTrue(
			TEXT("Embedded runtime loads for direct host-boundary validation"),
			Runtime.LoadEmbeddedSmokeModule(LoadResult)))
	{
		Ownership.Cleanup(Registry);
		DestroyEventSubscriptionWorld(OtherWorld);
		DestroyEventSubscriptionWorld(World);
		return false;
	}

	const int64 Token = Runtime.HandleEventSubscribeImport(
		static_cast<int32>(SourceHandle.Slot),
		static_cast<int32>(SourceHandle.Generation),
		7);
	TestEqual(TEXT("Valid arbitrary source preserves the i64 token"), Token,
		FAvidScriptEventSubscriptionHostSpy::ExpectedToken);
	TestEqual(TEXT("Host receives one subscribe call"), HostSpy.SubscribeCallCount, 1);
	TestEqual(TEXT("Host receives the prepared event ordinal"), HostSpy.LastEventOrdinal, 7u);
	TestEqual(TEXT("Host receives the resolved source"), HostSpy.LastSource.Get(),
		static_cast<UObject*>(Source));

	TestEqual(
		TEXT("Unsubscribe forwards the full opaque token"),
		Runtime.HandleEventUnsubscribeImport(Token),
		1);
	TestEqual(TEXT("Host receives one unsubscribe call"), HostSpy.UnsubscribeCallCount, 1);
	TestEqual(TEXT("Unsubscribe token is not truncated"), HostSpy.LastUnsubscribeToken, Token);

	TestEqual(
		TEXT("A live handle outside the session capability domain is rejected"),
		Runtime.HandleEventSubscribeImport(
			static_cast<int32>(UnauthorizedHandle.Slot),
			static_cast<int32>(UnauthorizedHandle.Generation),
			7),
		static_cast<int64>(0));
	TestEqual(
		TEXT("A capability-authorized object from another world is rejected"),
		Runtime.HandleEventSubscribeImport(
			static_cast<int32>(ForeignWorldHandle.Slot),
			static_cast<int32>(ForeignWorldHandle.Generation),
			7),
		static_cast<int64>(0));
	TestEqual(
		TEXT("A stale generation is rejected"),
		Runtime.HandleEventSubscribeImport(
			static_cast<int32>(SourceHandle.Slot),
			static_cast<int32>(SourceHandle.Generation + 1),
			7),
		static_cast<int64>(0));
	TestEqual(
		TEXT("Rejected sources never reach the subscription owner"),
		HostSpy.SubscribeCallCount,
		1);

	Runtime.Unload();
	Ownership.Cleanup(Registry);
	DestroyEventSubscriptionWorld(OtherWorld);
	DestroyEventSubscriptionWorld(World);
	return true;
}

#endif
