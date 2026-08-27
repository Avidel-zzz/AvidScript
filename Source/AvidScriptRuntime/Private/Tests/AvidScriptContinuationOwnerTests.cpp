#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptSessionContinuations.h"
#include "AvidScriptWasmRuntime.h"

#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace
{
class FAvidScriptContinuationHostSpy final : public IAvidScriptContinuationHost
{
public:
	int64 ScheduleDelay(const float DelaySeconds, const int32 CallbackId) override
	{
		LastDelaySeconds = DelaySeconds;
		LastCallbackId = CallbackId;
		++ScheduleCount;
		return ExpectedToken;
	}

	bool Cancel(const int64 Token) override
	{
		LastCancelledToken = Token;
		++CancelCount;
		return Token == ExpectedToken;
	}

	static constexpr int64 ExpectedToken = 0x000000010000002aLL;
	float LastDelaySeconds = 0.0f;
	int32 LastCallbackId = 0;
	int64 LastCancelledToken = 0;
	int32 ScheduleCount = 0;
	int32 CancelCount = 0;
};

bool CreateContinuationWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptContinuationWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	OutWorld->InitializeActorsForPlay(FURL());
	return true;
}

void DestroyContinuationWorld(UWorld*& World)
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

void AdvanceContinuationWorld(UWorld* World, const float ElapsedSeconds)
{
	World->Tick(LEVELTICK_All, 0.0f);
	++GFrameCounter;
	World->Tick(LEVELTICK_All, ElapsedSeconds);
	++GFrameCounter;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationHostBoundaryTest,
	"AvidScript.Runtime.Continuation.HostBoundary",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationHostBoundaryTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptContinuationHostSpy Host;
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext Context;
	Context.Continuations = &Host;
	Runtime.SetHostContext(Context);

	const int64 Token = Runtime.HandleContinuationDelayImport(0.25f, 17);
	TestEqual(TEXT("Runtime preserves the full i64 continuation token"), Token, Host.ExpectedToken);
	TestEqual(TEXT("Runtime forwards the delay"), Host.LastDelaySeconds, 0.25f);
	TestEqual(TEXT("Runtime forwards the callback id"), Host.LastCallbackId, 17);
	TestEqual(TEXT("Schedule crosses the host boundary once"), Host.ScheduleCount, 1);
	TestEqual(
		TEXT("Runtime forwards the full token to cancel"),
		Runtime.HandleContinuationCancelImport(Token),
		1);
	TestEqual(TEXT("Cancel crosses the host boundary once"), Host.CancelCount, 1);
	TestEqual(TEXT("Cancel receives the exact token"), Host.LastCancelledToken, Token);
	TestEqual(
		TEXT("A rejected forged token remains a normal false result"),
		Runtime.HandleContinuationCancelImport(Token + 1),
		0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationActiveLifecycleTest,
	"AvidScript.Runtime.Continuation.ActiveLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationActiveLifecycleTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(TEXT("Continuation test world is created"), CreateContinuationWorld(World)))
	{
		return false;
	}

	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	IAvidScriptContinuationHost& Host = Owner->ResetActive(World);
	const int64 CancelledToken = Host.ScheduleDelay(10.0f, 1);
	TestNotEqual(TEXT("A delay returns an opaque token"), CancelledToken, 0LL);
	TestEqual(TEXT("One active continuation is tracked"), Owner->GetActiveCount(), 1);
	TestTrue(TEXT("The active token can be cancelled"), Host.Cancel(CancelledToken));
	TestFalse(TEXT("A cancelled token is stale"), Host.Cancel(CancelledToken));

	const int64 FirstToken = Host.ScheduleDelay(0.01f, 11);
	const int64 SecondToken = Host.ScheduleDelay(0.01f, 12);
	TestNotEqual(TEXT("First ordered token is valid"), FirstToken, 0LL);
	TestNotEqual(TEXT("Second ordered token is valid"), SecondToken, 0LL);
	AdvanceContinuationWorld(World, 0.02f);

	TArray<FAvidScriptContinuationCompletion> Completions;
	Owner->DrainReady(Completions);
	TestEqual(TEXT("One continuation is delivered per safe pump"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TestEqual(TEXT("Equal deadlines preserve registration order"), Completions[0].CallbackId, 11);
	}
	Owner->DrainReady(Completions);
	TestEqual(TEXT("The second safe pump delivers the next continuation"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TestEqual(TEXT("Second completion follows the first"), Completions[0].CallbackId, 12);
	}
	TestFalse(TEXT("Completed tokens cannot be cancelled"), Host.Cancel(FirstToken));
	TestEqual(TEXT("Completed continuations leave no active entries"), Owner->GetActiveCount(), 0);
	const int64 ReadyToken = Host.ScheduleDelay(0.01f, 13);
	AdvanceContinuationWorld(World, 0.02f);
	TestTrue(TEXT("Cancellation can suppress a ready but not yet delivered callback"), Host.Cancel(ReadyToken));
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Cancelled ready callback is not delivered"), Completions.Num(), 0);

	Owner->Teardown();
	DestroyContinuationWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptContinuationPreparedTransactionTest,
	"AvidScript.Runtime.Continuation.PreparedTransaction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptContinuationPreparedTransactionTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(TEXT("Continuation transaction world is created"), CreateContinuationWorld(World)))
	{
		return false;
	}

	const TSharedPtr<FAvidScriptSessionContinuations> Owner =
		MakeShared<FAvidScriptSessionContinuations>();
	IAvidScriptContinuationHost& ActiveHost = Owner->ResetActive(World);
	const int64 ActiveToken = ActiveHost.ScheduleDelay(10.0f, 21);
	IAvidScriptContinuationHost& RejectedCandidate = Owner->BeginPrepared(World);
	const int64 RejectedToken = RejectedCandidate.ScheduleDelay(10.0f, 22);
	TestNotEqual(TEXT("Rejected candidate initially receives a token"), RejectedToken, 0LL);
	TestEqual(TEXT("Candidate uses an isolated prepared lane"), Owner->GetPreparedCount(), 1);
	Owner->DiscardPrepared();
	TestEqual(TEXT("Rejected candidate entries are discarded"), Owner->GetPreparedCount(), 0);
	TestEqual(TEXT("Rejected candidate preserves the active lane"), Owner->GetActiveCount(), 1);
	TestTrue(TEXT("Old active token remains cancellable"), ActiveHost.Cancel(ActiveToken));

	IAvidScriptContinuationHost& CandidateHost = Owner->BeginPrepared(World);
	const int64 CandidateToken = CandidateHost.ScheduleDelay(0.01f, 31);
	AdvanceContinuationWorld(World, 0.02f);
	FString ValidationError;
	TestTrue(
		TEXT("Completed prepared work remains commit-valid"),
		Owner->ValidatePreparedCommit(ValidationError));
	Owner->CommitPrepared();
	TArray<FAvidScriptContinuationCompletion> Completions;
	Owner->DrainReady(Completions);
	TestEqual(TEXT("Committed prepared completion is promoted once"), Completions.Num(), 1);
	if (Completions.Num() == 1)
	{
		TestEqual(TEXT("Promoted completion keeps its callback id"), Completions[0].CallbackId, 31);
		TestEqual(TEXT("Promoted completion keeps its token"), Completions[0].Token, CandidateToken);
	}
	TestFalse(TEXT("Completed promoted token is stale"), CandidateHost.Cancel(CandidateToken));

	Owner->Teardown();
	DestroyContinuationWorld(World);
	return true;
}

#endif
