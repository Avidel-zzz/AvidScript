#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmModuleLoader.h"
#include "AvidScriptWasmReload.h"
#include "AvidScriptWasmRuntime.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
const uint8 GAvidScriptActorImportLinkWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x18, 0x04, 0x60, 0x03, 0x7f, 0x7f, 0x7f,
	0x01, 0x7f, 0x60, 0x05, 0x7f, 0x7f, 0x7d, 0x7d,
	0x7d, 0x01, 0x7f, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x02, 0x41, 0x02, 0x0a, 0x61, 0x76,
	0x69, 0x64, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74,
	0x12, 0x61, 0x63, 0x74, 0x6f, 0x72, 0x5f, 0x67,
	0x65, 0x74, 0x5f, 0x6c, 0x6f, 0x63, 0x61, 0x74,
	0x69, 0x6f, 0x6e, 0x00, 0x00, 0x0a, 0x61, 0x76,
	0x69, 0x64, 0x73, 0x63, 0x72, 0x69, 0x70, 0x74,
	0x12, 0x61, 0x63, 0x74, 0x6f, 0x72, 0x5f, 0x73,
	0x65, 0x74, 0x5f, 0x6c, 0x6f, 0x63, 0x61, 0x74,
	0x69, 0x6f, 0x6e, 0x00, 0x01, 0x03, 0x03, 0x02,
	0x02, 0x03, 0x07, 0x25, 0x02, 0x12, 0x61, 0x76,
	0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65,
	0x67, 0x69, 0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79,
	0x00, 0x02, 0x0c, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x74, 0x69, 0x63, 0x6b, 0x00,
	0x03, 0x0a, 0x07, 0x02, 0x02, 0x00, 0x0b, 0x02,
	0x00, 0x0b
};

void AppendU32Leb(TArray<uint8>& Bytes, uint32 Value)
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

void AppendString(TArray<uint8>& Bytes, const char* Text)
{
	const int32 Length = static_cast<int32>(FCStringAnsi::Strlen(Text));
	AppendU32Leb(Bytes, static_cast<uint32>(Length));
	for (int32 Index = 0; Index < Length; ++Index)
	{
		Bytes.Add(static_cast<uint8>(Text[Index]));
	}
}

void AppendSection(TArray<uint8>& Module, uint8 SectionId, const TArray<uint8>& Payload)
{
	Module.Add(SectionId);
	AppendU32Leb(Module, static_cast<uint32>(Payload.Num()));
	Module.Append(Payload);
}

void AppendF32(TArray<uint8>& Bytes, float Value)
{
	const uint8* RawBytes = reinterpret_cast<const uint8*>(&Value);
	Bytes.Append(RawBytes, sizeof(Value));
}

void AppendActorLocationCall(
	TArray<uint8>& Body,
	uint32 FunctionIndex,
	const FAvidScriptObjectHandle& ActorHandle,
	const FVector& Value)
{
	Body.Add(0x41);
	AppendU32Leb(Body, ActorHandle.Slot);
	Body.Add(0x41);
	AppendU32Leb(Body, ActorHandle.Generation);
	Body.Add(0x43);
	AppendF32(Body, static_cast<float>(Value.X));
	Body.Add(0x43);
	AppendF32(Body, static_cast<float>(Value.Y));
	Body.Add(0x43);
	AppendF32(Body, static_cast<float>(Value.Z));
	Body.Add(0x10);
	AppendU32Leb(Body, FunctionIndex);
	Body.Add(0x1a);
}

