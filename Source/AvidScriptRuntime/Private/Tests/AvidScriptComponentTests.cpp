#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptComponent.h"

#include "AvidScriptObjectRegistryTestTypes.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Ssl.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace
{
const uint8 GComponentReloadCompatibleModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x0d, 0x03, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02,
	0x1b, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64, 0x73,
	0x63, 0x72, 0x69, 0x70, 0x74, 0x0c, 0x68, 0x6f,
	0x73, 0x74, 0x5f, 0x61, 0x64, 0x64, 0x5f, 0x69,
	0x33, 0x32, 0x00, 0x02, 0x03, 0x03, 0x02, 0x00,
	0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x01, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x02, 0x0a, 0x07,
	0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

const uint8 GComponentReloadBeginPlayTrapModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x0d, 0x03, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x60, 0x01, 0x7f, 0x01, 0x7f, 0x02,
	0x1b, 0x01, 0x0a, 0x61, 0x76, 0x69, 0x64, 0x73,
	0x63, 0x72, 0x69, 0x70, 0x74, 0x0c, 0x68, 0x6f,
	0x73, 0x74, 0x5f, 0x61, 0x64, 0x64, 0x5f, 0x69,
	0x33, 0x32, 0x00, 0x02, 0x03, 0x03, 0x02, 0x00,
	0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x01, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x02, 0x0a, 0x08,
	0x02, 0x03, 0x00, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

FString ComponentReloadBytesToLowerHex(const uint8* Bytes, int32 ByteCount)
{
	FString Hex;
	Hex.Reserve(ByteCount * 2);
	for (int32 Index = 0; Index < ByteCount; ++Index)
	{
		Hex += FString::Printf(TEXT("%02x"), Bytes[Index]);
	}
	return Hex;
}

FString ComputeComponentReloadSha256(const TArray<uint8>& Bytes)
{
	uint8 Digest[SHA256_DIGEST_LENGTH] = {};
	SHA256(Bytes.GetData(), static_cast<size_t>(Bytes.Num()), Digest);
	return ComponentReloadBytesToLowerHex(Digest, UE_ARRAY_COUNT(Digest));
}

bool WriteComponentReloadFixture(
	const FString& Root,
	const FString& ModuleId,
	const uint8* Bytecode,
	const int32 BytecodeSize,
	FString& OutManifestPath)
{
	if (!IFileManager::Get().MakeDirectory(*Root, true))
	{
		return false;
	}

	const TArray<uint8> WasmBytes(Bytecode, BytecodeSize);
	const FString WasmFileName = ModuleId + TEXT(".wasm");
	const FString WasmPath = FPaths::Combine(Root, WasmFileName);
	OutManifestPath = FPaths::Combine(Root, ModuleId + TEXT(".avidscript.json"));
	if (!FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath))
	{
		return false;
	}

	const FString ManifestJson = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"module_id\": \"%s\",\n")
		TEXT("  \"abi_version\": 1,\n")
		TEXT("  \"language\": \"wasm\",\n")
		TEXT("  \"wasm\": { \"file\": \"%s\", \"sha256\": \"%s\" },\n")
		TEXT("  \"required_exports\": [\"avid_on_begin_play\", \"avid_on_tick\"],\n")
		TEXT("  \"required_imports\": [{ \"module\": \"avidscript\", \"name\": \"host_add_i32\" }]\n")
		TEXT("}\n"),
		*ModuleId,
		*WasmFileName,
		*ComputeComponentReloadSha256(WasmBytes));
	return FFileHelper::SaveStringToFile(ManifestJson, *OutManifestPath);
}

bool CreateComponentWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::PIE, false, TEXT("AvidScriptComponentWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::PIE);
	WorldContext.SetCurrentWorld(OutWorld);

	return true;
}

void DestroyComponentWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}

	if (World->HasBegunPlay())
	{
		World->EndPlay(EEndPlayReason::Quit);
	}

	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}

	World->DestroyWorld(false);
	World = nullptr;
}

FString GetCSharpComponentManifestPath()
{
	FString ManifestPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ActorLifecycle"),
		TEXT("actor_lifecycle.avidscript.json"));
	ManifestPath = FPaths::ConvertRelativePathToFull(ManifestPath);
	FPaths::NormalizeFilename(ManifestPath);
	return ManifestPath;
}

