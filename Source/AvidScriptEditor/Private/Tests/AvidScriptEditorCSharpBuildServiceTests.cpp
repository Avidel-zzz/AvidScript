#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptComponent.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"

#include "Components/SceneComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationCommon.h"

namespace
{
FString NormalizeAvidScriptCSharpBuildTestPath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

bool LoadAvidScriptCSharpBuildTestJsonObject(const FString& Path, TSharedPtr<FJsonObject>& OutObject)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *Path))
	{
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	return FJsonSerializer::Deserialize(Reader, OutObject) && OutObject.IsValid();
}

bool CreateAvidScriptCSharpBuildTestWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptCSharpBuildProfileWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyAvidScriptCSharpBuildTestWorld(UWorld*& World)
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

AActor* SpawnAvidScriptCSharpBuildTestActor(UWorld& World)
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
	FAvidScriptEditorCSharpBuildServiceCustomProfileTest,
	"AvidScript.Editor.CSharpBuildService.CustomProfileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildServiceCustomProfileTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptTests"),
		TEXT("CSharpProfiles"),
		TEXT("CustomMover")));
	TestTrue(TEXT("Custom C# profile test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString SourcePath = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(TestRoot, TEXT("CustomMoverScript.cs")));
	const FString SourceText = TEXT(
		"using AvidScript;\n"
		"\n"
		"public static class CustomMoverScript\n"
		"{\n"
		"    public static void BeginPlay()\n"
		"    {\n"
		"        Actor.SetLocation(11.0f, 22.0f, 33.0f);\n"
		"    }\n"
		"\n"
		"    public static void Tick(float deltaSeconds)\n"
		"    {\n"
		"        Actor.SetLocation(deltaSeconds * 4.0f, 5.0f, 6.0f);\n"
		"    }\n"
		"}\n");
	TestTrue(TEXT("Custom C# source can be written"), FFileHelper::SaveStringToFile(SourceText, *SourcePath));

	FAvidScriptEditorCSharpBuildConfig Config;
	Config.BuildScriptPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleBuildScriptPath();
	Config.ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	Config.SourcePath = SourcePath;
	Config.ModuleId = TEXT("csharp_custom_mover");
	Config.ArtifactStem = TEXT("custom_mover");
	Config.OutputRoot = NormalizeAvidScriptCSharpBuildTestPath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("CustomMover")));
	Config.ReportPath = FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);
	Config.ManifestPath = FAvidScriptEditorCSharpBuildService::MakeManifestPathForOutputRoot(Config.OutputRoot, Config.ArtifactStem);

	FAvidScriptEditorCSharpBuildResult BuildResult;
	TestTrue(TEXT("Custom C# profile build succeeds"), FAvidScriptEditorCSharpBuildService::BuildProfile(Config, BuildResult));
	TestTrue(TEXT("Custom C# profile build result succeeds"), BuildResult.bSucceeded);
	TestEqual(TEXT("Custom C# profile module id"), BuildResult.ModuleId, Config.ModuleId);
	TestEqual(TEXT("Custom C# profile artifact stem"), BuildResult.ArtifactStem, Config.ArtifactStem);
	TestEqual(TEXT("Custom C# profile source path"), BuildResult.SourcePath, Config.SourcePath);
	TestEqual(TEXT("Custom C# profile report path"), BuildResult.ReportPath, Config.ReportPath);
	TestEqual(TEXT("Custom C# profile manifest path"), BuildResult.ManifestPath, Config.ManifestPath);
	TestTrue(TEXT("Custom C# profile report exists"), FPaths::FileExists(Config.ReportPath));
	TestTrue(TEXT("Custom C# profile manifest exists"), FPaths::FileExists(Config.ManifestPath));

	TSharedPtr<FJsonObject> ReportObject;
	TestTrue(TEXT("Custom C# profile report is valid JSON"), LoadAvidScriptCSharpBuildTestJsonObject(Config.ReportPath, ReportObject));
	if (!ReportObject.IsValid())
	{
		return true;
	}

	FString ReportModuleId;
	TestTrue(TEXT("Custom report declares module id"), ReportObject->TryGetStringField(TEXT("module_id"), ReportModuleId));
	TestEqual(TEXT("Custom report module id"), ReportModuleId, Config.ModuleId);

	const TSharedPtr<FJsonObject>* SourceObject = nullptr;
	TestTrue(TEXT("Custom report declares source object"), ReportObject->TryGetObjectField(TEXT("source"), SourceObject));
	if (SourceObject != nullptr && SourceObject->IsValid())
	{
		FString ReportSourceFile;
		TestTrue(TEXT("Custom report declares source file"), (*SourceObject)->TryGetStringField(TEXT("file"), ReportSourceFile));
		TestTrue(TEXT("Custom report source file points to custom source"), ReportSourceFile.EndsWith(TEXT("Saved/AvidScriptTests/CSharpProfiles/CustomMover/CustomMoverScript.cs")));
	}

	const TSharedPtr<FJsonObject>* ArtifactsObject = nullptr;
	TestTrue(TEXT("Custom report declares artifacts object"), ReportObject->TryGetObjectField(TEXT("artifacts"), ArtifactsObject));
	if (ArtifactsObject != nullptr && ArtifactsObject->IsValid())
	{
		FString WasmArtifactPath;
		FString ManifestArtifactPath;
		FString ReportArtifactPath;
		TestTrue(TEXT("Custom report declares wasm artifact"), (*ArtifactsObject)->TryGetStringField(TEXT("wasm_file"), WasmArtifactPath));
		TestTrue(TEXT("Custom report declares manifest artifact"), (*ArtifactsObject)->TryGetStringField(TEXT("manifest_file"), ManifestArtifactPath));
		TestTrue(TEXT("Custom report declares report artifact"), (*ArtifactsObject)->TryGetStringField(TEXT("report_file"), ReportArtifactPath));
		TestTrue(TEXT("Custom wasm artifact uses custom stem"), WasmArtifactPath.EndsWith(TEXT("Saved/AvidScriptCSharpGuest/CustomMover/custom_mover.csharp_adapter.wasm")));
		TestTrue(TEXT("Custom manifest artifact uses custom stem"), ManifestArtifactPath.EndsWith(TEXT("Saved/AvidScriptCSharpGuest/CustomMover/custom_mover.avidscript.json")));
		TestTrue(TEXT("Custom report artifact uses custom stem"), ReportArtifactPath.EndsWith(TEXT("Saved/AvidScriptCSharpGuest/CustomMover/custom_mover.csharp.report.json")));
	}

	TSharedPtr<FJsonObject> ManifestObject;
	TestTrue(TEXT("Custom C# profile manifest is valid JSON"), LoadAvidScriptCSharpBuildTestJsonObject(Config.ManifestPath, ManifestObject));
	if (ManifestObject.IsValid())
	{
		FString ManifestModuleId;
		TestTrue(TEXT("Custom manifest declares module id"), ManifestObject->TryGetStringField(TEXT("module_id"), ManifestModuleId));
		TestEqual(TEXT("Custom manifest module id"), ManifestModuleId, Config.ModuleId);
	}

	UWorld* World = nullptr;
	if (!CreateAvidScriptCSharpBuildTestWorld(World))
	{
		AddError(TEXT("Failed to create custom C# profile binding test world."));
		DestroyAvidScriptCSharpBuildTestWorld(World);
		return true;
	}

	AActor* Actor = SpawnAvidScriptCSharpBuildTestActor(*World);
	TestNotNull(TEXT("Custom C# profile binding actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyAvidScriptCSharpBuildTestWorld(World);
		return true;
	}

	FAvidScriptEditorComponentBindingResult BindingResult;
	TestTrue(TEXT("Custom C# profile report binds to actor"), FAvidScriptEditorComponentBindingService::ApplyCSharpReportToActor(Config.ReportPath, Actor, BindingResult));
	TestTrue(TEXT("Custom C# profile binding succeeds"), BindingResult.bSucceeded);
	TestNotNull(TEXT("Custom C# profile binding returns component"), BindingResult.Component);
	TestEqual(TEXT("Custom C# profile component manifest"), BindingResult.Component->GetScriptManifestPath(), Config.ManifestPath);

	DestroyAvidScriptCSharpBuildTestWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