TArray<uint8> BuildActorLifecycleFixture(
	const FAvidScriptObjectHandle& ActorHandle,
	uint32 BeginPlayImportIndex,
	const FVector& BeginPlayValue,
	uint32 EndPlayImportIndex,
	const FVector& EndPlayValue,
	bool bTrapAfterEndPlayCall = false)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> TypeSection;
	AppendU32Leb(TypeSection, 3);
	TypeSection.Add(0x60);
	AppendU32Leb(TypeSection, 5);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7d);
	TypeSection.Add(0x7d);
	TypeSection.Add(0x7d);
	AppendU32Leb(TypeSection, 1);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x60);
	AppendU32Leb(TypeSection, 0);
	AppendU32Leb(TypeSection, 0);
	TypeSection.Add(0x60);
	AppendU32Leb(TypeSection, 1);
	TypeSection.Add(0x7d);
	AppendU32Leb(TypeSection, 0);
	AppendSection(Module, 1, TypeSection);

	TArray<uint8> ImportSection;
	AppendU32Leb(ImportSection, 2);
	AppendString(ImportSection, "env");
	AppendString(ImportSection, "actor_set_location");
	ImportSection.Add(0x00);
	AppendU32Leb(ImportSection, 0);
	AppendString(ImportSection, "env");
	AppendString(ImportSection, "actor_add_location_offset");
	ImportSection.Add(0x00);
	AppendU32Leb(ImportSection, 0);
	AppendSection(Module, 2, ImportSection);

	TArray<uint8> FunctionSection;
	AppendU32Leb(FunctionSection, 3);
	AppendU32Leb(FunctionSection, 1);
	AppendU32Leb(FunctionSection, 2);
	AppendU32Leb(FunctionSection, 1);
	AppendSection(Module, 3, FunctionSection);

	TArray<uint8> ExportSection;
	AppendU32Leb(ExportSection, 3);
	AppendString(ExportSection, "avid_on_begin_play");
	ExportSection.Add(0x00);
	AppendU32Leb(ExportSection, 2);
	AppendString(ExportSection, "avid_on_tick");
	ExportSection.Add(0x00);
	AppendU32Leb(ExportSection, 3);
	AppendString(ExportSection, "avid_on_end_play");
	ExportSection.Add(0x00);
	AppendU32Leb(ExportSection, 4);
	AppendSection(Module, 7, ExportSection);

	TArray<uint8> BeginPlayBody;
	AppendU32Leb(BeginPlayBody, 0);
	AppendActorLocationCall(BeginPlayBody, BeginPlayImportIndex, ActorHandle, BeginPlayValue);
	BeginPlayBody.Add(0x0b);

	TArray<uint8> TickBody;
	AppendU32Leb(TickBody, 0);
	TickBody.Add(0x0b);

	TArray<uint8> EndPlayBody;
	AppendU32Leb(EndPlayBody, 0);
	AppendActorLocationCall(EndPlayBody, EndPlayImportIndex, ActorHandle, EndPlayValue);
	if (bTrapAfterEndPlayCall)
	{
		EndPlayBody.Add(0x00);
	}
	EndPlayBody.Add(0x0b);

	TArray<uint8> CodeSection;
	AppendU32Leb(CodeSection, 3);
	AppendU32Leb(CodeSection, static_cast<uint32>(BeginPlayBody.Num()));
	CodeSection.Append(BeginPlayBody);
	AppendU32Leb(CodeSection, static_cast<uint32>(TickBody.Num()));
	CodeSection.Append(TickBody);
	AppendU32Leb(CodeSection, static_cast<uint32>(EndPlayBody.Num()));
	CodeSection.Append(EndPlayBody);
	AppendSection(Module, 10, CodeSection);

	return Module;
}

