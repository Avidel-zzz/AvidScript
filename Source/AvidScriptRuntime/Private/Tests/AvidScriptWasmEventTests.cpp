#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"

#include "Misc/AutomationTest.h"

#include <limits>
namespace
{
void AppendEventU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendEventString(TArray<uint8>& Bytes, const char* Text)
{
	const int32 Length = static_cast<int32>(FCStringAnsi::Strlen(Text));
	AppendEventU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Text[Index]));
	}
}

void AppendEventSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendEventU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

TArray<uint8> BuildEventFixture(bool bTrap)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> Types;
	const uint8 TypeBytes[] = {
		0x03,
		0x60, 0x00, 0x00,
		0x60, 0x01, 0x7d, 0x00,
		0x60, 0x02, 0x7f, 0x7d, 0x00
	};
	Types.Append(TypeBytes, UE_ARRAY_COUNT(TypeBytes));
	AppendEventSection(Module, 1, Types);

	TArray<uint8> Functions;
	const uint8 FunctionBytes[] = { 0x03, 0x00, 0x01, 0x02 };
	Functions.Append(FunctionBytes, UE_ARRAY_COUNT(FunctionBytes));
	AppendEventSection(Module, 3, Functions);

	TArray<uint8> Exports;
	AppendEventU32Leb(Exports, 3);
	AppendEventString(Exports, "avid_on_begin_play");
	Exports.Add(0x00);
	Exports.Add(0x00);
	AppendEventString(Exports, "avid_on_tick");
	Exports.Add(0x00);
	Exports.Add(0x01);
	AppendEventString(Exports, "avid_on_event");
	Exports.Add(0x00);
	Exports.Add(0x02);
	AppendEventSection(Module, 7, Exports);

	TArray<uint8> Code;
	const uint8 Prefix[] = { 0x03, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b };
	Code.Append(Prefix, UE_ARRAY_COUNT(Prefix));
	if (bTrap)
	{
		const uint8 TrapBody[] = { 0x03, 0x00, 0x00, 0x0b };
		Code.Append(TrapBody, UE_ARRAY_COUNT(TrapBody));
	}
	else
	{
		const uint8 SuccessBody[] = { 0x02, 0x00, 0x0b };
		Code.Append(SuccessBody, UE_ARRAY_COUNT(SuccessBody));
	}
	AppendEventSection(Module, 10, Code);
	return Module;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmEventValidationAndMissingExportSmokeTest,
	"AvidScript.Runtime.Event.ValidationAndMissingExportSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmEventValidationAndMissingExportSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("Embedded module loads"), Runtime.LoadEmbeddedSmokeModule(Result));
	TestTrue(TEXT("Embedded BeginPlay succeeds"), Runtime.BeginPlay(Result));

	TestFalse(TEXT("Negative event id fails closed"), Runtime.DispatchEvent(-1, 1.0f, Result));
	TestEqual(TEXT("Negative event id category"), Result.ErrorCategory, FString(TEXT("invalid_argument")));
	TestFalse(TEXT("NaN event value fails closed"), Runtime.DispatchEvent(1, std::numeric_limits<float>::quiet_NaN(), Result));
	TestEqual(TEXT("NaN event value category"), Result.ErrorCategory, FString(TEXT("invalid_argument")));

	TestFalse(TEXT("Missing event export fails closed"), Runtime.DispatchEvent(7, 25.0f, Result));
	TestEqual(TEXT("Missing event export category"), Result.ErrorCategory, FString(TEXT("missing_export")));
	TestEqual(TEXT("Missing event export name"), Result.ExportName, FString(TEXT("avid_on_event")));
	TestEqual(TEXT("Failed event is not counted"), Runtime.GetEventCallbackCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmEventSuccessAndLifecycleSmokeTest,
	"AvidScript.Runtime.Event.SuccessAndLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmEventSuccessAndLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> WasmBytes = BuildEventFixture(false);
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("Event fixture loads"), Runtime.LoadModule(WasmBytes.GetData(), WasmBytes.Num(), TEXT("event_success"), Result));
	TestFalse(TEXT("Event before BeginPlay fails"), Runtime.DispatchEvent(1, 1.0f, Result));
	TestEqual(TEXT("Pre-BeginPlay event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	TestTrue(TEXT("Event fixture BeginPlay succeeds"), Runtime.BeginPlay(Result));
	TestTrue(TEXT("Gameplay event callback succeeds"), Runtime.DispatchEvent(7, 25.0f, Result));
	TestEqual(TEXT("Gameplay event count"), Result.EventCallbackCount, 1);
	TestEqual(TEXT("Gameplay event id"), Result.LastEventId, 7);
	TestEqual(TEXT("Gameplay event value"), Result.LastEventValue, 25.0f);
	TestTrue(TEXT("Gameplay event callback records timing"), Result.Metrics.EventCallbackCallMs > 0.0);
	TestTrue(TEXT("Optional EndPlay succeeds"), Runtime.EndPlay(Result));
	TestFalse(TEXT("Event after EndPlay fails"), Runtime.DispatchEvent(8, 1.0f, Result));
	TestEqual(TEXT("Post-EndPlay event category"), Result.ErrorCategory, FString(TEXT("invalid_state")));
	Runtime.Unload(Result);
	TestEqual(TEXT("Unload preserves event count"), Result.EventCallbackCount, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmEventTrapSmokeTest,
	"AvidScript.Runtime.Event.TrapSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmEventTrapSmokeTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> WasmBytes = BuildEventFixture(true);
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("Event trap fixture loads"), Runtime.LoadModule(WasmBytes.GetData(), WasmBytes.Num(), TEXT("event_trap"), Result));
	TestTrue(TEXT("Event trap fixture BeginPlay succeeds"), Runtime.BeginPlay(Result));
	TestFalse(TEXT("Event trap fails closed"), Runtime.DispatchEvent(9, 1.0f, Result));
	TestEqual(TEXT("Event trap category"), Result.ErrorCategory, FString(TEXT("trap")));
	TestEqual(TEXT("Event trap export"), Result.ExportName, FString(TEXT("avid_on_event")));
	TestEqual(TEXT("Trapped event is not counted"), Runtime.GetEventCallbackCount(), 0);
	return true;
}
#endif
