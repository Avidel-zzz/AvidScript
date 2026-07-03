#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmModuleLoader.h"
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

	FAvidScriptWasmRuntimeInstance Runtime;
	Runtime.SetHostContext(HostContext);

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

	return true;
}

#endif
