#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace
{
void AppendSceneU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendSceneString(TArray<uint8>& Bytes, const char* Text)
{
	const int32 Length = static_cast<int32>(FCStringAnsi::Strlen(Text));
	AppendSceneU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Text[Index]));
	}
}

void AppendSceneSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendSceneU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendSceneF32(TArray<uint8>& Bytes, float Value)
{
	Bytes.Append(reinterpret_cast<const uint8*>(&Value), sizeof(Value));
}

void AppendI32Load(TArray<uint8>& Body, uint32 Address)
{
	Body.Add(0x41);
	AppendSceneU32Leb(Body, Address);
	Body.Add(0x28);
	AppendSceneU32Leb(Body, 2);
	AppendSceneU32Leb(Body, 0);
}

void AppendF32Load(TArray<uint8>& Body, uint32 Address)
{
	Body.Add(0x41);
	AppendSceneU32Leb(Body, Address);
	Body.Add(0x2a);
	AppendSceneU32Leb(Body, 2);
	AppendSceneU32Leb(Body, 0);
}

TArray<uint8> BuildSceneComponentObjectGraphFixture(
	const FAvidScriptObjectHandle& ActorHandle,
	bool bUseInvalidHandleOutputPointer)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> TypeSection;
	AppendSceneU32Leb(TypeSection, 4);
	TypeSection.Add(0x60);
	AppendSceneU32Leb(TypeSection, 3);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7f);
	AppendSceneU32Leb(TypeSection, 1);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x60);
	AppendSceneU32Leb(TypeSection, 5);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7d);
	TypeSection.Add(0x7d);
	TypeSection.Add(0x7d);
	AppendSceneU32Leb(TypeSection, 1);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x60);
	AppendSceneU32Leb(TypeSection, 0);
	AppendSceneU32Leb(TypeSection, 0);
	TypeSection.Add(0x60);
	AppendSceneU32Leb(TypeSection, 1);
	TypeSection.Add(0x7d);
	AppendSceneU32Leb(TypeSection, 0);
	AppendSceneSection(Module, 1, TypeSection);

	TArray<uint8> ImportSection;
	AppendSceneU32Leb(ImportSection, 3);
	AppendSceneString(ImportSection, "env");
	AppendSceneString(ImportSection, "actor_get_root_component");
	ImportSection.Add(0x00);
	AppendSceneU32Leb(ImportSection, 0);
	AppendSceneString(ImportSection, "env");
	AppendSceneString(ImportSection, "scene_component_get_world_location");
	ImportSection.Add(0x00);
	AppendSceneU32Leb(ImportSection, 0);
	AppendSceneString(ImportSection, "env");
	AppendSceneString(ImportSection, "scene_component_set_world_location");
	ImportSection.Add(0x00);
	AppendSceneU32Leb(ImportSection, 1);
	AppendSceneSection(Module, 2, ImportSection);

	TArray<uint8> FunctionSection;
	AppendSceneU32Leb(FunctionSection, 2);
	AppendSceneU32Leb(FunctionSection, 2);
	AppendSceneU32Leb(FunctionSection, 3);
	AppendSceneSection(Module, 3, FunctionSection);

	TArray<uint8> MemorySection;
	AppendSceneU32Leb(MemorySection, 1);
	MemorySection.Add(0x00);
	AppendSceneU32Leb(MemorySection, 1);
	AppendSceneSection(Module, 5, MemorySection);

	TArray<uint8> ExportSection;
	AppendSceneU32Leb(ExportSection, 2);
	AppendSceneString(ExportSection, "avid_on_begin_play");
	ExportSection.Add(0x00);
	AppendSceneU32Leb(ExportSection, 3);
	AppendSceneString(ExportSection, "avid_on_tick");
	ExportSection.Add(0x00);
	AppendSceneU32Leb(ExportSection, 4);
	AppendSceneSection(Module, 7, ExportSection);

	TArray<uint8> BeginPlayBody;
	AppendSceneU32Leb(BeginPlayBody, 0);
	BeginPlayBody.Add(0x41);
	AppendSceneU32Leb(BeginPlayBody, ActorHandle.Slot);
	BeginPlayBody.Add(0x41);
	AppendSceneU32Leb(BeginPlayBody, ActorHandle.Generation);
	BeginPlayBody.Add(0x41);
	AppendSceneU32Leb(BeginPlayBody, bUseInvalidHandleOutputPointer ? 2147483644u : 16u);
	BeginPlayBody.Add(0x10);
	AppendSceneU32Leb(BeginPlayBody, 0);
	BeginPlayBody.Add(0x1a);

	if (!bUseInvalidHandleOutputPointer)
	{
		AppendI32Load(BeginPlayBody, 16);
		AppendI32Load(BeginPlayBody, 20);
		BeginPlayBody.Add(0x41);
		AppendSceneU32Leb(BeginPlayBody, 24);
		BeginPlayBody.Add(0x10);
		AppendSceneU32Leb(BeginPlayBody, 1);
		BeginPlayBody.Add(0x1a);

		AppendI32Load(BeginPlayBody, 16);
		AppendI32Load(BeginPlayBody, 20);
		AppendF32Load(BeginPlayBody, 24);
		BeginPlayBody.Add(0x43);
		AppendSceneF32(BeginPlayBody, 5.0f);
		BeginPlayBody.Add(0x92);
		AppendF32Load(BeginPlayBody, 28);
		AppendF32Load(BeginPlayBody, 32);
		BeginPlayBody.Add(0x10);
		AppendSceneU32Leb(BeginPlayBody, 2);
		BeginPlayBody.Add(0x1a);
	}
	BeginPlayBody.Add(0x0b);

	TArray<uint8> TickBody;
	AppendSceneU32Leb(TickBody, 0);
	TickBody.Add(0x0b);

	TArray<uint8> CodeSection;
	AppendSceneU32Leb(CodeSection, 2);
	AppendSceneU32Leb(CodeSection, static_cast<uint32>(BeginPlayBody.Num()));
	CodeSection.Append(BeginPlayBody);
	AppendSceneU32Leb(CodeSection, static_cast<uint32>(TickBody.Num()));
	CodeSection.Append(TickBody);
	AppendSceneSection(Module, 10, CodeSection);
	return Module;
}