UAvidScriptComponent* AddAvidScriptComponent(AActor* Actor, const FString& ScriptManifestPath = FString())
{
	UAvidScriptComponent* Component = NewObject<UAvidScriptComponent>(Actor, TEXT("AvidScriptComponent"));
	if (Component != nullptr)
	{
		Actor->AddInstanceComponent(Component);
		if (!ScriptManifestPath.IsEmpty())
		{
			Component->SetScriptManifestPath(ScriptManifestPath);
		}
		Component->RegisterComponent();
	}

	return Component;
}

bool BeginComponentWorld(UWorld* World)
{
	if (World == nullptr)
	{
		return false;
	}

	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	return true;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentReentrantReleaseTest,
	"AvidScript.Component.ReentrantRelease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentReentrantReleaseTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create the reentrant release world."));
		return false;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));
	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Reentrant release actor spawns"), Actor);
	UAvidScriptComponent* Component = Actor != nullptr ? AddAvidScriptComponent(Actor) : nullptr;
	TestNotNull(TEXT("Reentrant release component is created"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return false;
	}

	FAvidScriptRuntimeSession* Session = Component->GetRuntimeSessionForTesting();
	TestNotNull(TEXT("Component owns a live Runtime Session"), Session);
	if (Session != nullptr)
	{
		Session->SetLiveExecutionObserverForTesting([Component]()
		{
			Component->DestroyComponent();
		});
		Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	}

	TestTrue(TEXT("Component EndPlay is observed during the active guest call"), Component->GetRuntimeStats().bComponentEndPlayObserved);
	TestFalse(TEXT("Deferred Runtime release completes after the guest call unwinds"), Component->GetRuntimeStats().bRuntimeLoaded);
	TestFalse(TEXT("Destroyed component is no longer registered"), Component->IsRegistered());
	DestroyComponentWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentTransactionalReloadTest,
	"AvidScript.Component.TransactionalReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentTransactionalReloadTest::RunTest(const FString& Parameters)
{
	FString FixtureRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests/Phase44/ComponentReload")));
	FPaths::NormalizeFilename(FixtureRoot);
	IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);

	FString ManifestV1Path;
	FString ManifestV2Path;
	FString TrapManifestPath;
	if (!TestTrue(TEXT("v1 fixture writes"), WriteComponentReloadFixture(
			FixtureRoot,
			TEXT("component_reload_v1"),
			GComponentReloadCompatibleModule,
			UE_ARRAY_COUNT(GComponentReloadCompatibleModule),
			ManifestV1Path)) ||
		!TestTrue(TEXT("v2 fixture writes"), WriteComponentReloadFixture(
			FixtureRoot,
			TEXT("component_reload_v2"),
			GComponentReloadCompatibleModule,
			UE_ARRAY_COUNT(GComponentReloadCompatibleModule),
			ManifestV2Path)) ||
		!TestTrue(TEXT("trap fixture writes"), WriteComponentReloadFixture(
			FixtureRoot,
			TEXT("component_reload_trap"),
			GComponentReloadBeginPlayTrapModule,
			UE_ARRAY_COUNT(GComponentReloadBeginPlayTrapModule),
			TrapManifestPath)))
	{
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create transactional component reload world."));
		DestroyComponentWorld(World);
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));
	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Reload test actor spawns"), Actor);
	UAvidScriptComponent* Component = Actor != nullptr
		? AddAvidScriptComponent(Actor, ManifestV1Path)
		: nullptr;
	TestNotNull(TEXT("Reload component is created"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}

	TestEqual(
		TEXT("v1 is initially active"),
		Component->GetRuntimeStats().ModuleId,
		FString(TEXT("component_reload_v1")));
	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("v1 ticks"), Component->GetRuntimeStats().TickCallCount, 1);

	UFunction* ReloadFunction = Component->FindFunction(GET_FUNCTION_NAME_CHECKED(UAvidScriptComponent, ReloadScript));
	TestNotNull(TEXT("ReloadScript is reflected for Blueprint"), ReloadFunction);
	if (ReloadFunction != nullptr)
	{
		TestTrue(TEXT("ReloadScript is BlueprintCallable"), ReloadFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	Component->SetScriptManifestPath(ManifestV2Path);
	FAvidScriptWasmReloadResult ReloadResult;
	TestTrue(TEXT("compatible component reload applies"), Component->ReloadConfiguredScript(ReloadResult));
	TestTrue(TEXT("component reload result reports applied"), ReloadResult.bReloadApplied);
	TestEqual(TEXT("v2 becomes active"), Component->GetRuntimeStats().ModuleId, FString(TEXT("component_reload_v2")));
	TestEqual(TEXT("component records successful reload"), Component->GetRuntimeStats().SuccessfulReloadCount, 1);
	TestEqual(TEXT("active manifest commits v2"), Component->GetRuntimeStats().ScriptManifestPath, ManifestV2Path);
	TestTrue(TEXT("component stays loaded after successful reload"), Component->GetRuntimeStats().bRuntimeLoaded);

	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("v2 starts a fresh tick count"), Component->GetRuntimeStats().TickCallCount, 1);

	Component->SetScriptManifestPath(TrapManifestPath);
	TestFalse(TEXT("BeginPlay trap component reload is rejected"), Component->ReloadConfiguredScript(ReloadResult));
	TestTrue(TEXT("rejected component reload reports rollback"), ReloadResult.bRollbackPreservedLiveRuntime);
	TestFalse(TEXT("rejected component reload is not applied"), ReloadResult.bReloadApplied);
	TestEqual(TEXT("v2 remains active after rejection"), Component->GetRuntimeStats().ModuleId, FString(TEXT("component_reload_v2")));
	TestEqual(TEXT("component records rejected reload"), Component->GetRuntimeStats().RejectedReloadCount, 1);
	TestEqual(TEXT("active manifest remains v2"), Component->GetRuntimeStats().ScriptManifestPath, ManifestV2Path);
	TestTrue(TEXT("component stays loaded after rejected reload"), Component->GetRuntimeStats().bRuntimeLoaded);
	TestTrue(TEXT("component tick remains enabled after rejected reload"), Component->IsComponentTickEnabled());

	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("old v2 runtime continues ticking"), Component->GetRuntimeStats().TickCallCount, 2);

	DestroyComponentWorld(World);
	IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentOwnerHandleLifecycleSmokeTest,
	"AvidScript.Component.OwnerHandleLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentOwnerHandleLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript component world."));
		DestroyComponentWorld(World);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	UAvidScriptComponent* Component = AddAvidScriptComponent(Actor);
	TestNotNull(TEXT("AvidScript component is attachable to an actor"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	UFunction* DispatchEventFunction = Component->FindFunction(GET_FUNCTION_NAME_CHECKED(UAvidScriptComponent, DispatchScriptEvent));
	TestNotNull(TEXT("DispatchScriptEvent is reflected for Blueprint"), DispatchEventFunction);
	if (DispatchEventFunction != nullptr)
	{
		TestTrue(TEXT("DispatchScriptEvent is BlueprintCallable"), DispatchEventFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	UFunction* DispatchInputFunction = Component->FindFunction(GET_FUNCTION_NAME_CHECKED(UAvidScriptComponent, DispatchScriptInput));
	TestNotNull(TEXT("DispatchScriptInput is reflected for Blueprint"), DispatchInputFunction);
	if (DispatchInputFunction != nullptr)
	{
		TestTrue(TEXT("DispatchScriptInput is BlueprintCallable"), DispatchInputFunction->HasAnyFunctionFlags(FUNC_BlueprintCallable));
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component registers owner on BeginPlay"), StatsAfterBeginPlay.bOwnerRegistered);
	TestTrue(TEXT("Component exposes a valid owner handle"), StatsAfterBeginPlay.OwnerHandle.IsValid());
	TestEqual(TEXT("Component records owner object path"), StatsAfterBeginPlay.OwnerObjectPath, Actor->GetPathName());

	FAvidScriptObjectHandleResult ResolveResult;
	AActor* ResolvedOwner = nullptr;
	TestTrue(TEXT("Owner handle resolves while component is active"), Component->ResolveOwnerActor(ResolvedOwner, ResolveResult));
	TestEqual(TEXT("Resolved owner matches component owner"), ResolvedOwner, Actor);

	TestTrue(TEXT("Smoke world routes EndPlay"), World->EndPlay(EEndPlayReason::Quit));

	const FAvidScriptComponentRuntimeStats StatsAfterEndPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component records receiving EndPlay"), StatsAfterEndPlay.bComponentEndPlayObserved);
	TestFalse(TEXT("Missing optional guest EndPlay is not marked called"), StatsAfterEndPlay.bEndPlayCalled);
	TestTrue(TEXT("Component releases owner handle on EndPlay"), StatsAfterEndPlay.bOwnerReleased);

	ResolvedOwner = nullptr;
	TestFalse(TEXT("Released owner handle no longer resolves"), Component->ResolveOwnerActor(ResolvedOwner, ResolveResult));
	TestEqual(TEXT("Released owner handle reports stale generation"), ResolveResult.ErrorCategory, FString(TEXT("generation_mismatch")));

	DestroyComponentWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentRuntimeTickSmokeTest,
	"AvidScript.Component.RuntimeTickSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentRuntimeTickSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript component world."));
		DestroyComponentWorld(World);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	UAvidScriptComponent* Component = AddAvidScriptComponent(Actor);
	TestNotNull(TEXT("AvidScript component is attachable to an actor"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component loads embedded smoke runtime on BeginPlay"), StatsAfterBeginPlay.bRuntimeLoaded);
	TestTrue(TEXT("Component calls avid_on_begin_play"), StatsAfterBeginPlay.bBeginPlayCalled);

	World->Tick(LEVELTICK_All, 1.0f / 60.0f);

	const FAvidScriptComponentRuntimeStats StatsAfterTick = Component->GetRuntimeStats();
	TestTrue(TEXT("Component tick calls avid_on_tick"), StatsAfterTick.TickCallCount > 0);

	TestTrue(TEXT("Smoke world routes EndPlay"), World->EndPlay(EEndPlayReason::Quit));

	const FAvidScriptComponentRuntimeStats StatsAfterEndPlay = Component->GetRuntimeStats();
	TestFalse(TEXT("Component unloads runtime on EndPlay"), StatsAfterEndPlay.bRuntimeLoaded);

	DestroyComponentWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptComponentCSharpManifestLifecycleSmokeTest,
	"AvidScript.Component.CSharpManifestLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptComponentCSharpManifestLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath = GetCSharpComponentManifestPath();
	if (!TestTrue(TEXT("C# adapter manifest exists before component lifecycle test"), FPaths::FileExists(ManifestPath)))
	{
		AddError(FString::Printf(
			TEXT("Run BuildCSharpActorLifecycle.ps1 before this component smoke. manifest=%s"),
			*ManifestPath));
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateComponentWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript component world."));
		DestroyComponentWorld(World);
		return true;
	}

	TestTrue(TEXT("World BeginPlay succeeds"), BeginComponentWorld(World));

	AActor* Actor = World->SpawnActor<AAvidScriptActorBindingTestActor>();
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0));
	Actor->SetActorRotation(FRotator(5.0, 10.0, 15.0));
	Actor->SetActorScale3D(FVector(2.0, 2.0, 2.0));

	UAvidScriptComponent* Component = AddAvidScriptComponent(Actor, ManifestPath);
	TestNotNull(TEXT("AvidScript component is attachable to an actor with a C# manifest"), Component);
	if (Component == nullptr)
	{
		DestroyComponentWorld(World);
		return true;
	}

	const FAvidScriptComponentRuntimeStats StatsAfterBeginPlay = Component->GetRuntimeStats();
	TestTrue(TEXT("Component loads C# manifest runtime on BeginPlay"), StatsAfterBeginPlay.bRuntimeLoaded);
	TestTrue(TEXT("Component calls C# avid_on_begin_play"), StatsAfterBeginPlay.bBeginPlayCalled);
	TestTrue(TEXT("C# component BeginPlay moves actor"), Actor->GetActorLocation().Equals(FVector(100.0, 200.0, 300.0), 0.01));
	TestTrue(TEXT("C# component BeginPlay resets rotation"), Actor->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));
	TestTrue(TEXT("C# component BeginPlay resets scale"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.01));

	World->Tick(LEVELTICK_All, 1.0f / 60.0f);

	const FAvidScriptComponentRuntimeStats StatsAfterTick = Component->GetRuntimeStats();
	TestTrue(TEXT("Component tick calls C# avid_on_tick"), StatsAfterTick.TickCallCount > 0);
	TestTrue(TEXT("C# component Tick moves actor"), Actor->GetActorLocation().Equals(FVector(102.0, 200.0, 300.0), 0.01));
	TestTrue(TEXT("C# component Tick rotates actor"), Actor->GetActorRotation().Equals(FRotator(0.0, 1.5, 0.0), 0.01));
	TestTrue(TEXT("C# component Tick scales actor"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.01), 0.01));
	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	const FAvidScriptComponentRuntimeStats StatsAfterTimer = Component->GetRuntimeStats();
	TestTrue(TEXT("C# component Timer callback moves actor"), Actor->GetActorLocation().Equals(FVector(106.0, 200.0, 350.0), 0.01));
	TestEqual(TEXT("Component records one Timer callback"), StatsAfterTimer.TimerCallbackCount, 1);
	TestEqual(TEXT("Component records Timer callback id"), StatsAfterTimer.LastTimerCallbackId, 7);
	TestTrue(TEXT("Component records Timer handle"), StatsAfterTimer.LastTimerHandle > 0);
	TestTrue(TEXT("Component dispatches a gameplay event"), Component->DispatchScriptEvent(3, 25.0f));
	const FAvidScriptComponentRuntimeStats StatsAfterEvent = Component->GetRuntimeStats();
	TestTrue(TEXT("C# component event callback moves actor"), Actor->GetActorLocation().Equals(FVector(106.0, 225.0, 350.0), 0.01));
	TestEqual(TEXT("Component records one gameplay event"), StatsAfterEvent.EventCallbackCount, 1);
	TestEqual(TEXT("Component records gameplay event id"), StatsAfterEvent.LastEventId, 3);
	TestEqual(TEXT("Component records gameplay event value"), StatsAfterEvent.LastEventValue, 25.0f);
	TestFalse(TEXT("Component rejects an invalid gameplay event id"), Component->DispatchScriptEvent(-1, 1.0f));
	TestTrue(TEXT("Invalid host event does not unload a healthy runtime"), Component->GetRuntimeStats().bRuntimeLoaded);

	TestTrue(TEXT("Component dispatches typed input"), Component->DispatchScriptInput(5, 2, FVector(1.0, 2.0, 3.0)));
	const FAvidScriptComponentRuntimeStats StatsAfterInput = Component->GetRuntimeStats();
	TestTrue(TEXT("C# input maps ids and vector"), Actor->GetActorLocation().Equals(FVector(6.0, 4.0, 3.0), 0.01));
	TestEqual(TEXT("Input uses shared event accounting"), StatsAfterInput.EventCallbackCount, 2);
	TestEqual(TEXT("Component records Input event type"), StatsAfterInput.LastEventId, static_cast<int32>(EAvidScriptGameplayEventType::Input));
	TestEqual(TEXT("Component records input action id"), StatsAfterInput.LastInputActionId, 5);
	TestEqual(TEXT("Component records input trigger event"), StatsAfterInput.LastInputTriggerEvent, 2);
	TestTrue(TEXT("Component records input vector"), StatsAfterInput.LastInputValue.Equals(FVector(1.0, 2.0, 3.0), 0.01));
	TestFalse(TEXT("Component rejects a negative input action id"), Component->DispatchScriptInput(-1, 2, FVector::ZeroVector));
	TestTrue(TEXT("Invalid input does not unload a healthy runtime"), Component->GetRuntimeStats().bRuntimeLoaded);

	TestTrue(TEXT("Smoke world routes EndPlay"), World->EndPlay(EEndPlayReason::Quit));
	TestFalse(TEXT("Input cannot dispatch after EndPlay"), Component->DispatchScriptInput(5, 2, FVector::ZeroVector));
	TestTrue(TEXT("C# component EndPlay moves actor"), Actor->GetActorLocation().Equals(FVector::ZeroVector, 0.01));
	TestTrue(TEXT("C# component EndPlay resets rotation"), Actor->GetActorRotation().Equals(FRotator::ZeroRotator, 0.01));
	TestTrue(TEXT("C# component EndPlay resets scale"), Actor->GetActorScale3D().Equals(FVector(1.0, 1.0, 1.0), 0.01));

	const FAvidScriptComponentRuntimeStats StatsAfterEndPlay = Component->GetRuntimeStats();
	TestFalse(TEXT("Component unloads C# runtime on EndPlay"), StatsAfterEndPlay.bRuntimeLoaded);
	TestTrue(TEXT("Component records receiving C# EndPlay"), StatsAfterEndPlay.bComponentEndPlayObserved);
	TestTrue(TEXT("Component records the C# guest EndPlay export"), StatsAfterEndPlay.bEndPlayCalled);

	DestroyComponentWorld(World);
	return true;
}

#endif