TArray<uint8> BuildActorSetLocationFixture(const FAvidScriptObjectHandle& ActorHandle, const FVector& TargetLocation)
{
	TArray<uint8> Module;
	const uint8 Header[] = { 0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 };
	Module.Append(Header, UE_ARRAY_COUNT(Header));

	TArray<uint8> TypeSection;
	AppendU32Leb(TypeSection, 3);
	TypeSection.Add(0x60);
	AppendU32Leb(TypeSection, 5);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x7d);
	TypeSection.Add(0x7d);
	TypeSection.Add(0x7d);
	AppendU32Leb(TypeSection, 1);
	TypeSection.Add(0x7f);
	TypeSection.Add(0x60);
	AppendU32Leb(TypeSection, 0);
	AppendU32Leb(TypeSection, 0);
	TypeSection.Add(0x60);
	AppendU32Leb(TypeSection, 1);
	TypeSection.Add(0x7d);
	AppendU32Leb(TypeSection, 0);
	AppendSection(Module, 1, TypeSection);

	TArray<uint8> ImportSection;
	AppendU32Leb(ImportSection, 1);
	AppendString(ImportSection, "avidscript");
	AppendString(ImportSection, "actor_set_location");
	ImportSection.Add(0x00);
	AppendU32Leb(ImportSection, 0);
	AppendSection(Module, 2, ImportSection);

	TArray<uint8> FunctionSection;
	AppendU32Leb(FunctionSection, 2);
	AppendU32Leb(FunctionSection, 1);
	AppendU32Leb(FunctionSection, 2);
	AppendSection(Module, 3, FunctionSection);

	TArray<uint8> ExportSection;
	AppendU32Leb(ExportSection, 2);
	AppendString(ExportSection, "avid_on_begin_play");
	ExportSection.Add(0x00);
	AppendU32Leb(ExportSection, 1);
	AppendString(ExportSection, "avid_on_tick");
	ExportSection.Add(0x00);
	AppendU32Leb(ExportSection, 2);
	AppendSection(Module, 7, ExportSection);

	TArray<uint8> BeginPlayBody;
	AppendU32Leb(BeginPlayBody, 0);
	BeginPlayBody.Add(0x41);
	AppendU32Leb(BeginPlayBody, ActorHandle.Slot);
	BeginPlayBody.Add(0x41);
	AppendU32Leb(BeginPlayBody, ActorHandle.Generation);
	BeginPlayBody.Add(0x43);
	AppendF32(BeginPlayBody, static_cast<float>(TargetLocation.X));
	BeginPlayBody.Add(0x43);
	AppendF32(BeginPlayBody, static_cast<float>(TargetLocation.Y));
	BeginPlayBody.Add(0x43);
	AppendF32(BeginPlayBody, static_cast<float>(TargetLocation.Z));
	BeginPlayBody.Add(0x10);
	AppendU32Leb(BeginPlayBody, 0);
	BeginPlayBody.Add(0x1a);
	BeginPlayBody.Add(0x0b);

	TArray<uint8> TickBody;
	AppendU32Leb(TickBody, 0);
	TickBody.Add(0x0b);

	TArray<uint8> CodeSection;
	AppendU32Leb(CodeSection, 2);
	AppendU32Leb(CodeSection, static_cast<uint32>(BeginPlayBody.Num()));
	CodeSection.Append(BeginPlayBody);
	AppendU32Leb(CodeSection, static_cast<uint32>(TickBody.Num()));
	CodeSection.Append(TickBody);
	AppendSection(Module, 10, CodeSection);

	return Module;
}

FString WriteExternalWasmFixture(const TCHAR* FixtureFileName, const TArray<uint8>& WasmBytes)
{
	const FString FixtureDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptTests"), TEXT("Phase4"));
	IFileManager::Get().MakeDirectory(*FixtureDirectory, true);
	const FString FixturePath = FPaths::Combine(FixtureDirectory, FixtureFileName);
	FFileHelper::SaveArrayToFile(WasmBytes, *FixturePath);
	return FixturePath;
}