bool CreateSceneComponentImportWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptWasmSceneComponentImportWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroySceneComponentImportWorld(UWorld*& World)
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
	FAvidScriptWasmSceneComponentObjectGraphSmokeTest,
	"AvidScript.Runtime.WasmSceneComponent.ObjectGraphSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmSceneComponentObjectGraphSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateSceneComponentImportWorld(World))
	{
		AddError(TEXT("Failed to create SceneComponent import world."));
		return true;
	}

	AAvidScriptActorBindingTestActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroySceneComponentImportWorld(World);
		return true;
	}
	const FVector InitialLocation(10.0, 20.0, 30.0);
	Actor->SetActorLocation(InitialLocation);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult DummyResult;
	Registry.RegisterObject(World, DummyResult);
	FAvidScriptObjectHandleResult ActorResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, ActorResult);
	TestNotEqual(TEXT("Fixture actor does not occupy slot one"), ActorHandle.Slot, 1u);

	FAvidScriptWasmHostContext HostContext;
	FAvidScriptSessionObjectOwnership Ownership;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ObjectOwnership = &Ownership;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);

	const TArray<uint8> WasmBytes = BuildSceneComponentObjectGraphFixture(ActorHandle, false);
	FAvidScriptWasmSmokeResult Result;
	const bool bLoaded = Runtime.LoadModule(WasmBytes.GetData(), WasmBytes.Num(), TEXT("scene_component_object_graph"), Result);
	if (!bLoaded)
	{
		AddError(Result.ErrorMessage);
	}
	TestTrue(TEXT("SceneComponent object graph fixture links"), bLoaded);
	TestTrue(TEXT("SceneComponent object graph BeginPlay succeeds"), bLoaded && Runtime.BeginPlay(Result));
	TestTrue(TEXT("Guest handle and vector memory drive component setter"), Actor->GetActorLocation().Equals(InitialLocation + FVector(5.0, 0.0, 0.0), 0.01));

	Ownership.Cleanup(Registry);
	DestroySceneComponentImportWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmSceneComponentInvalidHandlePointerSmokeTest,
	"AvidScript.Runtime.WasmSceneComponent.InvalidHandlePointerSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmSceneComponentInvalidHandlePointerSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateSceneComponentImportWorld(World))
	{
		AddError(TEXT("Failed to create invalid pointer test world."));
		return true;
	}
	AAvidScriptActorBindingTestActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult ActorResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, ActorResult);
	FAvidScriptWasmHostContext HostContext;
	FAvidScriptSessionObjectOwnership Ownership;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ObjectOwnership = &Ownership;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);

	const TArray<uint8> WasmBytes = BuildSceneComponentObjectGraphFixture(ActorHandle, true);
	FAvidScriptWasmSmokeResult Result;
	const bool bLoaded = Runtime.LoadModule(WasmBytes.GetData(), WasmBytes.Num(), TEXT("scene_component_invalid_pointer"), Result);
	TestTrue(TEXT("Invalid pointer fixture still links"), bLoaded);
	TestFalse(TEXT("Out-of-bounds component handle output traps safely"), bLoaded && Runtime.BeginPlay(Result));
	TestEqual(TEXT("Pointer failure identifies root component import"), Result.ImportName, FString(TEXT("actor_get_root_component")));

	Ownership.Cleanup(Registry);
	DestroySceneComponentImportWorld(World);
	return true;
}

#endif
