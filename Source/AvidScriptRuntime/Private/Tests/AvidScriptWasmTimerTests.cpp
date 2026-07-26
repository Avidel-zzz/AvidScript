#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"

#include "AvidScriptRuntimeBackendTestLanes.h"
#include "Containers/Set.h"
#include "Math/NumericLimits.h"
#include "Misc/AutomationTest.h"

namespace
{
void AppendTimerU32Leb(TArray<uint8>& Bytes, uint32 Value)
{
	do
	{
		uint8 Byte = static_cast<uint8>(Value & 0x7f);
		Value >>= 7;
		if (Value != 0)
		{
			Byte |= 0x80;
		}
		Bytes.Add(Byte);
	} while (Value != 0);
}

void AppendTimerString(TArray<uint8>& Bytes, const char* Text)
{
	const int32 Length = static_cast<int32>(FCStringAnsi::Strlen(Text));
	AppendTimerU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Text[Index]));
	}
}

void AppendTimerSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendTimerU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

TArray<uint8> BuildTimerCallbackFixture(bool bTrap, bool bReschedule = true)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> TypeSection;
	AppendTimerU32Leb(TypeSection, 4);
	const uint8 Types[] = {
		0x60, 0x02, 0x7d, 0x7f, 0x01, 0x7f,
		0x60, 0x00, 0x00,
		0x60, 0x01, 0x7d, 0x00,
		0x60, 0x02, 0x7f, 0x7f, 0x00
	};
	TypeSection.Append(Types, UE_ARRAY_COUNT(Types));
	AppendTimerSection(Module, 1, TypeSection);

	TArray<uint8> ImportSection;
	AppendTimerU32Leb(ImportSection, 1);
	AppendTimerString(ImportSection, "env");
	AppendTimerString(ImportSection, "timer_set_once");
	ImportSection.Add(0x00);
	AppendTimerU32Leb(ImportSection, 0);
	AppendTimerSection(Module, 2, ImportSection);

	TArray<uint8> FunctionSection;
	AppendTimerU32Leb(FunctionSection, 3);
	AppendTimerU32Leb(FunctionSection, 1);
	AppendTimerU32Leb(FunctionSection, 2);
	AppendTimerU32Leb(FunctionSection, 3);
	AppendTimerSection(Module, 3, FunctionSection);

	TArray<uint8> ExportSection;
	AppendTimerU32Leb(ExportSection, 3);
	AppendTimerString(ExportSection, "avid_on_begin_play");
	ExportSection.Add(0x00);
	AppendTimerU32Leb(ExportSection, 1);
	AppendTimerString(ExportSection, "avid_on_tick");
	ExportSection.Add(0x00);
	AppendTimerU32Leb(ExportSection, 2);
	AppendTimerString(ExportSection, "avid_on_timer");
	ExportSection.Add(0x00);
	AppendTimerU32Leb(ExportSection, 3);
	AppendTimerSection(Module, 7, ExportSection);

	TArray<uint8> BeginPlayBody = { 0x00, 0x43, 0x00, 0x00, 0x00, 0x00, 0x41, 0x07, 0x10, 0x00, 0x1a, 0x0b };
	TArray<uint8> TickBody = { 0x00, 0x0b };
	TArray<uint8> TimerBody;
	if (bTrap)
	{
		TimerBody = { 0x00, 0x00, 0x0b };
	}
	else if (bReschedule)
	{
		TimerBody = { 0x00, 0x43, 0x00, 0x00, 0x00, 0x00, 0x41, 0x08, 0x10, 0x00, 0x1a, 0x0b };
	}
	else
	{
		TimerBody = { 0x00, 0x0b };
	}
	TArray<uint8> CodeSection;
	AppendTimerU32Leb(CodeSection, 3);
	AppendTimerU32Leb(CodeSection, static_cast<uint32>(BeginPlayBody.Num()));
	CodeSection.Append(BeginPlayBody);
	AppendTimerU32Leb(CodeSection, static_cast<uint32>(TickBody.Num()));
	CodeSection.Append(TickBody);
	AppendTimerU32Leb(CodeSection, static_cast<uint32>(TimerBody.Num()));
	CodeSection.Append(TimerBody);
	AppendTimerSection(Module, 10, CodeSection);

	return Module;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmTimerRegistrationAndCancellationSmokeTest,
	"AvidScript.Runtime.Timer.RegistrationAndCancellationSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmTimerRegistrationAndCancellationSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("Embedded module loads"), Runtime.LoadEmbeddedSmokeModule(Result));
	TestTrue(TEXT("Embedded BeginPlay succeeds"), Runtime.BeginPlay(Result));

	const int32 TimerHandle = Runtime.HandleTimerSetOnceImport(1.0f, 42);
	TestTrue(TEXT("Valid timer registration returns a handle"), TimerHandle > 0);
	TestEqual(TEXT("Timer registration increments pending count"), Runtime.GetPendingTimerCount(), 1);
	TestEqual(TEXT("Timer cancellation succeeds"), Runtime.HandleTimerCancelImport(TimerHandle), 1);
	TestEqual(TEXT("Timer cancellation removes pending timer"), Runtime.GetPendingTimerCount(), 0);
	TestEqual(TEXT("Cancelling an unknown timer is a no-op"), Runtime.HandleTimerCancelImport(TimerHandle), 0);
	TestEqual(TEXT("Negative delay is rejected"), Runtime.HandleTimerSetOnceImport(-0.01f, 1), 0);
	TestEqual(TEXT("NaN delay is rejected"), Runtime.HandleTimerSetOnceImport(std::numeric_limits<float>::quiet_NaN(), 1), 0);
	TestEqual(TEXT("Negative callback id is rejected"), Runtime.HandleTimerSetOnceImport(0.1f, -1), 0);

	TSet<int32> TimerHandles;
	bool bAllCapacityTimersRegistered = true;
	for (int32 TimerIndex = 0; TimerIndex < 1024; ++TimerIndex)
	{
		const int32 CapacityHandle = Runtime.HandleTimerSetOnceImport(60.0f, TimerIndex);
		bAllCapacityTimersRegistered &= CapacityHandle > 0;
		TimerHandles.Add(CapacityHandle);
	}
	TestTrue(TEXT("All timers up to the capacity register"), bAllCapacityTimersRegistered);
	TestEqual(TEXT("Timer handles are unique at capacity"), TimerHandles.Num(), 1024);
	TestEqual(TEXT("Pending timer count reaches the documented capacity"), Runtime.GetPendingTimerCount(), 1024);
	TestEqual(TEXT("Timer registration fails closed above capacity"), Runtime.HandleTimerSetOnceImport(60.0f, 1024), 0);

	for (const int32 CapacityHandle : TimerHandles)
	{
		TestEqual(TEXT("Capacity timer cancellation succeeds"), Runtime.HandleTimerCancelImport(CapacityHandle), 1);
	}
	TestEqual(TEXT("Cancelling capacity timers clears the active set"), Runtime.GetPendingTimerCount(), 0);

	bool bCapacityReusableAfterCancellation = true;
	for (int32 TimerIndex = 0; TimerIndex < 1024; ++TimerIndex)
	{
		bCapacityReusableAfterCancellation &= Runtime.HandleTimerSetOnceImport(60.0f, TimerIndex) > 0;
	}
	TestTrue(TEXT("Timer capacity is reusable after cancellation churn"), bCapacityReusableAfterCancellation);
	TestEqual(TEXT("Reused timer capacity reaches the limit"), Runtime.GetPendingTimerCount(), 1024);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmTimerExpirationAndUnloadSmokeTest,
	"AvidScript.Runtime.Timer.ExpirationAndUnloadSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmTimerExpirationAndUnloadSmokeTest::RunTest(const FString& Parameters)
{
	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult Result;
		TestTrue(TEXT("Embedded module loads"), Runtime.LoadEmbeddedSmokeModule(Result));
		TestTrue(TEXT("Embedded BeginPlay succeeds"), Runtime.BeginPlay(Result));
		const int32 TimerHandle = Runtime.HandleTimerSetOnceImport(0.0f, 7);
		TestTrue(TEXT("Zero-delay timer registers"), TimerHandle > 0);

		TestFalse(TEXT("Due timer without callback export fails closed"), Runtime.Tick(1.0f / 60.0f, Result));
		TestEqual(TEXT("Missing callback export category"), Result.ErrorCategory, FString(TEXT("missing_export")));
		TestEqual(TEXT("Missing callback export name"), Result.ExportName, FString(TEXT("avid_on_timer")));
		TestEqual(TEXT("Due one-shot timer is removed before callback"), Runtime.GetPendingTimerCount(), 0);
		TestEqual(TEXT("Failed callback is not counted"), Runtime.GetTimerCallbackCount(), 0);
	}

	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult Result;
		TestTrue(TEXT("Embedded module loads for cancellation"), Runtime.LoadEmbeddedSmokeModule(Result));
		TestTrue(TEXT("Embedded BeginPlay succeeds for cancellation"), Runtime.BeginPlay(Result));
		const int32 CancelledHandle = Runtime.HandleTimerSetOnceImport(0.0f, 8);
		TestEqual(TEXT("Due timer cancels before Tick"), Runtime.HandleTimerCancelImport(CancelledHandle), 1);
		TestTrue(TEXT("Cancelled timer does not require callback export"), Runtime.Tick(1.0f / 60.0f, Result));

		TestTrue(TEXT("Long timer registers before unload"), Runtime.HandleTimerSetOnceImport(10.0f, 9) > 0);
		Runtime.Unload(Result);
		TestEqual(TEXT("Unload clears pending timers"), Runtime.GetPendingTimerCount(), 0);
		TestEqual(TEXT("Unload resets callback count"), Runtime.GetTimerCallbackCount(), 0);
		TestEqual(TEXT("Unload result preserves pre-unload callback count"), Result.TimerCallbackCount, 0);
	}
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmTimerCallbackTrapSmokeTest,
	"AvidScript.Runtime.Timer.CallbackTrapSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmTimerCallbackTrapSmokeTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> WasmBytes = BuildTimerCallbackFixture(true);
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("Timer trap fixture loads"), Runtime.LoadModule(
		WasmBytes.GetData(), WasmBytes.Num(), TEXT("timer_callback_trap"), Result));
	TestTrue(TEXT("BeginPlay schedules the timer"), Runtime.BeginPlay(Result));
	TestEqual(TEXT("BeginPlay leaves one pending timer"), Runtime.GetPendingTimerCount(), 1);

	TestFalse(TEXT("Timer callback trap fails the Tick"), Runtime.Tick(1.0f / 60.0f, Result));
	TestEqual(TEXT("Timer callback trap category"), Result.ErrorCategory, FString(TEXT("trap")));
	TestEqual(TEXT("Timer callback trap export"), Result.ExportName, FString(TEXT("avid_on_timer")));
	TestEqual(TEXT("Trapped one-shot timer is removed"), Runtime.GetPendingTimerCount(), 0);
	TestEqual(TEXT("Trapped callback is not counted"), Runtime.GetTimerCallbackCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmTimerCallbackRescheduleSmokeTest,
	"AvidScript.Runtime.Timer.CallbackRescheduleNextFrameSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmTimerCallbackRescheduleSmokeTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> WasmBytes = BuildTimerCallbackFixture(false);
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("timer reschedule fixture loads")),
			Runtime.LoadModule(
				WasmBytes.GetData(),
				WasmBytes.Num(),
				TEXT("timer_callback_reschedule"),
				Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("BeginPlay schedules first timer")), Runtime.BeginPlay(Result));
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("first Tick executes callback")), Runtime.Tick(1.0f / 60.0f, Result));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("first callback count")), Result.TimerCallbackCount, 1);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("first callback id")), Result.LastTimerCallbackId, 7);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("callback reschedule waits pending")), Runtime.GetPendingTimerCount(), 1);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("second Tick executes deferred callback")), Runtime.Tick(1.0f / 60.0f, Result));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("second callback count")), Result.TimerCallbackCount, 2);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("deferred callback id")), Result.LastTimerCallbackId, 8);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("rescheduled timer waits next frame")), Runtime.GetPendingTimerCount(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmTimerDeterministicOrderSmokeTest,
	"AvidScript.Runtime.Timer.DeterministicDeadlineOrderSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmTimerDeterministicOrderSmokeTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> WasmBytes = BuildTimerCallbackFixture(false, false);
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("Timer order fixture loads"), Runtime.LoadModule(
		WasmBytes.GetData(), WasmBytes.Num(), TEXT("timer_deterministic_order"), Result));
	TestTrue(TEXT("BeginPlay schedules the first timer"), Runtime.BeginPlay(Result));
	const int32 SecondHandle = Runtime.HandleTimerSetOnceImport(0.0f, 20);
	const int32 ThirdHandle = Runtime.HandleTimerSetOnceImport(0.0f, 21);
	TestTrue(TEXT("Second timer handle follows the first"), SecondHandle > 1);
	TestTrue(TEXT("Third timer handle follows the second"), ThirdHandle > SecondHandle);

	TestTrue(TEXT("Tick executes timers sharing a deadline"), Runtime.Tick(1.0f / 60.0f, Result));
	TestEqual(TEXT("All same-deadline callbacks execute"), Result.TimerCallbackCount, 3);
	TestEqual(TEXT("The highest handle executes last"), Result.LastTimerHandle, ThirdHandle);
	TestEqual(TEXT("The highest-handle callback id executes last"), Result.LastTimerCallbackId, 21);
	TestEqual(TEXT("All one-shot timers are removed"), Runtime.GetPendingTimerCount(), 0);
	return true;
}

#endif
