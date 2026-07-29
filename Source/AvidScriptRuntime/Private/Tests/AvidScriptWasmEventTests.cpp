#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptGameplayEvent.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"

#include "AvidScriptRuntimeBackendTestLanes.h"
#include "Fixtures/AvidScriptGameplayEventFixture.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
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

bool CreateTypedEventWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptTypedEventWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyTypedEventWorld(UWorld*& World)
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

AActor* SpawnTypedEventActor(UWorld* World)
{
	AActor* Actor = World != nullptr ? World->SpawnActor<AActor>() : nullptr;
	if (Actor == nullptr)
	{
		return nullptr;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Actor, USceneComponent::StaticClass());
	if (Root == nullptr)
	{
		return nullptr;
	}
	Actor->SetRootComponent(Root);
	Actor->AddInstanceComponent(Root);
	Root->RegisterComponentWithWorld(World);
	return Actor;
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
	for (const FAvidScriptRuntimeBackendTestLane& Lane : GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("event fixture loads")),
			Runtime.LoadModule(WasmBytes.GetData(), WasmBytes.Num(), TEXT("event_success"), Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		TestAvidScriptRuntimeLaneIdentity(*this, Lane, Result);
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("event before BeginPlay fails")), Runtime.DispatchEvent(1, 1.0f, Result));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("pre-BeginPlay event category")), Result.ErrorCategory, FString(TEXT("invalid_state")));
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("event fixture BeginPlay succeeds")), Runtime.BeginPlay(Result));
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("gameplay event callback succeeds")), Runtime.DispatchEvent(7, 25.0f, Result));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("gameplay event count")), Result.EventCallbackCount, 1);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("gameplay event id")), Result.LastEventId, 7);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("gameplay event value")), Result.LastEventValue, 25.0f);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("event callback timing")), Result.Metrics.EventCallbackCallMs > 0.0);
		TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("optional EndPlay succeeds")), Runtime.EndPlay(Result));
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("event after EndPlay fails")), Runtime.DispatchEvent(8, 1.0f, Result));
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("post-EndPlay event category")), Result.ErrorCategory, FString(TEXT("invalid_state")));
		Runtime.Unload(Result);
		TestEqual(*AvidScriptRuntimeLaneLabel(Lane, TEXT("unload preserves event count")), Result.EventCallbackCount, 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmHotLifecycleResultContractTest,
	"AvidScript.Runtime.Event.HotLifecycleResultContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmHotLifecycleResultContractTest::RunTest(
	const FString& Parameters)
{
	const TArray<uint8> EventBytes = BuildEventFixture(false);
	for (const FAvidScriptRuntimeBackendTestLane& Lane :
		GetAvidScriptRuntimeBackendTestLanes())
	{
		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult SetupResult;
		if (!TestTrue(
				*AvidScriptRuntimeLaneLabel(
					Lane,
					TEXT("hot event fixture loads")),
				Runtime.LoadModule(
					EventBytes.GetData(),
					EventBytes.Num(),
					TEXT("hot_event"),
					SetupResult))
			|| !TestTrue(
				*AvidScriptRuntimeLaneLabel(
					Lane,
					TEXT("hot event fixture begins")),
				Runtime.BeginPlay(SetupResult)))
		{
			AddError(SetupResult.ErrorMessage);
			continue;
		}

		FAvidScriptWasmSmokeResult Sentinel;
		Sentinel.ModuleId = TEXT("untouched");
		Sentinel.ErrorMessage = TEXT("sentinel");
		Sentinel.TickCallCount = 777;
		TestTrue(
			*AvidScriptRuntimeLaneLabel(Lane, TEXT("hot Tick succeeds")),
			Runtime.TickHot(1.0f / 60.0f, Sentinel));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("hot Tick leaves result storage untouched")),
			Sentinel.ErrorMessage,
			FString(TEXT("sentinel")));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("hot Tick leaves scalar result untouched")),
			Sentinel.TickCallCount,
			777);

		TestTrue(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("hot event succeeds")),
			Runtime.DispatchEventHot(9, 12.5f, Sentinel));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("hot event leaves result storage untouched")),
			Sentinel.ModuleId,
			FString(TEXT("untouched")));

		const FAvidScriptWasmHotSnapshot HotSnapshot =
			Runtime.GetHotSnapshot();
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("hot snapshot captures Tick count")),
			HotSnapshot.TickCallCount,
			1);
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("hot snapshot captures event count")),
			HotSnapshot.EventCallbackCount,
			1);
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("hot snapshot captures event id")),
			HotSnapshot.LastEventId,
			9);

		TestFalse(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("invalid hot event fails closed")),
			Runtime.DispatchEventHot(-1, 1.0f, Sentinel));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("invalid hot event materializes diagnostics")),
			Sentinel.ErrorCategory,
			FString(TEXT("invalid_argument")));
		TestEqual(
			*AvidScriptRuntimeLaneLabel(
				Lane,
				TEXT("invalid payload does not fault Runtime")),
			Runtime.GetLifecycleState(),
			EAvidScriptLifecycleState::Running);
	}

	const TArray<uint8> TypedBytes =
		AvidScriptGameplayEventFixture::Build(false);
	UWorld* TypedWorld = nullptr;
	if (!CreateTypedEventWorld(TypedWorld))
	{
		AddError(TEXT("Failed to create typed hot event world."));
		return false;
	}
	AActor* TypedActor = SpawnTypedEventActor(TypedWorld);
	if (TypedActor == nullptr)
	{
		AddError(TEXT("Failed to spawn typed hot event actor."));
		DestroyTypedEventWorld(TypedWorld);
		return false;
	}
	FAvidScriptObjectRegistry TypedRegistry;
	FAvidScriptObjectHandleResult TypedRegisterResult;
	const FAvidScriptObjectHandle TypedActorHandle =
		TypedRegistry.RegisterObject(
			TypedActor,
			TypedRegisterResult);
	if (!TestTrue(
		TEXT("typed hot event actor registers"),
		TypedRegisterResult.bSucceeded))
	{
		DestroyTypedEventWorld(TypedWorld);
		return false;
	}
	FAvidScriptWasmRuntimeInstance TypedRuntime;
	FAvidScriptWasmHostContext TypedHostContext;
	TypedHostContext.ObjectRegistry = &TypedRegistry;
	TypedHostContext.ActorWritePolicy =
		EAvidScriptActorWritePolicy::AllowWrites;
	TypedRuntime.SetHostContext(TypedHostContext);
	FAvidScriptWasmSmokeResult TypedResult;
	TestTrue(
		TEXT("typed hot fixture loads"),
		TypedRuntime.LoadModule(
			TypedBytes.GetData(),
			TypedBytes.Num(),
			TEXT("hot_typed_event"),
			TypedResult));
	TestTrue(
		TEXT("typed hot fixture begins"),
		TypedRuntime.BeginPlay(TypedResult));
	FAvidScriptGameplayEvent InputEvent;
	InputEvent.Type = EAvidScriptGameplayEventType::Input;
	InputEvent.PrimaryId = 3;
	InputEvent.SecondaryId = 1;
	InputEvent.ObjectHandle = TypedActorHandle;
	InputEvent.VectorValue = FVector3f(0.5f, 0.0f, 0.0f);
	TypedResult.ErrorMessage = TEXT("typed_sentinel");
	TestTrue(
		TEXT("typed gameplay event hot path succeeds"),
		TypedRuntime.DispatchGameplayEventHot(InputEvent, TypedResult));
	TestEqual(
		TEXT("typed gameplay hot success leaves result untouched"),
		TypedResult.ErrorMessage,
		FString(TEXT("typed_sentinel")));
	TestEqual(
		TEXT("typed gameplay hot path records event"),
		TypedRuntime.GetHotSnapshot().EventCallbackCount,
		1);
	TypedRuntime.Unload(TypedResult);
	DestroyTypedEventWorld(TypedWorld);

	const TArray<uint8> TrapBytes = BuildEventFixture(true);
	FAvidScriptWasmRuntimeInstance TrapRuntime;
	FAvidScriptWasmSmokeResult TrapResult;
	TestTrue(
		TEXT("hot trap fixture loads"),
		TrapRuntime.LoadModule(
			TrapBytes.GetData(),
			TrapBytes.Num(),
			TEXT("hot_event_trap"),
			TrapResult));
	TestTrue(
		TEXT("hot trap fixture begins"),
		TrapRuntime.BeginPlay(TrapResult));
	TrapResult.ErrorMessage = TEXT("trap_sentinel");
	TestFalse(
		TEXT("hot trap fails closed"),
		TrapRuntime.DispatchEventHot(7, 1.0f, TrapResult));
	TestEqual(
		TEXT("hot trap materializes category"),
		TrapResult.ErrorCategory,
		FString(TEXT("trap")));
	TestEqual(
		TEXT("hot trap materializes export"),
		TrapResult.ExportName,
		FString(TEXT("avid_on_event")));
	TestTrue(
		TEXT("hot trap materializes lifecycle snapshot"),
		TrapResult.bBeginPlayCalled);
	TestEqual(
		TEXT("hot trap faults Runtime"),
		TrapRuntime.GetLifecycleState(),
		EAvidScriptLifecycleState::Faulted);
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTypedGameplayEventPayloadTest,
	"AvidScript.Runtime.Event.TypedPayload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTypedGameplayEventPayloadTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateTypedEventWorld(World))
	{
		AddError(TEXT("Failed to create typed event world."));
		return false;
	}
	AActor* Actor = SpawnTypedEventActor(World);
	if (Actor == nullptr)
	{
		AddError(TEXT("Failed to spawn typed event actor."));
		DestroyTypedEventWorld(World);
		return false;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("typed event actor registers"), RegisterResult.bSucceeded);

	const TArray<uint8> WasmBytes = AvidScriptGameplayEventFixture::Build(false);
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	Runtime.SetHostContext(HostContext);
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("typed event fixture loads"), Runtime.LoadModule(WasmBytes.GetData(), WasmBytes.Num(), TEXT("typed_event"), Result));
	TestTrue(TEXT("typed event fixture begins"), Runtime.BeginPlay(Result));

	FAvidScriptGameplayEvent Event;
	Event.Type = EAvidScriptGameplayEventType::Hit;
	Event.PrimaryId = 17;
	Event.SecondaryId = 23;
	Event.ObjectHandle = ActorHandle;
	Event.VectorValue = FVector3f(11.0f, 22.0f, 33.0f);
	TestTrue(TEXT("typed gameplay event dispatches"), Runtime.DispatchGameplayEvent(Event, Result));
	TestEqual(TEXT("typed gameplay event invokes one callback"), Runtime.GetEventCallbackCount(), 1);
	TestEqual(TEXT("generic event payload preserves handle and vector order"), Actor->GetActorLocation(), FVector(11.0, 22.0, 33.0));

	DestroyTypedEventWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptTypedGameplayEventOptionalAndTrapTest,
	"AvidScript.Runtime.Event.TypedOptionalAndTrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTypedGameplayEventOptionalAndTrapTest::RunTest(const FString& Parameters)
{
	FAvidScriptGameplayEvent InputEvent;
	InputEvent.Type = EAvidScriptGameplayEventType::Input;
	InputEvent.PrimaryId = 3;
	InputEvent.SecondaryId = 1;
	InputEvent.VectorValue = FVector3f(0.5f, 0.0f, 0.0f);

	FAvidScriptWasmRuntimeInstance LegacyRuntime;
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("legacy module loads"), LegacyRuntime.LoadEmbeddedSmokeModule(Result));
	TestTrue(TEXT("legacy module begins"), LegacyRuntime.BeginPlay(Result));

	FAvidScriptGameplayEvent InvalidHitEvent;
	InvalidHitEvent.Type = EAvidScriptGameplayEventType::Hit;
	TestFalse(TEXT("hit without an object handle is rejected before VM dispatch"), LegacyRuntime.DispatchGameplayEvent(InvalidHitEvent, Result));
	TestEqual(TEXT("missing hit handle category"), Result.ErrorCategory, FString(TEXT("invalid_argument")));
	TestEqual(TEXT("invalid hit does not fault the runtime"), LegacyRuntime.GetLifecycleState(), EAvidScriptLifecycleState::Running);

	FAvidScriptGameplayEvent InvalidInputEvent = InputEvent;
	InvalidInputEvent.VectorValue.X = std::numeric_limits<float>::quiet_NaN();
	TestFalse(TEXT("non-finite input payload is rejected before VM dispatch"), LegacyRuntime.DispatchGameplayEvent(InvalidInputEvent, Result));
	TestEqual(TEXT("non-finite input category"), Result.ErrorCategory, FString(TEXT("invalid_argument")));
	TestEqual(TEXT("invalid input does not fault the runtime"), LegacyRuntime.GetLifecycleState(), EAvidScriptLifecycleState::Running);

	TestTrue(TEXT("missing typed event export is a cached no-op"), LegacyRuntime.DispatchGameplayEvent(InputEvent, Result));
	TestTrue(TEXT("second missing typed event export remains a no-op"), LegacyRuntime.DispatchGameplayEvent(InputEvent, Result));
	TestEqual(TEXT("missing typed event callback is not counted"), LegacyRuntime.GetEventCallbackCount(), 0);
	TestEqual(TEXT("legacy runtime remains running"), LegacyRuntime.GetLifecycleState(), EAvidScriptLifecycleState::Running);

	const TArray<uint8> TrapBytes = AvidScriptGameplayEventFixture::Build(true);
	FAvidScriptWasmRuntimeInstance TrapRuntime;
	TestTrue(TEXT("typed event trap fixture loads"), TrapRuntime.LoadModule(TrapBytes.GetData(), TrapBytes.Num(), TEXT("typed_event_trap"), Result));
	TestTrue(TEXT("typed event trap fixture begins"), TrapRuntime.BeginPlay(Result));
	TestFalse(TEXT("typed event trap fails closed"), TrapRuntime.DispatchGameplayEvent(InputEvent, Result));
	TestEqual(TEXT("typed event trap category"), Result.ErrorCategory, FString(TEXT("trap")));
	TestEqual(TEXT("typed event trap identifies generic export"), Result.ExportName, FString(TEXT("avid_on_gameplay_event")));
	return true;
}
#endif
