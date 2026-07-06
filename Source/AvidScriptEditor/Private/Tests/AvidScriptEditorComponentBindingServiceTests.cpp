#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorComponentBindingService.h"

#include "AvidScriptComponent.h"

#include "Components/SceneComponent.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
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

#endif // WITH_DEV_AUTOMATION_TESTS
