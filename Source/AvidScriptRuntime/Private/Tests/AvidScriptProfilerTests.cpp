#include "Profiling/AvidScriptProfiler.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptProfilerEventBufferTest,
	"AvidScript.Runtime.Profiling.EventBuffer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptProfilerEventBufferTest::RunTest(const FString& Parameters)
{
	FAvidScriptProfilerEventBuffer Buffer(3);
	Buffer.Record(
		EAvidScriptProfilerEventKind::GuestCall,
		1,
		10,
		2);
	TestTrue(TEXT("disabled profiler buffer stays empty"), Buffer.Snapshot().Events.IsEmpty());

	Buffer.SetBufferEnabled(true);
	for (uint32 Index = 0; Index < 5; ++Index)
	{
		Buffer.Record(
			EAvidScriptProfilerEventKind::HostCall,
			100 + Index,
			1000 + Index,
			10 + Index,
			7,
			0x12340000ULL + Index,
			42,
			Index,
			Index != 4);
	}

	const FAvidScriptProfilerSnapshot Snapshot = Buffer.Snapshot();
	TestEqual(TEXT("snapshot remains bounded"), Snapshot.Events.Num(), 3);
	TestEqual(TEXT("overflow reports overwritten events"), Snapshot.DroppedEventCount, 2ULL);
	TestEqual(TEXT("snapshot starts at oldest retained sequence"), Snapshot.Events[0].Sequence, 3ULL);
	TestEqual(TEXT("snapshot ends at latest sequence"), Snapshot.Events[2].Sequence, 5ULL);
	TestEqual(TEXT("event keeps operation id"), Snapshot.Events[2].OperationId, 104U);
	TestEqual(TEXT("event keeps source probe id"), Snapshot.Events[2].ProbeId, 0x12340004ULL);
	TestFalse(TEXT("event keeps failure outcome"), Snapshot.Events[2].bSucceeded);
	TestTrue(TEXT("snapshot publishes cycle conversion"), Snapshot.SecondsPerCycle > 0.0);
	TestEqual(TEXT("single writer accepts every event"), Snapshot.RejectedThreadEventCount, 0ULL);

	Buffer.Reset();
	TestTrue(TEXT("reset clears retained events"), Buffer.Snapshot().Events.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptProfilerScopeTest,
	"AvidScript.Runtime.Profiling.Scope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptProfilerScopeTest::RunTest(const FString& Parameters)
{
	FAvidScriptProfilerEventBuffer Buffer(4);
	{
		FAvidScriptProfilerScope DisabledScope(
			&Buffer,
			EAvidScriptProfilerEventKind::GuestCall,
			9);
		TestFalse(TEXT("disabled scope does not read cycles"), DisabledScope.IsCapturing());
	}
	TestTrue(TEXT("disabled scope emits no buffered event"), Buffer.Snapshot().Events.IsEmpty());

	Buffer.SetBufferEnabled(true);
	{
		FAvidScriptProfilerScope Scope(
			&Buffer,
			EAvidScriptProfilerEventKind::Continuation,
			17,
			3,
			0xabcdefULL,
			91,
			5);
		TestTrue(TEXT("enabled scope captures cycles"), Scope.IsCapturing());
		Scope.SetSucceeded(false);
	}

	const FAvidScriptProfilerSnapshot Snapshot = Buffer.Snapshot();
	TestEqual(TEXT("scope emits one event"), Snapshot.Events.Num(), 1);
	TestEqual(
		TEXT("scope keeps kind"),
		Snapshot.Events[0].Kind,
		EAvidScriptProfilerEventKind::Continuation);
	TestEqual(TEXT("scope keeps epoch"), Snapshot.Events[0].Epoch, 3ULL);
	TestEqual(TEXT("scope keeps correlation"), Snapshot.Events[0].CorrelationId, 91ULL);
	TestFalse(TEXT("scope keeps outcome"), Snapshot.Events[0].bSucceeded);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
