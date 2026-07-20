#if WITH_DEV_AUTOMATION_TESTS

#include "HostEffects/AvidScriptHostEffectTransaction.h"

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptObjectRegistryTestTypes.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace
{
bool CreateHostEffectWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptHostEffectWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyHostEffectWorld(UWorld*& World)
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

FAvidScriptObjectHandle RegisterHostEffectObject(
	FAvidScriptObjectRegistry& Registry,
	UObject& Object,
	FAutomationTestBase& Test)
{
	FAvidScriptObjectHandleResult Result;
	const FAvidScriptObjectHandle Handle = Registry.RegisterObject(&Object, Result);
	Test.TestTrue(TEXT("Host effect object registers"), Result.bSucceeded);
	return Handle;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptHostEffectTransactionRollbackTest,
	"AvidScript.Architecture.HostEffects.Rollback",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptHostEffectTransactionRollbackTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateHostEffectWorld(World))
	{
		AddError(TEXT("Failed to create host effect transaction world."));
		return true;
	}

	AAvidScriptActorBindingTestActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	if (!TestNotNull(TEXT("Host effect actor spawns"), Actor))
	{
		DestroyHostEffectWorld(World);
		return true;
	}

	const FTransform InitialTransform(
		FRotator(5.0, 15.0, 25.0),
		FVector(10.0, 20.0, 30.0),
		FVector(1.0, 1.5, 2.0));
	Actor->SetActorTransform(InitialTransform, false, nullptr, ETeleportType::TeleportPhysics);

	FAvidScriptObjectRegistry Registry;
	const FAvidScriptObjectHandle ActorHandle = RegisterHostEffectObject(Registry, *Actor, *this);
	USceneComponent* RootComponent = Actor->GetRootComponent();
	if (!TestNotNull(TEXT("Host effect actor has root component"), RootComponent))
	{
		DestroyHostEffectWorld(World);
		return true;
	}
	const FAvidScriptObjectHandle ComponentHandle = RegisterHostEffectObject(Registry, *RootComponent, *this);

	FAvidScriptHostEffectTransaction Transaction;
	FAvidScriptBindingHostEffectPrepareResult PrepareResult;
	TestTrue(TEXT("First actor write captures snapshot"), Transaction.PrepareEffect(
		Registry,
		ActorHandle,
		*Actor,
		EAvidScriptBindingReloadEffect::ActorTransform,
		PrepareResult));
	TestTrue(TEXT("Repeated actor write reuses snapshot"), Transaction.PrepareEffect(
		Registry,
		ActorHandle,
		*Actor,
		EAvidScriptBindingReloadEffect::ActorTransform,
		PrepareResult));
	TestEqual(TEXT("One actor snapshot"), Transaction.GetCapturedObjectCount(), 1);

	const FTransform ActorMutation(
		FRotator(35.0, 45.0, 55.0),
		FVector(100.0, 200.0, 300.0),
		FVector(2.0, 2.5, 3.0));
	Actor->SetActorTransform(ActorMutation, false, nullptr, ETeleportType::TeleportPhysics);
	TestTrue(TEXT("Component write captures its own domain snapshot"), Transaction.PrepareEffect(
		Registry,
		ComponentHandle,
		*RootComponent,
		EAvidScriptBindingReloadEffect::SceneComponentTransform,
		PrepareResult));
	RootComponent->SetWorldTransform(
		FTransform(FRotator(65.0, 75.0, 85.0), FVector(400.0, 500.0, 600.0), FVector(4.0)),
		false,
		nullptr,
		ETeleportType::TeleportPhysics);

	FAvidScriptHostEffectTransactionResult RollbackResult;
	TestTrue(TEXT("Rollback restores all captured effects"), Transaction.Rollback(Registry, RollbackResult));
	TestTrue(TEXT("Rollback reports success"), RollbackResult.bSucceeded);
	TestEqual(TEXT("Rollback captured two domains"), RollbackResult.CapturedObjectCount, 2);
	TestEqual(TEXT("Rollback restored two domains"), RollbackResult.RestoredObjectCount, 2);
	TestEqual(TEXT("Rollback has no failures"), RollbackResult.FailedObjectCount, 0);
	TestTrue(TEXT("Actor returns to pre-candidate transform"), Actor->GetActorTransform().Equals(InitialTransform, 0.01));
	TestEqual(TEXT("Transaction is rolled back"), Transaction.GetState(), EAvidScriptHostEffectTransactionState::RolledBack);

	FAvidScriptHostEffectTransactionResult RepeatedResult;
	TestFalse(TEXT("Repeated rollback is rejected"), Transaction.Rollback(Registry, RepeatedResult));
	TestEqual(TEXT("Repeated rollback reports closed state"), RepeatedResult.ErrorCategory, FString(TEXT("host_effect_transaction_closed")));

	DestroyHostEffectWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptHostEffectTransactionFailureAndCommitTest,
	"AvidScript.Architecture.HostEffects.FailureAndCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptHostEffectTransactionFailureAndCommitTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateHostEffectWorld(World))
	{
		AddError(TEXT("Failed to create host effect failure world."));
		return true;
	}

	AAvidScriptActorBindingTestActor* FirstActor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	AAvidScriptActorBindingTestActor* SecondActor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	if (!TestNotNull(TEXT("First failure actor spawns"), FirstActor)
		|| !TestNotNull(TEXT("Second failure actor spawns"), SecondActor))
	{
		DestroyHostEffectWorld(World);
		return true;
	}

	const FTransform FirstInitial(FRotator::ZeroRotator, FVector(1.0, 2.0, 3.0), FVector::OneVector);
	const FTransform SecondInitial(FRotator::ZeroRotator, FVector(4.0, 5.0, 6.0), FVector::OneVector);
	FirstActor->SetActorTransform(FirstInitial);
	SecondActor->SetActorTransform(SecondInitial);

	FAvidScriptObjectRegistry Registry;
	const FAvidScriptObjectHandle FirstHandle = RegisterHostEffectObject(Registry, *FirstActor, *this);
	const FAvidScriptObjectHandle SecondHandle = RegisterHostEffectObject(Registry, *SecondActor, *this);

	FAvidScriptHostEffectTransaction Transaction;
	FAvidScriptBindingHostEffectPrepareResult PrepareResult;
	TestTrue(TEXT("First rollback entry captures"), Transaction.PrepareEffect(
		Registry, FirstHandle, *FirstActor, EAvidScriptBindingReloadEffect::ActorTransform, PrepareResult));
	TestTrue(TEXT("Second rollback entry captures"), Transaction.PrepareEffect(
		Registry, SecondHandle, *SecondActor, EAvidScriptBindingReloadEffect::ActorTransform, PrepareResult));
	FirstActor->SetActorLocation(FVector(100.0, 200.0, 300.0));
	SecondActor->SetActorLocation(FVector(400.0, 500.0, 600.0));

	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(TEXT("Second handle releases after capture"), Registry.ReleaseHandle(SecondHandle, ReleaseResult));
	FAvidScriptHostEffectTransactionResult RollbackResult;
	TestFalse(TEXT("Rollback reports a stale captured handle"), Transaction.Rollback(Registry, RollbackResult));
	TestEqual(TEXT("One valid entry is still restored"), RollbackResult.RestoredObjectCount, 1);
	TestEqual(TEXT("One stale entry fails"), RollbackResult.FailedObjectCount, 1);
	TestTrue(TEXT("First entry restores despite later-entry failure"), FirstActor->GetActorTransform().Equals(FirstInitial, 0.01));
	TestEqual(TEXT("First failure source is stale handle"), RollbackResult.ErrorSource, FString::Printf(
		TEXT("%u:%u"), SecondHandle.Slot, SecondHandle.Generation));

	FAvidScriptHostEffectTransaction StalePrepareTransaction;
	TestFalse(TEXT("Stale handle is rejected before capture"), StalePrepareTransaction.PrepareEffect(
		Registry, SecondHandle, *SecondActor, EAvidScriptBindingReloadEffect::ActorTransform, PrepareResult));
	TestEqual(TEXT("Stale prepare reports invalid handle"), PrepareResult.ErrorCategory, FString(TEXT("host_effect_handle_invalid")));
	TestFalse(TEXT("Unsupported effect fails closed"), StalePrepareTransaction.PrepareEffect(
		Registry, FirstHandle, *FirstActor, EAvidScriptBindingReloadEffect::Unsupported, PrepareResult));
	TestEqual(TEXT("Unsupported effect has stable category"), PrepareResult.ErrorCategory, FString(TEXT("binding_reload_effect_unsupported")));

	FAvidScriptHostEffectTransaction CommitTransaction;
	TestTrue(TEXT("Committed entry captures"), CommitTransaction.PrepareEffect(
		Registry, FirstHandle, *FirstActor, EAvidScriptBindingReloadEffect::ActorTransform, PrepareResult));
	const FVector CommittedLocation(700.0, 800.0, 900.0);
	FirstActor->SetActorLocation(CommittedLocation);
	FAvidScriptHostEffectTransactionResult CommitResult;
	TestTrue(TEXT("Open transaction commits"), CommitTransaction.Commit(CommitResult));
	TestEqual(TEXT("Commit preserves candidate write"), FirstActor->GetActorLocation(), CommittedLocation);
	TestEqual(TEXT("Transaction is committed"), CommitTransaction.GetState(), EAvidScriptHostEffectTransactionState::Committed);
	FAvidScriptHostEffectTransactionResult ClosedResult;
	TestFalse(TEXT("Rollback after commit is rejected"), CommitTransaction.Rollback(Registry, ClosedResult));
	TestEqual(TEXT("Rollback after commit reports closed state"), ClosedResult.ErrorCategory, FString(TEXT("host_effect_transaction_closed")));

	DestroyHostEffectWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
