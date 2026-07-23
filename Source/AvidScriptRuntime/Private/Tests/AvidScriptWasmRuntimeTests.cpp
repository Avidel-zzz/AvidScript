#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptWasmRuntime.h"
#include "AvidScriptWorldSubsystem.h"

#include "AvidScriptBindingDescriptor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"

namespace
{
const uint8 GAvidScriptMissingTickWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x04, 0x01, 0x60, 0x00, 0x00,
	0x03, 0x02, 0x01, 0x00,
	0x07, 0x16, 0x01, 0x12, 0x61, 0x76, 0x69, 0x64,
	0x5f, 0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69,
	0x6e, 0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00,
	0x0a, 0x04, 0x01, 0x02, 0x00, 0x0b
};

const uint8 GAvidScriptTrapTickWasmModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x08,
	0x02, 0x02, 0x00, 0x0b, 0x03, 0x00, 0x00, 0x0b
};

bool CreateSmokeWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;

	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::PIE, false, TEXT("AvidScriptSmokeWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::PIE);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroySmokeWorld(UWorld*& World)
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptBindingDescriptorLegacyIdentityTest,
	"AvidScript.Runtime.Binding.DescriptorLegacyIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBindingDescriptorLegacyIdentityTest::RunTest(const FString& Parameters)
{
	FAvidScriptBindingPackageModel Package;
	Package.GeneratorVersion = TEXT("legacy.generator");
	Package.EngineVersion = TEXT("5.8.0");
	Package.Source = TEXT("ue_reflection");
	Package.PackageName = TEXT("legacy.package");

	FAvidScriptBindingTypeModel Type;
	Type.StableId = TEXT("type-id");
	Type.CanonicalType = TEXT("bool");
	Type.Kind = TEXT("scalar");
	Type.CppType = TEXT("bool");
	Type.Size = 1;
	Type.Alignment = 1;
	Type.AbiTypes = { TEXT("i") };
	Package.Types.Add(Type);

	FAvidScriptBindingFunctionModel Binding;
	Binding.StableId = TEXT("binding-id");
	Binding.CanonicalIdentity = TEXT("owner::fn(void)");
	Binding.Ordinal = 0;
	Binding.OwnerClass = TEXT("/Script/Engine.Actor");
	Binding.UeMember = TEXT("Fn");
	Binding.UeFunction = TEXT("Fn");
	Binding.ScriptName = TEXT("Fn");
	Binding.DispatchMode = TEXT("cached_process_event");
	Binding.bStatic = false;
	Binding.bConst = true;
	Binding.ReloadEffect = EAvidScriptBindingReloadEffect::None;
	Binding.ReturnValue.Name = TEXT("return");
	Binding.ReturnValue.Direction = TEXT("return");
	Binding.ReturnValue.CanonicalType = TEXT("void");
	Binding.ReturnValue.TypeId = TEXT("void-id");
	Binding.ReturnValue.Kind = TEXT("void");
	Binding.ReturnValue.CppType = TEXT("void");
	Binding.HostImport.Module = TEXT("avidscript");
	Binding.HostImport.Name = TEXT("avid_ue_test");
	Binding.HostImport.Signature = TEXT("(ii)i");
	Package.Bindings.Add(Binding);

	const TCHAR* ExpectedSelectionHashes[] = {
		TEXT(""),
		TEXT(""),
		TEXT("267679b8a642f30d296bd8645580f271ef25bb7bd4e4c42244eeceae83ba4d54"),
		TEXT("267679b8a642f30d296bd8645580f271ef25bb7bd4e4c42244eeceae83ba4d54"),
		TEXT("267679b8a642f30d296bd8645580f271ef25bb7bd4e4c42244eeceae83ba4d54"),
		TEXT("bcd25af7b336196501ff44962e5c23e032d23cd8d9bb0c0792b1aaad32a1f75f")
	};
	const TCHAR* ExpectedPackageHashes[] = {
		TEXT(""),
		TEXT(""),
		TEXT("54fe33c5d11181fee26c57d10b8a64e210fd0f0b4c58bf61b3a05825c1d6ffc1"),
		TEXT("beb2669bb1a4c51c206884876db5bf7a0f370e963cc1b70343163beb13095c6b"),
		TEXT("beb2669bb1a4c51c206884876db5bf7a0f370e963cc1b70343163beb13095c6b"),
		TEXT("1f973d1960e15c88c361b7b88855ebaa5de502b36c2542dfd99a286986aad3da")
	};

	for (int32 SchemaVersion = 2; SchemaVersion <= 5; ++SchemaVersion)
	{
		Package.SchemaVersion = SchemaVersion;
		Package.SelectionHash = FAvidScriptBindingDescriptorIdentity::MakeSelectionHash(Package);
		TestEqual(
			*FString::Printf(TEXT("Schema v%d selection hash keeps legacy bytes"), SchemaVersion),
			Package.SelectionHash,
			FString(ExpectedSelectionHashes[SchemaVersion]));
		TestEqual(
			*FString::Printf(TEXT("Schema v%d package hash keeps legacy bytes"), SchemaVersion),
			FAvidScriptBindingDescriptorIdentity::MakePackageHash(Package),
			FString(ExpectedPackageHashes[SchemaVersion]));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptMinimalWasmSmokeTest,
	"AvidScript.Runtime.MinimalWasmSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptMinimalWasmSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmSmokeResult Result;
	const bool bSucceeded = FAvidScriptWasmRuntime::RunEmbeddedSmokeTest(Result);

	if (!bSucceeded)
	{
		AddError(Result.ErrorMessage);
	}

	TestTrue(TEXT("WAMR embedded smoke test succeeds"), bSucceeded);
	TestTrue(TEXT("avid_on_begin_play export is called"), Result.bBeginPlayCalled);
	TestTrue(TEXT("avid_on_tick export is called"), Result.bTickCalled);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptPackagedTimingSmokeTest,
	"AvidScript.Runtime.PackagedTimingSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptPackagedTimingSmokeTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmRuntimeInstance Runtime;
	FAvidScriptWasmSmokeResult Result;

	TestTrue(TEXT("Embedded module loads"), Runtime.LoadEmbeddedSmokeModule(Result));
	TestTrue(TEXT("Runtime init timing is captured"), Result.Metrics.RuntimeInitMs >= 0.0);
	TestTrue(TEXT("Module load timing is captured"), Result.Metrics.ModuleLoadMs > 0.0);
	TestTrue(TEXT("Module instantiate timing is captured"), Result.Metrics.ModuleInstantiateMs > 0.0);
	TestTrue(TEXT("Exec env creation timing is captured"), Result.Metrics.ExecEnvCreateMs >= 0.0);

	TestTrue(TEXT("BeginPlay export succeeds"), Runtime.BeginPlay(Result));
	TestTrue(TEXT("BeginPlay timing is captured"), Result.Metrics.BeginPlayCallMs >= 0.0);

	TestTrue(TEXT("Tick export succeeds"), Runtime.Tick(1.0f / 60.0f, Result));
	TestTrue(TEXT("Tick timing is captured"), Result.Metrics.TickCallMs >= 0.0);

	TestTrue(TEXT("Missing optional EndPlay export is a successful no-op"), Runtime.EndPlay(Result));
	TestFalse(TEXT("Missing optional EndPlay export is not marked called"), Result.bEndPlayCalled);
	TestEqual(TEXT("Missing optional EndPlay export has zero call time"), Result.Metrics.EndPlayCallMs, 0.0);

	Runtime.Unload(Result);
	TestTrue(TEXT("Unload reports completed state"), Result.bUnloaded);
	TestTrue(TEXT("Unload timing is captured"), Result.Metrics.UnloadMs >= 0.0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmSessionRestartSmokeTest,
	"AvidScript.Runtime.SessionRestartSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmSessionRestartSmokeTest::RunTest(const FString& Parameters)
{
	for (int32 RunIndex = 0; RunIndex < 2; ++RunIndex)
	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult Result;

		TestTrue(TEXT("Embedded module loads"), Runtime.LoadEmbeddedSmokeModule(Result));
		TestTrue(TEXT("Runtime reports loaded"), Runtime.IsLoaded());
		TestTrue(TEXT("BeginPlay export succeeds"), Runtime.BeginPlay(Result));
		TestTrue(TEXT("Tick export succeeds"), Runtime.Tick(1.0f / 60.0f, Result));
		TestEqual(TEXT("Tick call count"), Runtime.GetTickCallCount(), 1);

		Runtime.Unload();
		TestFalse(TEXT("Runtime unload clears loaded state"), Runtime.IsLoaded());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWasmErrorSmokeTest,
	"AvidScript.Runtime.ErrorSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWasmErrorSmokeTest::RunTest(const FString& Parameters)
{
	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult Result;

		TestTrue(
			TEXT("Module missing tick export still loads"),
			Runtime.LoadModule(GAvidScriptMissingTickWasmModule, UE_ARRAY_COUNT(GAvidScriptMissingTickWasmModule), TEXT("missing_tick"), Result));
		TestTrue(TEXT("BeginPlay still succeeds"), Runtime.BeginPlay(Result));
		TestFalse(TEXT("Missing Tick export is reported without crash"), Runtime.Tick(1.0f / 60.0f, Result));
		TestEqual(TEXT("Missing export category"), Result.ErrorCategory, FString(TEXT("missing_export")));
		TestEqual(TEXT("Missing export name"), Result.ExportName, FString(TEXT("avid_on_tick")));
	}

	{
		FAvidScriptWasmRuntimeInstance Runtime;
		FAvidScriptWasmSmokeResult Result;

		TestTrue(
			TEXT("Trap module loads"),
			Runtime.LoadModule(GAvidScriptTrapTickWasmModule, UE_ARRAY_COUNT(GAvidScriptTrapTickWasmModule), TEXT("trap_tick"), Result));
		TestTrue(TEXT("BeginPlay succeeds before trap"), Runtime.BeginPlay(Result));
		TestFalse(TEXT("Tick trap is reported without crash"), Runtime.Tick(1.0f / 60.0f, Result));
		TestEqual(TEXT("Trap category"), Result.ErrorCategory, FString(TEXT("trap")));
		TestEqual(TEXT("Trap export name"), Result.ExportName, FString(TEXT("avid_on_tick")));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptWorldSubsystemLifecycleSmokeTest,
	"AvidScript.Runtime.WorldSubsystemLifecycleSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptWorldSubsystemLifecycleSmokeTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!CreateSmokeWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript smoke world."));
		DestroySmokeWorld(World);
		return true;
	}

	UAvidScriptWorldSubsystem* Subsystem = World->GetSubsystem<UAvidScriptWorldSubsystem>();
	TestNotNull(TEXT("AvidScript world subsystem exists for PIE worlds"), Subsystem);

	const FURL Url;
	World->InitializeActorsForPlay(Url);
	World->BeginPlay();
	World->SetBegunPlay(true);
	World->Tick(LEVELTICK_All, 1.0f / 60.0f);

	if (Subsystem != nullptr)
	{
		const FAvidScriptWorldRuntimeStats StatsAfterTick = Subsystem->GetRuntimeStats();
		TestTrue(TEXT("Subsystem loaded runtime on BeginPlay"), StatsAfterTick.bRuntimeLoaded);
		TestTrue(TEXT("Subsystem called avid_on_begin_play"), StatsAfterTick.bBeginPlayCalled);
		TestTrue(TEXT("Subsystem ticked avid_on_tick"), StatsAfterTick.TickCallCount > 0);
	}

	TestTrue(TEXT("Smoke world routes EndPlay to world subsystems"), World->EndPlay(EEndPlayReason::Quit));

	if (Subsystem != nullptr)
	{
		const FAvidScriptWorldRuntimeStats StatsAfterEndPlay = Subsystem->GetRuntimeStats();
		TestFalse(TEXT("Subsystem unloads runtime on EndPlay"), StatsAfterEndPlay.bRuntimeLoaded);
		TestTrue(TEXT("Subsystem records EndPlay cleanup"), StatsAfterEndPlay.bEndPlayCalled);
	}

	DestroySmokeWorld(World);
	return true;
}

#endif