bool CreateWasmActorImportWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptWasmActorImportWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyWasmActorImportWorld(UWorld*& World)
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
	FAvidScriptWasmActorImportLinkSmokeTest,
	"AvidScript.Runtime.WasmActor.ImportLinkSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmActorImportLinkSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;

	const bool bLoaded = Runtime.LoadModule(
		GAvidScriptActorImportLinkWasmModule,
		UE_ARRAY_COUNT(GAvidScriptActorImportLinkWasmModule),
		TEXT("actor_import_link"),
		Result);

	if (!bLoaded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("WASM guest links actor host imports"), bLoaded);
	TestTrue(TEXT("Linked module can call BeginPlay"), bLoaded && Runtime.BeginPlay(Result));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmActorExternalFileSetLocationSmokeTest,
	"AvidScript.Runtime.WasmActor.ExternalFileSetLocationSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmActorExternalFileSetLocationSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateWasmActorImportWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript external WASM actor test world."));
		DestroyWasmActorImportWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyWasmActorImportWorld(World);
		return true;
	}

	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers"), RegisterResult.bSucceeded);

	const FVector TargetLocation(123.0, 456.0, 789.0);
	const TArray<uint8> WasmBytes = BuildActorSetLocationFixture(ActorHandle, TargetLocation);
	const FString FixturePath = WriteExternalWasmFixture(TEXT("actor_set_location_external.wasm"), WasmBytes);

	TArray<uint8> LoadedBytes;
	FAvidScriptWasmModuleLoadResult LoadResult;
	const bool bLoadedFromFile = FAvidScriptWasmModuleLoader::LoadFromFile(FixturePath, LoadedBytes, LoadResult);
	if (!bLoadedFromFile)
	{
		AddError(LoadResult.ErrorMessage);
	}

	TestTrue(TEXT("External fixture loads"), bLoadedFromFile);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);

	FAvidScriptWasmSmokeResult RuntimeResult;
	const bool bRuntimeLoaded = Runtime.LoadModule(
		LoadedBytes.GetData(),
		LoadedBytes.Num(),
		TEXT("external_actor_set_location"),
		RuntimeResult);
	if (!bRuntimeLoaded)
	{
		AddError(RuntimeResult.ErrorMessage);
	}

	TestTrue(TEXT("Runtime loads external actor fixture"), bRuntimeLoaded);

	const bool bBeginPlaySucceeded = bRuntimeLoaded && Runtime.BeginPlay(RuntimeResult);
	if (!bBeginPlaySucceeded)
	{
		AddError(RuntimeResult.ErrorMessage);
	}

	TestTrue(TEXT("BeginPlay calls actor import"), bBeginPlaySucceeded);
	TestEqual(TEXT("Actor moved by WASM"), Actor->GetActorLocation(), TargetLocation);

	IFileManager::Get().Delete(*FixturePath);
	DestroyWasmActorImportWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmActorImportDirectHandlerSmokeTest,
	"AvidScript.Runtime.WasmActor.DirectHandlerSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmActorImportDirectHandlerSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateWasmActorImportWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript WASM actor import test world."));
		DestroyWasmActorImportWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyWasmActorImportWorld(World);
		return true;
	}

	const FVector InitialLocation(12.0, 34.0, 56.0);
	Actor->SetActorLocation(InitialLocation);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers"), RegisterResult.bSucceeded);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;
	HostContext.OwnerHandle = ActorHandle;

	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);
	TestEqual(TEXT("Owner slot import returns the injected handle"), Runtime.HandleOwnerGetSlotImport(), static_cast<int32>(ActorHandle.Slot));
	TestEqual(TEXT("Owner generation import returns the injected handle"), Runtime.HandleOwnerGetGenerationImport(), static_cast<int32>(ActorHandle.Generation));

	FVector ReadLocation = FVector::ZeroVector;
	TestEqual(
		TEXT("Actor get import succeeds"),
		Runtime.HandleActorGetLocationImport(ActorHandle.Slot, ActorHandle.Generation, ReadLocation),
		1);
	TestEqual(TEXT("Read location matches actor"), ReadLocation, InitialLocation);

	const FVector TargetLocation(123.0, 456.0, 789.0);
	TestEqual(
		TEXT("Actor set import succeeds"),
		Runtime.HandleActorSetLocationImport(ActorHandle.Slot, ActorHandle.Generation, TargetLocation),
		1);
	TestEqual(TEXT("Actor moved by import handler"), Actor->GetActorLocation(), TargetLocation);

	const FVector Offset(1.5, -2.0, 3.25);
	const FVector ExpectedOffsetLocation = TargetLocation + Offset;
	int32 AddOffsetResult = 0;
	AddOffsetResult = Runtime.HandleActorAddLocationOffsetImport(ActorHandle.Slot, ActorHandle.Generation, Offset);
	TestEqual(TEXT("Actor add location offset import succeeds"), AddOffsetResult, 1);
	TestEqual(TEXT("Actor moved by offset import handler"), Actor->GetActorLocation(), ExpectedOffsetLocation);

	const FRotator InitialRotation(5.0, 15.0, 25.0);
	Actor->SetActorRotation(InitialRotation);
	FRotator ReadRotation = FRotator::ZeroRotator;
	TestEqual(
		TEXT("Actor get rotation import succeeds"),
		Runtime.HandleActorGetRotationImport(ActorHandle.Slot, ActorHandle.Generation, ReadRotation),
		1);
	TestTrue(TEXT("Read rotation matches actor"), ReadRotation.Equals(InitialRotation, 0.01));

	const FRotator TargetRotation(10.0, 90.0, -10.0);
	TestEqual(
		TEXT("Actor set rotation import succeeds"),
		Runtime.HandleActorSetRotationImport(ActorHandle.Slot, ActorHandle.Generation, TargetRotation),
		1);
	TestTrue(TEXT("Actor rotated by import handler"), Actor->GetActorRotation().Equals(TargetRotation, 0.01));

	const FVector InitialScale(1.0, 2.0, 3.0);
	Actor->SetActorScale3D(InitialScale);
	FVector ReadScale = FVector::ZeroVector;
	TestEqual(TEXT("Actor get scale import succeeds"), Runtime.HandleActorGetScaleImport(ActorHandle.Slot, ActorHandle.Generation, ReadScale), 1);
	TestTrue(TEXT("Read scale matches actor"), ReadScale.Equals(InitialScale, 0.01));

	const FVector TargetScale(2.0, 3.0, 4.0);
	TestEqual(TEXT("Actor set scale import succeeds"), Runtime.HandleActorSetScaleImport(ActorHandle.Slot, ActorHandle.Generation, TargetScale), 1);
	TestTrue(TEXT("Actor scaled by import handler"), Actor->GetActorScale3D().Equals(TargetScale, 0.01));
	FAvidScriptObjectHandle ComponentHandle;
	TestEqual(
		TEXT("Actor root component import succeeds"),
		Runtime.HandleActorGetRootComponentImport(ActorHandle.Slot, ActorHandle.Generation, ComponentHandle),
		1);
	TestTrue(TEXT("Root component import returns a valid handle"), ComponentHandle.IsValid());
	TestNotEqual(TEXT("Root component does not use owner slot"), ComponentHandle.Slot, ActorHandle.Slot);

	FVector ComponentLocation = FVector::ZeroVector;
	TestEqual(
		TEXT("Scene component get world location import succeeds"),
		Runtime.HandleSceneComponentGetWorldLocationImport(ComponentHandle.Slot, ComponentHandle.Generation, ComponentLocation),
		1);
	TestTrue(TEXT("Scene component location matches actor root"), ComponentLocation.Equals(Actor->GetRootComponent()->GetComponentLocation(), 0.01));

	const FVector ComponentTargetLocation(321.0, 654.0, 987.0);
	TestEqual(
		TEXT("Scene component set world location import succeeds"),
		Runtime.HandleSceneComponentSetWorldLocationImport(ComponentHandle.Slot, ComponentHandle.Generation, ComponentTargetLocation),
		1);
	TestTrue(TEXT("Scene component moves through import handler"), Actor->GetRootComponent()->GetComponentLocation().Equals(ComponentTargetLocation, 0.01));

	FAvidScriptObjectHandleResult ReleaseResult;
	TestTrue(TEXT("Owner handle releases for stale generation coverage"), Registry.ReleaseHandle(ActorHandle, ReleaseResult));
	TestEqual(TEXT("Stale owner slot import fails closed"), Runtime.HandleOwnerGetSlotImport(), 0);
	TestEqual(TEXT("Stale owner generation import fails closed"), Runtime.HandleOwnerGetGenerationImport(), 0);

	DestroyWasmActorImportWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmActorImportMissingContextSmokeTest,
	"AvidScript.Runtime.WasmActor.MissingContextSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmActorImportMissingContextSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;

	FVector ReadLocation = FVector::ZeroVector;
	TestEqual(
		TEXT("Missing host context fails closed on get"),
		Runtime.HandleActorGetLocationImport(1, 1, ReadLocation),
		0);
	TestEqual(TEXT("Failed get leaves zero vector"), ReadLocation, FVector::ZeroVector);

	TestEqual(
		TEXT("Missing host context fails closed on set"),
		Runtime.HandleActorSetLocationImport(1, 1, FVector(1.0, 2.0, 3.0)),
		0);

	int32 MissingAddOffsetResult = 0;
	MissingAddOffsetResult = Runtime.HandleActorAddLocationOffsetImport(1, 1, FVector(1.0, 2.0, 3.0));
	TestEqual(TEXT("Missing host context fails closed on add location offset"), MissingAddOffsetResult, 0);

	FRotator ReadRotation = FRotator::ZeroRotator;
	TestEqual(TEXT("Missing host context fails closed on get rotation"), Runtime.HandleActorGetRotationImport(1, 1, ReadRotation), 0);
	TestTrue(TEXT("Failed rotation get leaves zero rotator"), ReadRotation.Equals(FRotator::ZeroRotator, 0.01));
	TestEqual(TEXT("Missing host context fails closed on set rotation"), Runtime.HandleActorSetRotationImport(1, 1, FRotator(1.0, 2.0, 3.0)), 0);
	FVector ReadScale = FVector::ZeroVector;
	TestEqual(TEXT("Missing host context fails closed on get scale"), Runtime.HandleActorGetScaleImport(1, 1, ReadScale), 0);
	TestTrue(TEXT("Failed scale get leaves zero vector"), ReadScale.Equals(FVector::ZeroVector, 0.01));
	TestEqual(TEXT("Missing host context fails closed on set scale"), Runtime.HandleActorSetScaleImport(1, 1, FVector(1.0, 2.0, 3.0)), 0);
	FAvidScriptObjectHandle MissingComponentHandle{ 9, 9 };
	TestEqual(TEXT("Missing host context fails closed on root component"), Runtime.HandleActorGetRootComponentImport(1, 1, MissingComponentHandle), 0);
	TestFalse(TEXT("Failed root component get zeros handle"), MissingComponentHandle.IsValid());
	FVector MissingComponentLocation(9.0, 9.0, 9.0);
	TestEqual(TEXT("Missing host context fails closed on component get"), Runtime.HandleSceneComponentGetWorldLocationImport(1, 1, MissingComponentLocation), 0);
	TestEqual(TEXT("Failed component get zeros output"), MissingComponentLocation, FVector::ZeroVector);
	TestEqual(TEXT("Missing host context fails closed on component set"), Runtime.HandleSceneComponentSetWorldLocationImport(1, 1, FVector(1.0, 2.0, 3.0)), 0);
	TestEqual(TEXT("Missing owner context fails closed on slot"), Runtime.HandleOwnerGetSlotImport(), 0);
	TestEqual(TEXT("Missing owner context fails closed on generation"), Runtime.HandleOwnerGetGenerationImport(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmActorEndPlayFailureIdempotencySmokeTest,
	"AvidScript.Runtime.WasmActor.EndPlayFailureIdempotencySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmActorEndPlayFailureIdempotencySmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateWasmActorImportWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript EndPlay idempotency test world."));
		DestroyWasmActorImportWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("EndPlay idempotency actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyWasmActorImportWorld(World);
		return true;
	}

	const FVector InitialLocation(10.0, 20.0, 30.0);
	const FVector EndPlayOffset(1.0, 2.0, 3.0);
	Actor->SetActorLocation(InitialLocation);

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("EndPlay idempotency actor registers"), RegisterResult.bSucceeded);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	const TArray<uint8> WasmBytes = BuildActorLifecycleFixture(
		ActorHandle,
		0,
		InitialLocation,
		1,
		EndPlayOffset,
		true);

	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);
	FAvidScriptWasmSmokeResult Result;
	TestTrue(TEXT("EndPlay trap fixture loads"), Runtime.LoadModule(
		WasmBytes.GetData(), WasmBytes.Num(), TEXT("end_play_failure_idempotency"), Result));
	TestTrue(TEXT("EndPlay trap fixture begins play"), Runtime.BeginPlay(Result));

	FAvidScriptWasmSmokeResult FirstEndPlayResult;
	TestFalse(TEXT("First EndPlay reports the guest trap"), Runtime.EndPlay(FirstEndPlayResult));
	TestEqual(TEXT("First EndPlay trap category"), FirstEndPlayResult.ErrorCategory, FString(TEXT("trap")));
	TestEqual(TEXT("EndPlay side effect executes once"), Actor->GetActorLocation(), InitialLocation + EndPlayOffset);

	FAvidScriptWasmSmokeResult SecondEndPlayResult;
	TestFalse(TEXT("Repeated EndPlay returns the cached guest failure"), Runtime.EndPlay(SecondEndPlayResult));
	TestEqual(TEXT("Repeated EndPlay preserves the failure"), SecondEndPlayResult.ErrorMessage, FirstEndPlayResult.ErrorMessage);
	TestEqual(TEXT("Repeated EndPlay does not repeat the side effect"), Actor->GetActorLocation(), InitialLocation + EndPlayOffset);
	TestEqual(
		TEXT("Repeated EndPlay does not call another host import"),
		SecondEndPlayResult.HostImportCallCount,
		FirstEndPlayResult.HostImportCallCount);

	Runtime.Unload();
	DestroyWasmActorImportWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmReloadSkipsEndPlayTransitionSmokeTest,
	"AvidScript.Reload.SkipsEndPlayTransitionSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmReloadSkipsEndPlayTransitionSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateWasmActorImportWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript reload EndPlay transition test world."));
		DestroyWasmActorImportWorld(World);
		return true;
	}

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Reload transition actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyWasmActorImportWorld(World);
		return true;
	}

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Reload transition actor registers"), RegisterResult.bSucceeded);

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	const TArray<uint8> OldBytes = BuildActorLifecycleFixture(
		ActorHandle,
		0,
		FVector(100.0, 0.0, 0.0),
		0,
		FVector(200.0, 0.0, 0.0));
	const TArray<uint8> NewBytes = BuildActorLifecycleFixture(
		ActorHandle,
		1,
		FVector(10.0, 0.0, 0.0),
		0,
		FVector::ZeroVector);

	FAvidScriptWasmReloadManifest OldManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_end_play_old"));
	OldManifest.RequiredExports.Add(TEXT("avid_on_end_play"));
	FAvidScriptWasmReloadManifest NewManifest = FAvidScriptWasmReloadManifest::MakeSmoke(TEXT("reload_end_play_new"));
	NewManifest.RequiredExports.Add(TEXT("avid_on_end_play"));

	FAvidScriptWasmReloadSession Session;
	Session.SetHostContext(HostContext);
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(TEXT("Old lifecycle module loads"), Session.LoadInitialModule(
		OldBytes.GetData(), OldBytes.Num(), OldManifest, ReloadResult));
	TestEqual(TEXT("Old BeginPlay sets the actor location"), Actor->GetActorLocation(), FVector(100.0, 0.0, 0.0));

	TestTrue(TEXT("New lifecycle module reloads"), Session.ReloadModule(
		NewBytes.GetData(), NewBytes.Num(), NewManifest, ReloadResult));
	TestEqual(
		TEXT("Reload does not emit UE EndPlay for the old guest"),
		Actor->GetActorLocation(),
		FVector(110.0, 0.0, 0.0));

	Session.UnloadLive();
	DestroyWasmActorImportWorld(World);
	return true;
}
#endif
