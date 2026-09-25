#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptLanguageErrorCatalog.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeBackendTestLanes.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmRuntime.h"
#include "Memory/AvidScriptManagedHeap.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptBoundedLanguageErrorBuildTest,
	"AvidScript.Runtime.BoundedLanguageErrorBuild.FormalManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptBoundedLanguageErrorBuildTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath = FPaths::Combine(FPaths::ProjectSavedDir(),
		TEXT("AvidScriptBoundedLanguageErrorBuild/bounded_language_errors.avidscript.json"));
	FAvidScriptWasmReloadManifest Manifest;
	TArray<uint8> Bytecode;
	FAvidScriptWasmReloadManifestLoadResult LoadResult;
	if (!TestTrue(TEXT("formal build manifest and WASM load"),
		FAvidScriptWasmReloadManifestLoader::LoadFromFile(
			ManifestPath, Manifest, Bytecode, LoadResult)))
	{
		AddError(LoadResult.ErrorMessage);
		return false;
	}
	TestEqual(TEXT("formal module id"), Manifest.ModuleId,
		FString(TEXT("bounded_language_errors")));
	const TArray<FAvidScriptRuntimeBackendTestLane> Lanes =
		GetAvidScriptRuntimeBackendTestLanes();
	if (!TestEqual(TEXT("Windows test has both VM backends"), Lanes.Num(), 2))
		return false;
	if (!TestNotNull(TEXT("Editor Engine is available"), GEngine))
		return false;
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false,
		TEXT("AvidScriptBoundedLanguageErrorBuild"));
	if (!TestNotNull(TEXT("test World is created"), World))
		return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
	for (const FAvidScriptRuntimeBackendTestLane& Lane : Lanes)
	{
		AActor* Owner = World->SpawnActor<AActor>();
		if (!TestNotNull(TEXT("test Actor is created"), Owner))
			return false;
		FAvidScriptObjectRegistry Registry;
		FAvidScriptObjectHandleResult HandleResult;
		FAvidScriptWasmHostContext Context;
		Context.World = World;
		Context.ObjectRegistry = &Registry;
		Context.OwnerHandle = Registry.RegisterObject(Owner, HandleResult, false);
		if (!TestTrue(TEXT("test Actor has a valid script handle"),
			Context.OwnerHandle.IsValid()))
			return false;
		FAvidScriptRuntimeSession Session;
		Session.SetBackendSelectionForTesting(Lane.Selection);
		Session.SetHostContext(Context);
		FAvidScriptWasmReloadResult SessionResult;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Session activates formal manifest")),
			Session.LoadInitialModule(Bytecode.GetData(), Bytecode.Num(), Manifest, SessionResult)))
		{
			AddError(SessionResult.ErrorMessage);
			continue;
		}
		FAvidScriptWasmSmokeResult TickResult;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("Session executes positive Tick")),
			Session.Tick(1.0f / 60.0f, TickResult)))
			AddError(TickResult.ErrorMessage);

		FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
		FAvidScriptWasmSmokeResult Result;
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("formal WASM loads directly")),
			Runtime.LoadModule(Bytecode.GetData(), Bytecode.Num(), Manifest.ModuleId, Result)))
		{
			AddError(Result.ErrorMessage);
			continue;
		}
		const FAvidScriptLanguageErrorCatalog* Catalog = Runtime.GetLanguageErrorCatalog();
		const FAvidScriptLanguageErrorSource* Source = Catalog ? Catalog->FindSource(1) : nullptr;
		TestTrue(TEXT("formal build retains source position"), Source
			&& Source->SourceId ==
				TEXT("Plugins/AvidScript/Fixtures/Phase66/BoundedLanguageErrorsLifecycle.cs")
			&& Source->Length > 0);
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("direct BeginPlay catches error")),
			Runtime.BeginPlay(Result)))
			AddError(Result.ErrorMessage);
		if (!TestTrue(*AvidScriptRuntimeLaneLabel(Lane, TEXT("direct positive Tick succeeds")),
			Runtime.Tick(1.0f / 60.0f, Result)))
			AddError(Result.ErrorMessage);
		const AvidScript::Managed::FHeap* Heap = Runtime.GetManagedHeapForTesting();
		TestTrue(TEXT("handled calls release managed roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
		TestFalse(*AvidScriptRuntimeLaneLabel(Lane, TEXT("negative Tick reports language error")),
			Runtime.Tick(-1.0f, Result));
		TestEqual(TEXT("uncaught Tick error category"), Result.ErrorCategory,
			FString(TEXT("language_error_uncaught")));
		TestTrue(TEXT("uncaught Tick includes source"), Source
			&& Result.ErrorMessage.Contains(Source->SourceId));
		TestTrue(TEXT("uncaught Tick releases invocation roots"), Heap
			&& Heap->GetStats().ActiveFrames == 0 && Heap->GetStats().LiveRoots == 0);
	}
	return true;
}

#endif
