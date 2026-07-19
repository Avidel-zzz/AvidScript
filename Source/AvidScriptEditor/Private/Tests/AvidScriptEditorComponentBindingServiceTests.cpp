#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorComponentBindingService.h"

#include "AvidScriptComponent.h"

#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

namespace
{
const uint8 GEditorComponentReloadCompatibleModule[] = {
	0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
	0x01, 0x08, 0x02, 0x60, 0x00, 0x00, 0x60, 0x01,
	0x7d, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07,
	0x25, 0x02, 0x12, 0x61, 0x76, 0x69, 0x64, 0x5f,
	0x6f, 0x6e, 0x5f, 0x62, 0x65, 0x67, 0x69, 0x6e,
	0x5f, 0x70, 0x6c, 0x61, 0x79, 0x00, 0x00, 0x0c,
	0x61, 0x76, 0x69, 0x64, 0x5f, 0x6f, 0x6e, 0x5f,
	0x74, 0x69, 0x63, 0x6b, 0x00, 0x01, 0x0a, 0x07,
	0x02, 0x02, 0x00, 0x0b, 0x02, 0x00, 0x0b
};

bool BeginAvidScriptComponentBindingWorld(UWorld* World)
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

FString NormalizeAvidScriptComponentBindingTestPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString GetAvidScriptComponentBindingCSharpReportPath()
{
	return NormalizeAvidScriptComponentBindingTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ActorLifecycle"),
		TEXT("actor_lifecycle.csharp.report.json")));
}

FString GetAvidScriptComponentBindingCSharpManifestPath()
{
	return NormalizeAvidScriptComponentBindingTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ActorLifecycle"),
		TEXT("actor_lifecycle.avidscript.json")));
}

bool CreateAvidScriptComponentBindingWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptEditorComponentBindingWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyAvidScriptComponentBindingWorld(UWorld*& World)
{
	if (GEditor != nullptr)
	{
		GEditor->SelectNone(false, true, false);
	}

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

AActor* SpawnAvidScriptComponentBindingActor(UWorld& World)
{
	AActor* Actor = World.SpawnActor<AActor>();
	if (Actor == nullptr)
	{
		return nullptr;
	}

	USceneComponent* RootComponent = NewObject<USceneComponent>(Actor, TEXT("Root"));
	if (RootComponent != nullptr)
	{
		Actor->SetRootComponent(RootComponent);
		RootComponent->RegisterComponent();
	}

	return Actor;
}

bool WriteAvidScriptEditorComponentReloadFixtures(
	FString& OutValidManifestPath,
	FString& OutInvalidManifestPath,
	FString& OutFixtureRoot)
{
	OutFixtureRoot = NormalizeAvidScriptComponentBindingTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests/Phase44/EditorComponentReload")));
	IFileManager::Get().DeleteDirectory(*OutFixtureRoot, false, true);
	if (!IFileManager::Get().MakeDirectory(*OutFixtureRoot, true))
	{
		return false;
	}

	const FString WasmFileName(TEXT("binding_reload_v2.wasm"));
	const FString WasmPath = FPaths::Combine(OutFixtureRoot, WasmFileName);
	const TArray<uint8> WasmBytes(
		GEditorComponentReloadCompatibleModule,
		UE_ARRAY_COUNT(GEditorComponentReloadCompatibleModule));
	if (!FFileHelper::SaveArrayToFile(WasmBytes, *WasmPath))
	{
		return false;
	}

	OutValidManifestPath = FPaths::Combine(OutFixtureRoot, TEXT("binding_reload_v2.avidscript.json"));
	const FString ManifestJson = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"module_id\": \"binding_reload_v2\",\n")
		TEXT("  \"abi_version\": 1,\n")
		TEXT("  \"language\": \"wasm\",\n")
		TEXT("  \"wasm\": { \"file\": \"%s\", \"sha256\": \"2d8b23c662aba6350dca97e773ec4ea839100baab4a1f0c4b82893458ee88f78\" },\n")
		TEXT("  \"required_exports\": [\"avid_on_begin_play\", \"avid_on_tick\"],\n")
		TEXT("  \"required_imports\": [{ \"module\": \"env\", \"name\": \"actor_set_location\" }]\n")
		TEXT("}\n"),
		*WasmFileName);
	OutInvalidManifestPath = FPaths::Combine(OutFixtureRoot, TEXT("binding_reload_invalid.avidscript.json"));
	return FFileHelper::SaveStringToFile(ManifestJson, *OutValidManifestPath) &&
		FFileHelper::SaveStringToFile(TEXT("{ not valid json"), *OutInvalidManifestPath);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorComponentBindingApplyManifestToActorTest,
	"AvidScript.Editor.ComponentBinding.ApplyManifestToActorSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorComponentBindingApplyManifestToActorTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath = GetAvidScriptComponentBindingCSharpManifestPath();
	if (!FPaths::FileExists(ManifestPath))
	{
		AddWarning(FString::Printf(
			TEXT("C# manifest is missing; run BuildCSharpActorLifecycle.ps1 before this binding smoke. manifest=%s"),
			*ManifestPath));
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateAvidScriptComponentBindingWorld(World))
	{
		AddError(TEXT("Failed to create component binding test world."));
		DestroyAvidScriptComponentBindingWorld(World);
		return true;
	}

	AActor* Actor = SpawnAvidScriptComponentBindingActor(*World);
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyAvidScriptComponentBindingWorld(World);
		return true;
	}

	FAvidScriptEditorComponentBindingRequest Request;
	Request.Actor = Actor;
	Request.ManifestPath = ManifestPath;

	FAvidScriptEditorComponentBindingResult Result;
	TestTrue(
		TEXT("Manifest applies to actor"),
		FAvidScriptEditorComponentBindingService::ApplyManifestToActor(Request, Result));
	TestTrue(TEXT("Binding result succeeds"), Result.bSucceeded);
	TestTrue(TEXT("Missing component is created"), Result.bCreatedComponent);
	TestEqual(TEXT("Binding status"), Result.Status, EAvidScriptEditorComponentBindingStatus::Bound);
	TestEqual(TEXT("Normalized manifest path"), Result.NormalizedManifestPath, ManifestPath);
	TestNotNull(TEXT("Result returns component"), Result.Component);
	TestEqual(TEXT("Actor has same component"), Actor->FindComponentByClass<UAvidScriptComponent>(), Result.Component);

	if (Result.Component != nullptr)
	{
		TestEqual(TEXT("Component stores manifest path"), Result.Component->GetScriptManifestPath(), ManifestPath);
	}

	DestroyAvidScriptComponentBindingWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorComponentBindingApplyCSharpReportToSelectedActorTest,
	"AvidScript.Editor.ComponentBinding.ApplyCSharpReportToSelectedActorSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorComponentBindingApplyCSharpReportToSelectedActorTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = GetAvidScriptComponentBindingCSharpReportPath();
	const FString ManifestPath = GetAvidScriptComponentBindingCSharpManifestPath();
	if (!FPaths::FileExists(ReportPath) || !FPaths::FileExists(ManifestPath))
	{
		AddWarning(FString::Printf(
			TEXT("C# report or manifest is missing; run BuildCSharpActorLifecycle.ps1 before this selected-actor smoke. report=%s manifest=%s"),
			*ReportPath,
			*ManifestPath));
		return true;
	}

	if (GEditor == nullptr)
	{
		AddError(TEXT("GEditor is not available for selected-actor binding smoke."));
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateAvidScriptComponentBindingWorld(World))
	{
		AddError(TEXT("Failed to create selected-actor binding test world."));
		DestroyAvidScriptComponentBindingWorld(World);
		return true;
	}

	AActor* Actor = SpawnAvidScriptComponentBindingActor(*World);
	TestNotNull(TEXT("Selected test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyAvidScriptComponentBindingWorld(World);
		return true;
	}

	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, false, true, false);

	FAvidScriptEditorComponentBindingResult Result;
	TestTrue(
		TEXT("C# report manifest applies to selected actor"),
		FAvidScriptEditorComponentBindingService::ApplyCSharpReportToSelectedActor(ReportPath, Result));
	TestTrue(TEXT("Selected binding result succeeds"), Result.bSucceeded);
	TestTrue(TEXT("Selected binding creates component"), Result.bCreatedComponent);
	TestEqual(TEXT("Selected binding status"), Result.Status, EAvidScriptEditorComponentBindingStatus::Bound);
	TestEqual(TEXT("Selected binding report path"), Result.ReportPath, ReportPath);
	TestEqual(TEXT("Selected binding manifest path"), Result.NormalizedManifestPath, ManifestPath);
	TestNotNull(TEXT("Selected binding returns component"), Result.Component);

	if (Result.Component != nullptr)
	{
		TestEqual(TEXT("Selected component stores manifest path"), Result.Component->GetScriptManifestPath(), ManifestPath);
	}

	DestroyAvidScriptComponentBindingWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorComponentBindingTransactionalLiveReloadTest,
	"AvidScript.Editor.ComponentBinding.TransactionalLiveReload",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorComponentBindingTransactionalLiveReloadTest::RunTest(const FString& Parameters)
{
	FString ValidManifestPath;
	FString InvalidManifestPath;
	FString FixtureRoot;
	if (!TestTrue(
			TEXT("Binding reload fixtures write"),
			WriteAvidScriptEditorComponentReloadFixtures(
				ValidManifestPath,
				InvalidManifestPath,
				FixtureRoot)))
	{
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateAvidScriptComponentBindingWorld(World))
	{
		AddError(TEXT("Failed to create transactional binding test world."));
		DestroyAvidScriptComponentBindingWorld(World);
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}

	TestTrue(TEXT("Binding world BeginPlay succeeds"), BeginAvidScriptComponentBindingWorld(World));
	AActor* Actor = SpawnAvidScriptComponentBindingActor(*World);
	TestNotNull(TEXT("Transactional binding actor spawns"), Actor);
	UAvidScriptComponent* Component = Actor != nullptr
		? NewObject<UAvidScriptComponent>(Actor, TEXT("AvidScriptLiveComponent"))
		: nullptr;
	TestNotNull(TEXT("Live AvidScript component is created"), Component);
	if (Component == nullptr)
	{
		DestroyAvidScriptComponentBindingWorld(World);
		IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
		return true;
	}

	Actor->AddInstanceComponent(Component);
	Component->RegisterComponent();
	TestTrue(TEXT("Existing component starts embedded runtime"), Component->GetRuntimeStats().bRuntimeLoaded);
	TestEqual(TEXT("Embedded runtime is initially active"), Component->GetRuntimeStats().ModuleId, FString(TEXT("embedded_smoke")));

	FAvidScriptEditorComponentBindingRequest Request;
	Request.Actor = Actor;
	Request.ManifestPath = ValidManifestPath;
	FAvidScriptEditorComponentBindingResult Result;
	TestTrue(
		TEXT("Valid manifest transaction applies"),
		FAvidScriptEditorComponentBindingService::ApplyManifestToActor(Request, Result));
	TestTrue(TEXT("Live binding attempts reload"), Result.bReloadAttempted);
	TestTrue(TEXT("Live binding applies reload"), Result.bReloadApplied);
	TestTrue(TEXT("Runtime result reports applied reload"), Result.RuntimeResult.bReloadApplied);
	TestEqual(TEXT("Successful binding returns live component"), Result.Component, Component);
	TestFalse(TEXT("Existing component is reused"), Result.bCreatedComponent);
	TestEqual(TEXT("Valid path commits"), Component->GetScriptManifestPath(), ValidManifestPath);
	TestEqual(TEXT("Valid module becomes active"), Component->GetRuntimeStats().ModuleId, FString(TEXT("binding_reload_v2")));
	TestEqual(TEXT("Component records binding reload success"), Component->GetRuntimeStats().SuccessfulReloadCount, 1);

	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Reloaded binding runtime ticks"), Component->GetRuntimeStats().TickCallCount, 1);

	UPackage* ActorPackage = Actor->GetOutermost();
	TestNotNull(TEXT("Transactional binding actor has a package"), ActorPackage);
	ActorPackage->SetDirtyFlag(false);
	TestFalse(TEXT("Package starts clean before rejected binding"), ActorPackage->IsDirty());

	Request.ManifestPath = InvalidManifestPath;
	TestFalse(
		TEXT("Invalid manifest transaction is rejected"),
		FAvidScriptEditorComponentBindingService::ApplyManifestToActor(Request, Result));
	TestTrue(TEXT("Rejected binding attempted reload"), Result.bReloadAttempted);
	TestFalse(TEXT("Rejected binding did not apply reload"), Result.bReloadApplied);
	TestEqual(TEXT("Rejected binding status"), Result.Status, EAvidScriptEditorComponentBindingStatus::ReloadRejected);
	TestEqual(TEXT("Rejected binding category"), Result.ErrorCategory, FString(TEXT("reload_rejected")));
	TestEqual(TEXT("Runtime preserves manifest diagnosis"), Result.RuntimeResult.ErrorCategory, FString(TEXT("manifest_invalid")));
	TestTrue(TEXT("Runtime result confirms live rollback"), Result.RuntimeResult.bRollbackPreservedLiveRuntime);
	TestEqual(TEXT("Rejected binding returns live component"), Result.Component, Component);
	TestEqual(TEXT("Configured path rolls back"), Component->GetScriptManifestPath(), ValidManifestPath);
	TestEqual(TEXT("Active manifest remains committed path"), Component->GetRuntimeStats().ScriptManifestPath, ValidManifestPath);
	TestEqual(TEXT("Active module remains v2"), Component->GetRuntimeStats().ModuleId, FString(TEXT("binding_reload_v2")));
	TestEqual(TEXT("Component records binding reload rejection"), Component->GetRuntimeStats().RejectedReloadCount, 1);
	TestTrue(TEXT("Old runtime remains live"), Component->GetRuntimeStats().bRuntimeLoaded);
	TestFalse(TEXT("Rejected binding preserves clean package state"), ActorPackage->IsDirty());

	Component->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
	TestEqual(TEXT("Old runtime continues ticking after binding rejection"), Component->GetRuntimeStats().TickCallCount, 2);

	DestroyAvidScriptComponentBindingWorld(World);
	IFileManager::Get().DeleteDirectory(*FixtureRoot, false, true);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
