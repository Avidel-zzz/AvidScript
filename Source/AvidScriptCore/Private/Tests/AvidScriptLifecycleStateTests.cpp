#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptLifecycleState.h"

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAvidScriptLifecycleStateValidTransitionsTest,
    "AvidScript.Architecture.Lifecycle.ValidTransitions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptLifecycleStateValidTransitionsTest::RunTest(const FString& Parameters)
{
    FAvidScriptLifecycleStateMachine StateMachine;
    FAvidScriptLifecycleTransitionResult Result;

    TestEqual(TEXT("Initial state is Empty"), StateMachine.GetState(), EAvidScriptLifecycleState::Empty);
    TestTrue(TEXT("Empty to Loaded"), StateMachine.TryTransition(EAvidScriptLifecycleState::Loaded, Result));
    TestTrue(TEXT("Loaded to Starting"), StateMachine.TryTransition(EAvidScriptLifecycleState::Starting, Result));
    TestTrue(TEXT("Starting to Running"), StateMachine.TryTransition(EAvidScriptLifecycleState::Running, Result));
    TestTrue(TEXT("Running to Stopping"), StateMachine.TryTransition(EAvidScriptLifecycleState::Stopping, Result));
    TestTrue(TEXT("Stopping to Stopped"), StateMachine.TryTransition(EAvidScriptLifecycleState::Stopped, Result));
    TestEqual(TEXT("Final state is Stopped"), StateMachine.GetState(), EAvidScriptLifecycleState::Stopped);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAvidScriptLifecycleStateInvalidTransitionsTest,
    "AvidScript.Architecture.Lifecycle.InvalidTransitions",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptLifecycleStateInvalidTransitionsTest::RunTest(const FString& Parameters)
{
    FAvidScriptLifecycleStateMachine StateMachine;
    FAvidScriptLifecycleTransitionResult Result;

    TestFalse(TEXT("Empty cannot skip directly to Running"), StateMachine.TryTransition(EAvidScriptLifecycleState::Running, Result));
    TestEqual(TEXT("Rejected transition preserves Empty"), StateMachine.GetState(), EAvidScriptLifecycleState::Empty);
    TestEqual(TEXT("Rejected transition reports previous state"), Result.PreviousState, EAvidScriptLifecycleState::Empty);
    TestEqual(TEXT("Rejected transition reports requested state"), Result.RequestedState, EAvidScriptLifecycleState::Running);

    TestTrue(TEXT("Empty to Loaded"), StateMachine.TryTransition(EAvidScriptLifecycleState::Loaded, Result));
    TestFalse(TEXT("Loaded cannot stop before start"), StateMachine.TryTransition(EAvidScriptLifecycleState::Stopping, Result));
    TestEqual(TEXT("Rejected stop preserves Loaded"), StateMachine.GetState(), EAvidScriptLifecycleState::Loaded);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
    FAvidScriptLifecycleStateFaultAndResetTest,
    "AvidScript.Architecture.Lifecycle.FaultAndReset",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptLifecycleStateFaultAndResetTest::RunTest(const FString& Parameters)
{
    FAvidScriptLifecycleStateMachine StateMachine;
    FAvidScriptLifecycleTransitionResult Result;

    TestTrue(TEXT("Empty to Loaded"), StateMachine.TryTransition(EAvidScriptLifecycleState::Loaded, Result));
    TestTrue(TEXT("Loaded to Starting"), StateMachine.TryTransition(EAvidScriptLifecycleState::Starting, Result));
    TestTrue(TEXT("Starting to Faulted"), StateMachine.MarkFaulted(Result));
    TestEqual(TEXT("Fault is observable"), StateMachine.GetState(), EAvidScriptLifecycleState::Faulted);
    TestTrue(TEXT("Repeated fault is idempotent"), StateMachine.MarkFaulted(Result));
    TestFalse(TEXT("Repeated fault does not change state"), Result.bChanged);

    StateMachine.Reset();
    TestEqual(TEXT("Reset returns to Empty"), StateMachine.GetState(), EAvidScriptLifecycleState::Empty);
    TestTrue(TEXT("Repeated Empty transition is idempotent"), StateMachine.TryTransition(EAvidScriptLifecycleState::Empty, Result));
    TestFalse(TEXT("Idempotent transition reports no change"), Result.bChanged);
    return true;
}

#endif
