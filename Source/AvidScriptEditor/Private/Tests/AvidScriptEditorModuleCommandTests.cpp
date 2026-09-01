#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorModule.h"

#include "AvidScriptComponent.h"
#include "AvidScriptEditorCommandLauncher.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorMenuRegistrar.h"

#include "Components/SceneComponent.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
class FFakeAvidScriptEditorModuleLiveReloadWatchHost final
	: public IAvidScriptEditorCSharpLiveReloadWatchHost
{
public:
	virtual bool Start(
		const FString& WorkspaceRoot,
		FOnChangeBatch InOnChangeBatch,
		FString& OutErrorCategory,
		FString& OutErrorMessage) override
	{
		++StartCount;
		WatchedRoot = WorkspaceRoot;
		Callback = MoveTemp(InOnChangeBatch);
		bWatching = true;
		return true;
	}

	virtual void Stop() override
	{
		if (bWatching)
		{
			++StopCount;
		}
		bWatching = false;
		Callback = FOnChangeBatch();
	}

	virtual bool IsWatching() const override
	{
		return bWatching;
	}

	void Emit(const FString& FilePath)
	{
		if (!bWatching || !Callback)
		{
			return;
		}
		FAvidScriptEditorCSharpLiveReloadChangeBatch Batch;
		Batch.FilePaths.Add(FilePath);
		Callback(MoveTemp(Batch));
	}

	bool bWatching = false;
	int32 StartCount = 0;
	int32 StopCount = 0;
	FString WatchedRoot;
	FOnChangeBatch Callback;
};

bool CreateAvidScriptEditorModuleCSharpBindingWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptEditorModuleCSharpBindingWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

bool BeginAvidScriptEditorModuleCSharpBindingWorld(UWorld* World)
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

void DestroyAvidScriptEditorModuleCSharpBindingWorld(UWorld*& World)
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

FString NormalizeAvidScriptEditorModuleCSharpProfilePath(FString Path)
{
	Path = FPaths::ConvertRelativePathToFull(Path);
	FPaths::NormalizeFilename(Path);
	return Path;
}

FString GetAvidScriptEditorModuleCSharpProfileTestRoot()
{
	return NormalizeAvidScriptEditorModuleCSharpProfilePath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptEditorTests"),
		TEXT("ModuleCSharpProfile")));
}

FString MakeAvidScriptEditorModuleCSharpProfileSourceText()
{
	const FString SamplePath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectPluginsDir(),
		TEXT("AvidScript/Samples/CSharp/GeneratedBindingLifecycle/GeneratedBindingLifecycleScript.cs")));
	FString SourceText;
	FFileHelper::LoadFileToString(SourceText, *SamplePath);
	return SourceText;
}

bool WriteAvidScriptRejectedReloadManifestCandidate(
	const FString& ManifestPath,
	FString& OutOriginalText)
{
	OutOriginalText.Reset();
	if (!FFileHelper::LoadFileToString(OutOriginalText, *ManifestPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> ManifestObject;
	const TSharedRef<TJsonReader<>> Reader =
		TJsonReaderFactory<>::Create(OutOriginalText);
	if (!FJsonSerializer::Deserialize(Reader, ManifestObject)
		|| !ManifestObject.IsValid())
	{
		return false;
	}

	TArray<TSharedPtr<FJsonValue>> RequiredExports;
	const TArray<TSharedPtr<FJsonValue>>* ExistingRequiredExports = nullptr;
	if (ManifestObject->TryGetArrayField(
			TEXT("required_exports"),
			ExistingRequiredExports)
		&& ExistingRequiredExports != nullptr)
	{
		RequiredExports = *ExistingRequiredExports;
	}
	RequiredExports.Add(MakeShared<FJsonValueString>(
		TEXT("avid_phase45_missing_reload_export")));
	ManifestObject->SetArrayField(
		TEXT("required_exports"),
		MoveTemp(RequiredExports));

	FString RejectedText;
	const TSharedRef<TJsonWriter<>> Writer =
		TJsonWriterFactory<>::Create(&RejectedText);
	return FJsonSerializer::Serialize(ManifestObject.ToSharedRef(), Writer)
		&& FFileHelper::SaveStringToFile(RejectedText, *ManifestPath);
}

AActor* SpawnAvidScriptEditorModuleCSharpBindingActor(UWorld& World)
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
	FAvidScriptEditorModuleSampleCommandConfigTest,
	"AvidScript.Editor.Module.SampleCommandConfigSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorModuleSampleCommandConfigTest::RunTest(const FString& Parameters)
{
	const FString SourcePath = FAvidScriptEditorModule::GetSampleCommandSourcePath();
	TestTrue(TEXT("Sample command points to actor_set_location.avid"), SourcePath.EndsWith(TEXT("Plugins/AvidScript/Samples/AvidScript/ActorSetLocation/actor_set_location.avid")));
	TestTrue(TEXT("Sample command source exists"), FPaths::FileExists(SourcePath));

	FAvidScriptEditorCommandLaunchConfig CommandConfig;
	FString ErrorMessage;
	TestTrue(TEXT("Sample command default config can be built"), FAvidScriptEditorModule::MakeSampleCommandConfig(CommandConfig, ErrorMessage));
	TestEqual(TEXT("Sample command uses sample source"), CommandConfig.SourcePath, SourcePath);
	TestTrue(TEXT("Sample command uses default ActorHost bindings"), CommandConfig.BindingsPath.EndsWith(TEXT("Bindings/ActorHostBindings.avidscript.json")));
	TestTrue(TEXT("Sample command output root uses source name"), CommandConfig.OutputRoot.EndsWith(TEXT("Saved/AvidScriptGenerated/actor_set_location")));
	TestTrue(TEXT("Sample command report path uses source name"), CommandConfig.ReportPath.EndsWith(TEXT("Saved/AvidScriptReports/actor_set_location.frontend.report.json")));
	TestFalse(TEXT("Sample command compiles by default"), CommandConfig.bSkipCompile);
	TestTrue(TEXT("Sample command config has no error on success"), ErrorMessage.IsEmpty());

	FAvidScriptEditorMenuEntryConfig MenuConfig = FAvidScriptEditorModule::MakeSampleMenuEntryConfig(FSimpleDelegate::CreateLambda([]() {
	}));
	TestEqual(TEXT("Sample command owner"), MenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("Sample command menu"), MenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("Sample command section"), MenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("Sample command entry"), MenuConfig.EntryName, FName(TEXT("AvidScript.RunSampleCommand")));
	TestFalse(TEXT("Sample command label is set"), MenuConfig.Label.IsEmpty());
	TestFalse(TEXT("Sample command tooltip is set"), MenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("Sample command execute action is bound"), MenuConfig.ExecuteAction.IsBound());

	const FString CSharpReportPath = FAvidScriptEditorModule::GetCSharpActorLifecycleReportPath();
	TestTrue(TEXT("C# report path uses ActorLifecycle report"), CSharpReportPath.EndsWith(TEXT("Saved/AvidScriptCSharpGuest/ActorLifecycle/actor_lifecycle.csharp.report.json")));

	const FString CSharpBuildScriptPath = FAvidScriptEditorModule::GetCSharpActorLifecycleBuildScriptPath();
	TestTrue(TEXT("C# build script path uses ActorLifecycle wrapper"), CSharpBuildScriptPath.EndsWith(TEXT("Plugins/AvidScript/Build/BuildCSharpActorLifecycle.ps1")));
	TestTrue(TEXT("C# build script exists"), FPaths::FileExists(CSharpBuildScriptPath));

	FAvidScriptEditorMenuEntryConfig CSharpBindMenuConfig =
		FAvidScriptEditorModule::MakeCSharpActorLifecycleBindMenuEntryConfig(FSimpleDelegate::CreateLambda([]() {
		}));
	TestEqual(TEXT("C# bind command owner"), CSharpBindMenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("C# bind command menu"), CSharpBindMenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("C# bind command section"), CSharpBindMenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("C# bind command entry"), CSharpBindMenuConfig.EntryName, FName(TEXT("AvidScript.BindCSharpActorLifecycleReport")));
	TestFalse(TEXT("C# bind command label is set"), CSharpBindMenuConfig.Label.IsEmpty());
	TestFalse(TEXT("C# bind command tooltip is set"), CSharpBindMenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("C# bind command execute action is bound"), CSharpBindMenuConfig.ExecuteAction.IsBound());

	FAvidScriptEditorMenuEntryConfig CSharpBuildAndBindMenuConfig =
		FAvidScriptEditorModule::MakeCSharpActorLifecycleBuildAndBindMenuEntryConfig(FSimpleDelegate::CreateLambda([]() {
		}));
	TestEqual(TEXT("C# build-and-bind command owner"), CSharpBuildAndBindMenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("C# build-and-bind command menu"), CSharpBuildAndBindMenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("C# build-and-bind command section"), CSharpBuildAndBindMenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("C# build-and-bind command entry"), CSharpBuildAndBindMenuConfig.EntryName, FName(TEXT("AvidScript.BuildAndBindCSharpActorLifecycle")));
	TestFalse(TEXT("C# build-and-bind command label is set"), CSharpBuildAndBindMenuConfig.Label.IsEmpty());
	TestFalse(TEXT("C# build-and-bind command tooltip is set"), CSharpBuildAndBindMenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("C# build-and-bind command execute action is bound"), CSharpBuildAndBindMenuConfig.ExecuteAction.IsBound());

	const FString DefaultCSharpProfilePath = FAvidScriptEditorModule::GetDefaultCSharpProfilePath();
	TestTrue(TEXT("Default C# profile path uses Saved profile file"), DefaultCSharpProfilePath.EndsWith(TEXT("Saved/AvidScriptCSharpProfiles/default.csharp-profile.json")));

	FAvidScriptEditorMenuEntryConfig CSharpProfileBuildAndBindMenuConfig =
		FAvidScriptEditorModule::MakeCSharpProfileBuildAndBindMenuEntryConfig(FSimpleDelegate::CreateLambda([]() {
		}));
	TestEqual(TEXT("C# profile build-and-bind command owner"), CSharpProfileBuildAndBindMenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("C# profile build-and-bind command menu"), CSharpProfileBuildAndBindMenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("C# profile build-and-bind command section"), CSharpProfileBuildAndBindMenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("C# profile build-and-bind command entry"), CSharpProfileBuildAndBindMenuConfig.EntryName, FName(TEXT("AvidScript.BuildAndBindCSharpProfile")));
	TestFalse(TEXT("C# profile build-and-bind command label is set"), CSharpProfileBuildAndBindMenuConfig.Label.IsEmpty());
	TestFalse(TEXT("C# profile build-and-bind command tooltip is set"), CSharpProfileBuildAndBindMenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("C# profile build-and-bind command execute action is bound"), CSharpProfileBuildAndBindMenuConfig.ExecuteAction.IsBound());

	FAvidScriptEditorMenuEntryConfig CSharpProfileTemplateMenuConfig =
		FAvidScriptEditorModule::MakeCSharpProfileTemplateMenuEntryConfig(FSimpleDelegate::CreateLambda([]() {
		}));
	TestEqual(TEXT("C# profile template command owner"), CSharpProfileTemplateMenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("C# profile template command menu"), CSharpProfileTemplateMenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("C# profile template command section"), CSharpProfileTemplateMenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("C# profile template command entry"), CSharpProfileTemplateMenuConfig.EntryName, FName(TEXT("AvidScript.CreateDefaultCSharpProfile")));
	TestFalse(TEXT("C# profile template command label is set"), CSharpProfileTemplateMenuConfig.Label.IsEmpty());
	TestFalse(TEXT("C# profile template command tooltip is set"), CSharpProfileTemplateMenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("C# profile template command execute action is bound"), CSharpProfileTemplateMenuConfig.ExecuteAction.IsBound());

	const FString ProjectWorkspaceProfilePath = FAvidScriptEditorModule::GetProjectCSharpWorkspaceProfilePath();
	TestTrue(
		TEXT("Project C# workspace profile uses project Scripts directory"),
		ProjectWorkspaceProfilePath.EndsWith(TEXT("Scripts/AvidScript/default.csharp-profile.json")));

	FAvidScriptEditorMenuEntryConfig CSharpWorkspaceCreateMenuConfig =
		FAvidScriptEditorModule::MakeCSharpWorkspaceCreateMenuEntryConfig(FSimpleDelegate::CreateLambda([]() {
		}));
	TestEqual(TEXT("C# workspace create command owner"), CSharpWorkspaceCreateMenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("C# workspace create command menu"), CSharpWorkspaceCreateMenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("C# workspace create command section"), CSharpWorkspaceCreateMenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("C# workspace create command entry"), CSharpWorkspaceCreateMenuConfig.EntryName, FName(TEXT("AvidScript.CreateProjectCSharpGameplayWorkspace")));
	TestTrue(TEXT("C# workspace create command label is explicit"), CSharpWorkspaceCreateMenuConfig.Label.ToString().Contains(TEXT("Project C# Gameplay Workspace")));
	TestFalse(TEXT("C# workspace create command tooltip is set"), CSharpWorkspaceCreateMenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("C# workspace create command execute action is bound"), CSharpWorkspaceCreateMenuConfig.ExecuteAction.IsBound());
	for (const EAvidScriptEditorIdeKind Ide : {
		EAvidScriptEditorIdeKind::SystemDefault,
		EAvidScriptEditorIdeKind::VisualStudio,
		EAvidScriptEditorIdeKind::Rider,
		EAvidScriptEditorIdeKind::VisualStudioCode })
	{
		const FAvidScriptEditorMenuEntryConfig OpenWorkspaceConfig =
			FAvidScriptEditorModule::MakeCSharpWorkspaceOpenMenuEntryConfig(
				Ide,
				FSimpleDelegate::CreateLambda([]() {}));
		TestFalse(TEXT("C# workspace open command entry is set"), OpenWorkspaceConfig.EntryName.IsNone());
		TestFalse(TEXT("C# workspace open command label is set"), OpenWorkspaceConfig.Label.IsEmpty());
		TestFalse(TEXT("C# workspace open command tooltip is set"), OpenWorkspaceConfig.ToolTip.IsEmpty());
		TestTrue(TEXT("C# workspace open command action is bound"), OpenWorkspaceConfig.ExecuteAction.IsBound());
	}
	TestNotEqual(
		TEXT("C# workspace IDE commands use distinct entries"),
		FAvidScriptEditorModule::GetCSharpWorkspaceOpenEntryName(EAvidScriptEditorIdeKind::VisualStudio),
		FAvidScriptEditorModule::GetCSharpWorkspaceOpenEntryName(EAvidScriptEditorIdeKind::Rider));

	FAvidScriptEditorMenuEntryConfig CSharpWorkspaceBuildAndBindMenuConfig =
		FAvidScriptEditorModule::MakeCSharpWorkspaceBuildAndBindMenuEntryConfig(FSimpleDelegate::CreateLambda([]() {
		}));
	TestEqual(TEXT("C# workspace build command owner"), CSharpWorkspaceBuildAndBindMenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("C# workspace build command menu"), CSharpWorkspaceBuildAndBindMenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("C# workspace build command section"), CSharpWorkspaceBuildAndBindMenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("C# workspace build command entry"), CSharpWorkspaceBuildAndBindMenuConfig.EntryName, FName(TEXT("AvidScript.BuildAndBindProjectCSharpGameplay")));
	TestTrue(TEXT("C# workspace build command label is explicit"), CSharpWorkspaceBuildAndBindMenuConfig.Label.ToString().Contains(TEXT("Build And Bind Project C# Gameplay Script")));
	TestFalse(TEXT("C# workspace build command tooltip is set"), CSharpWorkspaceBuildAndBindMenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("C# workspace build command execute action is bound"), CSharpWorkspaceBuildAndBindMenuConfig.ExecuteAction.IsBound());

	FAvidScriptEditorMenuEntryConfig CSharpLiveReloadStartMenuConfig =
		FAvidScriptEditorModule::MakeCSharpWorkspaceLiveReloadStartMenuEntryConfig(
			FSimpleDelegate::CreateLambda([]() {
			}));
	TestEqual(TEXT("C# live reload start command owner"), CSharpLiveReloadStartMenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("C# live reload start command menu"), CSharpLiveReloadStartMenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("C# live reload start command section"), CSharpLiveReloadStartMenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("C# live reload start command entry"), CSharpLiveReloadStartMenuConfig.EntryName, FName(TEXT("AvidScript.StartProjectCSharpLiveReload")));
	TestFalse(TEXT("C# live reload start tooltip is set"), CSharpLiveReloadStartMenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("C# live reload start action is bound"), CSharpLiveReloadStartMenuConfig.ExecuteAction.IsBound());

	FAvidScriptEditorMenuEntryConfig CSharpLiveReloadStopMenuConfig =
		FAvidScriptEditorModule::MakeCSharpWorkspaceLiveReloadStopMenuEntryConfig(
			FSimpleDelegate::CreateLambda([]() {
			}));
	TestEqual(TEXT("C# live reload stop command owner"), CSharpLiveReloadStopMenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("C# live reload stop command menu"), CSharpLiveReloadStopMenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("C# live reload stop command section"), CSharpLiveReloadStopMenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("C# live reload stop command entry"), CSharpLiveReloadStopMenuConfig.EntryName, FName(TEXT("AvidScript.StopProjectCSharpLiveReload")));
	TestFalse(TEXT("C# live reload stop tooltip is set"), CSharpLiveReloadStopMenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("C# live reload stop action is bound"), CSharpLiveReloadStopMenuConfig.ExecuteAction.IsBound());

	FAvidScriptEditorMenuEntryConfig DebuggerMenuConfig =
		FAvidScriptEditorModule::MakeDebuggerMenuEntryConfig(
			FSimpleDelegate::CreateLambda([]() {
			}));
	TestEqual(TEXT("debugger command owner"), DebuggerMenuConfig.OwnerName, FName(TEXT("AvidScriptEditor")));
	TestEqual(TEXT("debugger command menu"), DebuggerMenuConfig.MenuName, FName(TEXT("LevelEditor.MainMenu.Tools")));
	TestEqual(TEXT("debugger command section"), DebuggerMenuConfig.SectionName, FName(TEXT("AvidScript")));
	TestEqual(TEXT("debugger command entry"), DebuggerMenuConfig.EntryName, FName(TEXT("AvidScript.OpenDebugger")));
	TestEqual(TEXT("debugger tab name"), FAvidScriptEditorModule::GetDebuggerTabName(), FName(TEXT("AvidScript.Debugger")));
	TestFalse(TEXT("debugger command label is set"), DebuggerMenuConfig.Label.IsEmpty());
	TestFalse(TEXT("debugger command tooltip is set"), DebuggerMenuConfig.ToolTip.IsEmpty());
	TestTrue(TEXT("debugger command execute action is bound"), DebuggerMenuConfig.ExecuteAction.IsBound());

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorModuleCSharpBindSelectedActorTest,
	"AvidScript.Editor.Module.CSharpBindSelectedActorSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorModuleCSharpBindSelectedActorTest::RunTest(const FString& Parameters)
{
	const FString ReportPath = FAvidScriptEditorModule::GetCSharpActorLifecycleReportPath();
	if (!FPaths::FileExists(ReportPath))
	{
		AddWarning(FString::Printf(
			TEXT("C# report is missing; run BuildCSharpActorLifecycle.ps1 before this module binding smoke. report=%s"),
			*ReportPath));
		return true;
	}

	if (GEditor == nullptr)
	{
		AddError(TEXT("GEditor is not available for module C# binding smoke."));
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateAvidScriptEditorModuleCSharpBindingWorld(World))
	{
		AddError(TEXT("Failed to create module C# binding test world."));
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		return true;
	}

	AActor* Actor = SpawnAvidScriptEditorModuleCSharpBindingActor(*World);
	TestNotNull(TEXT("Module binding test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		return true;
	}

	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, false, true, false);

	FAvidScriptEditorModule& Module =
		FModuleManager::LoadModuleChecked<FAvidScriptEditorModule>(TEXT("AvidScriptEditor"));

	FAvidScriptEditorComponentBindingResult Result;
	TestTrue(TEXT("Module C# binding command succeeds"), Module.ExecuteCSharpActorLifecycleBinding(Result));
	TestTrue(TEXT("Module C# binding result succeeds"), Result.bSucceeded);
	TestEqual(TEXT("Module C# binding report path"), Result.ReportPath, ReportPath);
	TestNotNull(TEXT("Module C# binding returns component"), Result.Component);
	TestEqual(TEXT("Module binding actor component"), Actor->FindComponentByClass<UAvidScriptComponent>(), Result.Component);

	DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorModuleCSharpBuildAndBindSelectedActorTest,
	"AvidScript.Editor.Module.CSharpBuildAndBindSelectedActorSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorModuleCSharpBuildAndBindSelectedActorTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr)
	{
		AddError(TEXT("GEditor is not available for module C# build-and-bind smoke."));
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateAvidScriptEditorModuleCSharpBindingWorld(World))
	{
		AddError(TEXT("Failed to create module C# build-and-bind test world."));
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		return true;
	}

	AActor* Actor = SpawnAvidScriptEditorModuleCSharpBindingActor(*World);
	TestNotNull(TEXT("Module build-and-bind test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		return true;
	}

	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, false, true, false);

	FAvidScriptEditorModule& Module =
		FModuleManager::LoadModuleChecked<FAvidScriptEditorModule>(TEXT("AvidScriptEditor"));

	FAvidScriptEditorCSharpBuildResult BuildResult;
	FAvidScriptEditorComponentBindingResult BindingResult;
	TestTrue(TEXT("Module C# build-and-bind command succeeds"), Module.ExecuteCSharpActorLifecycleBuildAndBinding(BuildResult, BindingResult));
	TestTrue(TEXT("Module C# build result succeeds"), BuildResult.bSucceeded);
	TestEqual(TEXT("Module C# build process exit"), BuildResult.ProcessExitCode, 0);
	TestEqual(TEXT("Module C# build report path"), BuildResult.ReportPath, FAvidScriptEditorModule::GetCSharpActorLifecycleReportPath());
	TestTrue(TEXT("Module C# build report exists"), FPaths::FileExists(BuildResult.ReportPath));
	TestTrue(TEXT("Module C# build-and-bind result succeeds"), BindingResult.bSucceeded);
	TestEqual(TEXT("Module C# build-and-bind report path"), BindingResult.ReportPath, BuildResult.ReportPath);
	TestNotNull(TEXT("Module C# build-and-bind returns component"), BindingResult.Component);
	TestEqual(TEXT("Module build-and-bind actor component"), Actor->FindComponentByClass<UAvidScriptComponent>(), BindingResult.Component);

	DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorModuleCSharpProfileBuildAndBindSelectedActorTest,
	"AvidScript.Editor.Module.CSharpProfileBuildAndBindSelectedActorSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorModuleCSharpProfileBuildAndBindSelectedActorTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr)
	{
		AddError(TEXT("GEditor is not available for module C# profile build-and-bind smoke."));
		return true;
	}

	const FString TestRoot = GetAvidScriptEditorModuleCSharpProfileTestRoot();
	TestTrue(TEXT("Module C# profile test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString SourcePath = NormalizeAvidScriptEditorModuleCSharpProfilePath(FPaths::Combine(TestRoot, TEXT("ProfileMenuMover.cs")));
	const FString SourceText = MakeAvidScriptEditorModuleCSharpProfileSourceText();
	if (!TestFalse(TEXT("Generated binding lifecycle source is available"), SourceText.IsEmpty()))
	{
		return false;
	}
	TestTrue(TEXT("Module C# profile source can be written"), FFileHelper::SaveStringToFile(SourceText, *SourcePath));

	const FString OutputRoot = NormalizeAvidScriptEditorModuleCSharpProfilePath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptCSharpGuest"),
		TEXT("ProfileMenuMover")));
	const FString ProfilePath = NormalizeAvidScriptEditorModuleCSharpProfilePath(FPaths::Combine(TestRoot, TEXT("profile_menu_mover.csharp-profile.json")));
	const FString ProjectPath = FAvidScriptEditorCSharpBuildService::GetDefaultActorLifecycleProjectPath();
	const FString ProfileText = FString::Printf(
		TEXT("{\n")
		TEXT("  \"schema_version\": 1,\n")
		TEXT("  \"language\": \"csharp\",\n")
		TEXT("  \"source_path\": \"%s\",\n")
		TEXT("  \"project_path\": \"%s\",\n")
		TEXT("  \"module_id\": \"csharp_profile_menu_mover\",\n")
		TEXT("  \"artifact_stem\": \"profile_menu_mover\",\n")
		TEXT("  \"output_root\": \"%s\"\n")
		TEXT("}\n"),
		*SourcePath,
		*ProjectPath,
		*OutputRoot);
	TestTrue(TEXT("Module C# profile can be written"), FFileHelper::SaveStringToFile(ProfileText, *ProfilePath));

	UWorld* World = nullptr;
	if (!CreateAvidScriptEditorModuleCSharpBindingWorld(World))
	{
		AddError(TEXT("Failed to create module C# profile build-and-bind test world."));
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		return true;
	}

	AActor* Actor = SpawnAvidScriptEditorModuleCSharpBindingActor(*World);
	TestNotNull(TEXT("Module C# profile build-and-bind test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		return true;
	}

	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, false, true, false);

	FAvidScriptEditorModule& Module =
		FModuleManager::LoadModuleChecked<FAvidScriptEditorModule>(TEXT("AvidScriptEditor"));

	FAvidScriptEditorCSharpBuildResult BuildResult;
	FAvidScriptEditorComponentBindingResult BindingResult;
	const bool bBuildAndBindSucceeded = Module.ExecuteCSharpProfileBuildAndBinding(
		ProfilePath,
		BuildResult,
		BindingResult);
	if (!TestTrue(
		TEXT("Module C# profile builds and binds with generated bindings"),
		bBuildAndBindSucceeded))
	{
		AddError(BuildResult.ErrorMessage + TEXT("\n") + BuildResult.Stderr);
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		return false;
	}
	TestTrue(TEXT("Module C# profile build succeeds"), BuildResult.bSucceeded);
	TestEqual(TEXT("Module C# profile process exits successfully"), BuildResult.ProcessExitCode, 0);
	TestTrue(TEXT("Module C# profile binding package exists"), FPaths::FileExists(BuildResult.BindingPackagePath));
	TestEqual(TEXT("Module C# profile module id"), BuildResult.ModuleId, FString(TEXT("csharp_profile_menu_mover")));
	TestEqual(TEXT("Module C# profile artifact stem"), BuildResult.ArtifactStem, FString(TEXT("profile_menu_mover")));
	TestEqual(TEXT("Module C# profile build report path"), BuildResult.ReportPath, FAvidScriptEditorCSharpBuildService::MakeReportPathForOutputRoot(OutputRoot, TEXT("profile_menu_mover")));
	TestTrue(TEXT("Module C# profile build report exists"), FPaths::FileExists(BuildResult.ReportPath));
	TestTrue(TEXT("Module C# profile manifest exists"), FPaths::FileExists(BuildResult.ManifestPath));
	TestTrue(TEXT("Module C# profile binding succeeds"), BindingResult.bSucceeded);
	TestNotNull(TEXT("Module C# profile returns the bound component"), BindingResult.Component);
	TestEqual(TEXT("Module C# profile binds the selected Actor"), Actor->FindComponentByClass<UAvidScriptComponent>(), BindingResult.Component);

	DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorModuleCSharpWorkspaceBuildAndBindSelectedActorTest,
	"AvidScript.Editor.Module.CSharpWorkspaceBuildAndBindSelectedActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorModuleCSharpWorkspaceBuildAndBindSelectedActorTest::RunTest(const FString& Parameters)
{
	if (GEditor == nullptr)
	{
		AddError(TEXT("GEditor is not available for project C# workspace build-and-bind."));
		return false;
	}

	const FString TestRoot = NormalizeAvidScriptEditorModuleCSharpProfilePath(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript/Tests/P44/ModuleWorkspaceCommand")));
	IFileManager::Get().DeleteDirectory(*TestRoot, false, true);

	FAvidScriptEditorCSharpWorkspaceConfig WorkspaceConfig;
	WorkspaceConfig.WorkspaceRoot = FPaths::Combine(TestRoot, TEXT("Workspace"));
	WorkspaceConfig.GeneratedRoot = FPaths::Combine(TestRoot, TEXT("Generated"));
	WorkspaceConfig.BindingPackageRoot = FPaths::Combine(TestRoot, TEXT("BindingPackages"));
	WorkspaceConfig.OutputRoot = FPaths::Combine(TestRoot, TEXT("Output"));

	UWorld* World = nullptr;
	if (!CreateAvidScriptEditorModuleCSharpBindingWorld(World))
	{
		AddError(TEXT("Failed to create project C# workspace command test world."));
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
		return false;
	}
	if (!TestTrue(
			TEXT("Project C# workspace command world begins play"),
			BeginAvidScriptEditorModuleCSharpBindingWorld(World)))
	{
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
		return false;
	}

	AActor* Actor = SpawnAvidScriptEditorModuleCSharpBindingActor(*World);
	if (!TestNotNull(TEXT("Project C# workspace command actor spawns"), Actor))
	{
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
		return false;
	}
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(Actor, true, false, true, false);

	double Now = 1.0;
	AActor* ReloadTarget = nullptr;
	int32 ReloadBuildCount = 0;
	int32 EditorHeartbeatCount = 0;
	int32 LastBuildHeartbeatCount = 0;
	bool bRejectNextReloadCandidate = false;
	FString ReloadCandidateManifestPath;
	FFakeAvidScriptEditorModuleLiveReloadWatchHost* FakeWatchHost =
		new FFakeAvidScriptEditorModuleLiveReloadWatchHost();
	TUniquePtr<FAvidScriptEditorCSharpLiveReloadService> LiveReloadService(
		new FAvidScriptEditorCSharpLiveReloadService(
			TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeWatchHost),
			[&ReloadBuildCount]()
			{
				++ReloadBuildCount;
				return FAvidScriptEditorCSharpAsyncBuildJobFactory::Create();
			},
			[this,
			 &ReloadTarget,
			 &bRejectNextReloadCandidate,
			 &ReloadCandidateManifestPath](
				const FString& ReportPath,
				AActor* Target,
				FAvidScriptEditorComponentBindingResult& OutBindingResult)
			{
				ReloadTarget = Target;
				FString OriginalManifestText;
				if (bRejectNextReloadCandidate)
				{
					bRejectNextReloadCandidate = false;
					if (!TestTrue(
							TEXT("Real reload candidate manifest can be made invalid"),
							WriteAvidScriptRejectedReloadManifestCandidate(
								ReloadCandidateManifestPath,
								OriginalManifestText)))
					{
						return false;
					}
				}

				const bool bApplied = FAvidScriptEditorComponentBindingService::
					ApplyCSharpReportToActor(
						ReportPath,
						Target,
						OutBindingResult);
				if (!OriginalManifestText.IsEmpty())
				{
					TestTrue(
						TEXT("Rejected candidate manifest is restored after binding probe"),
						FFileHelper::SaveStringToFile(
							OriginalManifestText,
							*ReloadCandidateManifestPath));
				}
				return bApplied;
			},
			[&Now]() { return Now; }));
	FAvidScriptEditorCSharpLiveReloadService* LiveReloadServicePtr = LiveReloadService.Get();
	const FTSTicker::FDelegateHandle HeartbeatHandle =
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda(
				[&EditorHeartbeatCount](float)
				{
					++EditorHeartbeatCount;
					return true;
				}));
	ON_SCOPE_EXIT
	{
		FTSTicker::GetCoreTicker().RemoveTicker(HeartbeatHandle);
	};
	auto PumpLiveReloadBuild = [
		LiveReloadServicePtr,
		&EditorHeartbeatCount,
		&LastBuildHeartbeatCount]()
	{
		const int32 InitialHeartbeatCount = EditorHeartbeatCount;
		const double DeadlineSeconds = FPlatformTime::Seconds() + 90.0;
		while (LiveReloadServicePtr->IsRunning()
			&& LiveReloadServicePtr->GetLastResult().Status ==
				EAvidScriptEditorCSharpLiveReloadServiceStatus::Building
			&& FPlatformTime::Seconds() < DeadlineSeconds)
		{
			FPlatformProcess::Sleep(0.01f);
			FTSTicker::GetCoreTicker().Tick(0.01f);
		}
		LastBuildHeartbeatCount =
			EditorHeartbeatCount - InitialHeartbeatCount;
		return LiveReloadServicePtr->GetLastResult().Status !=
			EAvidScriptEditorCSharpLiveReloadServiceStatus::Building;
	};
	FAvidScriptEditorModule Module(MoveTemp(LiveReloadService));
	FString OriginalSourceText;
	FString OriginalSourcePath;
	bool bRestoreOriginalSource = false;
	bool bCleanupCompleted = false;
	auto CleanupWorkspaceReloadTest = [&]()
	{
		if (bCleanupCompleted)
		{
			return;
		}
		bCleanupCompleted = true;
		if (bRestoreOriginalSource)
		{
			TestTrue(
				TEXT("Project C# source is restored during test cleanup"),
				FFileHelper::SaveStringToFile(
					OriginalSourceText,
					*OriginalSourcePath));
		}
		FAvidScriptEditorCSharpLiveReloadServiceResult CleanupStopResult;
		Module.ExecuteStopCSharpWorkspaceLiveReload(CleanupStopResult);
		DestroyAvidScriptEditorModuleCSharpBindingWorld(World);
		IFileManager::Get().DeleteDirectory(*TestRoot, false, true);
	};
	FAvidScriptEditorCSharpWorkspaceResult WorkspaceResult;
	FAvidScriptEditorCSharpBuildResult BuildResult;
	FAvidScriptEditorComponentBindingResult BindingResult;
	FAvidScriptEditorCSharpLiveReloadServiceResult LiveReloadResult;
	const bool bSucceeded = Module.ExecuteStartCSharpWorkspaceLiveReload(
		WorkspaceConfig,
		WorkspaceResult,
		BuildResult,
		BindingResult,
		LiveReloadResult);
	if (!TestTrue(TEXT("Project C# workspace command builds and binds"), bSucceeded))
	{
		AddError(
			WorkspaceResult.ErrorMessage
			+ TEXT("\n")
			+ BuildResult.ErrorMessage
			+ TEXT("\n")
			+ BuildResult.Stderr);
		CleanupWorkspaceReloadTest();
		return false;
	}

	TestTrue(TEXT("Project C# workspace result succeeds"), WorkspaceResult.bSucceeded);
	TestEqual(TEXT("Project C# workspace creates six user files"), WorkspaceResult.CreatedUserFileCount, 6);
	TestTrue(TEXT("Project C# workspace source exists"), FPaths::FileExists(WorkspaceResult.SourcePath));
	TestTrue(TEXT("Project C# workspace project exists"), FPaths::FileExists(WorkspaceResult.ProjectPath));
	TestTrue(TEXT("Project C# workspace solution exists"), FPaths::FileExists(WorkspaceResult.SolutionPath));
	TestTrue(TEXT("Project C# workspace editor config exists"), FPaths::FileExists(WorkspaceResult.EditorConfigPath));
	TestTrue(TEXT("Project C# workspace profile exists"), FPaths::FileExists(WorkspaceResult.ProfilePath));
	TestTrue(TEXT("Project C# workspace facade exists"), FPaths::FileExists(WorkspaceResult.FacadePath));
	TestTrue(TEXT("Project C# workspace build succeeds"), BuildResult.bSucceeded);
	TestEqual(TEXT("Project C# workspace build module"), BuildResult.ModuleId, FString(TEXT("csharp_project_gameplay")));
	TestEqual(TEXT("Project C# workspace uses its source"), BuildResult.SourcePath, WorkspaceResult.SourcePath);
	TestEqual(TEXT("Project C# workspace performs bootstrap and final builds"), BuildResult.BuildInvocationCount, 2);
	TestTrue(TEXT("Project C# workspace report exists"), FPaths::FileExists(BuildResult.ReportPath));
	TestTrue(TEXT("Project C# workspace manifest exists"), FPaths::FileExists(BuildResult.ManifestPath));
	ReloadCandidateManifestPath = BuildResult.ManifestPath;
	TestTrue(TEXT("Project C# workspace runtime package exists"), FPaths::FileExists(BuildResult.BindingPackagePath));
	TestTrue(TEXT("Project C# workspace binding succeeds"), BindingResult.bSucceeded);
	TestNotNull(TEXT("Project C# workspace returns component"), BindingResult.Component);
	TestEqual(TEXT("Project C# workspace binds selected Actor"), Actor->FindComponentByClass<UAvidScriptComponent>(), BindingResult.Component);
	TestTrue(TEXT("Project C# live reload starts"), Module.IsCSharpWorkspaceLiveReloadRunning());
	TestEqual(TEXT("Project C# live reload registers one watcher"), FakeWatchHost->StartCount, 1);
	TestEqual(TEXT("Project C# live reload watches workspace"), FakeWatchHost->WatchedRoot, WorkspaceResult.WorkspaceRoot);
	TestEqual(TEXT("Project C# live reload fixes bound Actor"), LiveReloadResult.TargetActorPath, Actor->GetPathName());
	if (BindingResult.Component == nullptr)
	{
		CleanupWorkspaceReloadTest();
		return false;
	}
	OriginalSourcePath = WorkspaceResult.SourcePath;
	if (!TestTrue(
			TEXT("Project C# live reload source can be read for failure recovery"),
			FFileHelper::LoadFileToString(OriginalSourceText, *OriginalSourcePath)))
	{
		CleanupWorkspaceReloadTest();
		return false;
	}
	bRestoreOriginalSource = true;
	ON_SCOPE_EXIT
	{
		CleanupWorkspaceReloadTest();
	};
	BindingResult.Component->TickComponent(0.5f, LEVELTICK_All, nullptr);
	TestTrue(
		TEXT("Initial C# runtime accumulates persistent rotation state"),
		FMath::IsNearlyEqual(Actor->GetActorScale3D().X, 1.045, 0.001));

	FAvidScriptEditorCSharpWorkspaceResult RepeatWorkspaceResult;
	FAvidScriptEditorCSharpBuildResult RepeatBuildResult;
	FAvidScriptEditorComponentBindingResult RepeatBindingResult;
	FAvidScriptEditorCSharpLiveReloadServiceResult RepeatLiveReloadResult;
	TestFalse(
		TEXT("Project C# live reload rejects repeated start"),
		Module.ExecuteStartCSharpWorkspaceLiveReload(
			WorkspaceConfig,
			RepeatWorkspaceResult,
			RepeatBuildResult,
			RepeatBindingResult,
			RepeatLiveReloadResult));
	TestTrue(TEXT("Repeated start leaves live reload running"), RepeatLiveReloadResult.bRunning);
	TestEqual(
		TEXT("Repeated start returns a fresh start failure status"),
		RepeatLiveReloadResult.Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::StartFailed);
	TestEqual(
		TEXT("Repeated start has a structured category"),
		RepeatLiveReloadResult.ErrorCategory,
		FString(TEXT("live_reload_already_running")));
	TestEqual(
		TEXT("Repeated start preserves the fixed Actor identity"),
		RepeatLiveReloadResult.TargetActorPath,
		Actor->GetPathName());
	TestEqual(TEXT("Repeated start does not rebuild"), RepeatBuildResult.BuildInvocationCount, 0);
	TestEqual(TEXT("Repeated start does not register another watcher"), FakeWatchHost->StartCount, 1);
	FString ReloadedSourceText = OriginalSourceText;
	const int32 DeclarationReplacementCount = ReloadedSourceText.ReplaceInline(
		TEXT("    private static float TotalRotationDegrees;"),
		TEXT("    [AvidStateAlias(\"TotalRotationDegrees\")]\n    private static float AccumulatedRotationDegrees;"),
		ESearchCase::CaseSensitive);
	const int32 AccumulationReplacementCount = ReloadedSourceText.ReplaceInline(
		TEXT("TotalRotationDegrees += RotationSpeedDegreesPerSecond * deltaSeconds;"),
		TEXT("AccumulatedRotationDegrees += RotationSpeedDegreesPerSecond * deltaSeconds;"),
		ESearchCase::CaseSensitive);
	const int32 ReadReplacementCount = ReloadedSourceText.ReplaceInline(
		TEXT("TotalRotationDegrees / 1000.0f"),
		TEXT("AccumulatedRotationDegrees / 1000.0f"),
		ESearchCase::CaseSensitive);
	const int32 SpeedReplacementCount = ReloadedSourceText.ReplaceInline(
		TEXT("90.0f"),
		TEXT("180.0f"),
		ESearchCase::CaseSensitive);
	bool bReloadAnchorsValid = true;
	bReloadAnchorsValid &= TestEqual(
		TEXT("Reload source declaration anchor matches exactly once"),
		DeclarationReplacementCount,
		1);
	bReloadAnchorsValid &= TestEqual(
		TEXT("Reload source accumulation anchor matches exactly once"),
		AccumulationReplacementCount,
		1);
	bReloadAnchorsValid &= TestEqual(
		TEXT("Reload source read anchor matches exactly once"),
		ReadReplacementCount,
		1);
	bReloadAnchorsValid &= TestEqual(
		TEXT("Reload source speed anchor matches exactly once"),
		SpeedReplacementCount,
		1);
	if (!bReloadAnchorsValid)
	{
		AddError(TEXT("Reload source anchors must each match once before the fixture writes candidate source."));
		return false;
	}
	TestNotEqual(TEXT("Reload source changes gameplay behavior"), ReloadedSourceText, OriginalSourceText);
	TestTrue(TEXT("Reload source renames the persisted field"), ReloadedSourceText.Contains(TEXT("AccumulatedRotationDegrees")));
	TestTrue(TEXT("Reload source declares the former field name alias"), ReloadedSourceText.Contains(TEXT("[AvidStateAlias(\"TotalRotationDegrees\")]")));
	if (!TestTrue(
			TEXT("Project C# source can be changed for automatic reload"),
			FFileHelper::SaveStringToFile(ReloadedSourceText, *WorkspaceResult.SourcePath)))
	{
		return false;
	}

	AActor* OtherActor = SpawnAvidScriptEditorModuleCSharpBindingActor(*World);
	if (!TestNotNull(TEXT("Selection drift Actor spawns"), OtherActor))
	{
		return false;
	}
	GEditor->SelectNone(false, true, false);
	GEditor->SelectActor(OtherActor, true, false, true, false);
	FakeWatchHost->Emit(WorkspaceResult.SourcePath);
	FakeWatchHost->Emit(WorkspaceResult.SourcePath);
	LiveReloadServicePtr->Tick();
	Now = 1.35;
	LiveReloadServicePtr->Tick();
	if (!TestTrue(TEXT("Real automatic reload reaches a terminal state"), PumpLiveReloadBuild()))
	{
		return false;
	}
	TestTrue(
		TEXT("Editor CoreTicker heartbeat advances during the real asynchronous build"),
		LastBuildHeartbeatCount > 0);
	TestEqual(TEXT("Reload keeps the initial bound Actor"), ReloadTarget, Actor);
	TestEqual(TEXT("Two quick changes trigger one real reload build"), ReloadBuildCount, 1);
	TestEqual(
		TEXT("Real automatic reload succeeds"),
		LiveReloadServicePtr->GetLastResult().Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildSucceeded);
	TestEqual(
		TEXT("Real automatic reload applies to the fixed component"),
		LiveReloadServicePtr->GetLastResult().BuildResult.BindingResult.Component,
		BindingResult.Component);
	TestTrue(
		TEXT("Real automatic reload keeps runtime loaded"),
		BindingResult.Component->GetRuntimeStats().bRuntimeLoaded);
	TestEqual(
		TEXT("Real automatic reload records one committed reload"),
		BindingResult.Component->GetRuntimeStats().SuccessfulReloadCount,
		1);
	const FAvidScriptWasmReloadResult& StateMigrationResult =
		LiveReloadServicePtr->GetLastResult().BuildResult.BindingResult.RuntimeResult;
	TestTrue(TEXT("Real automatic reload attempts guest state migration"), StateMigrationResult.bStateMigrationAttempted);
	TestTrue(TEXT("Real automatic reload applies guest state migration"), StateMigrationResult.bStateMigrationApplied);
	TestTrue(TEXT("Real automatic reload applies a renamed state alias"), StateMigrationResult.StateMigrationAliasedSlotCount >= 1);
	TestTrue(TEXT("Real automatic reload migrates at least the mutable gameplay field"),
		StateMigrationResult.StateMigrationMigratedSlotCount >= 1);
	TestTrue(TEXT("Real automatic reload migrates gameplay state bytes"),
		StateMigrationResult.StateMigrationMigratedByteCount >= 4);
	const FString ReloadedManifestPath = BindingResult.Component->GetRuntimeStats().ScriptManifestPath;
	const FString ReloadedWasmPath = FPaths::Combine(
		BuildResult.OutputRoot,
		BuildResult.ArtifactStem + TEXT(".wasm"));
	FString CommittedManifestText;
	TArray<uint8> CommittedWasmBytes;
	TestTrue(
		TEXT("Committed reload manifest bytes can be captured"),
		FFileHelper::LoadFileToString(
			CommittedManifestText,
			*ReloadedManifestPath));
	TestTrue(
		TEXT("Committed reload WASM bytes can be captured"),
		FFileHelper::LoadFileToArray(
			CommittedWasmBytes,
			*ReloadedWasmPath));
	Actor->SetActorRotation(FRotator::ZeroRotator);
	BindingResult.Component->TickComponent(0.5f, LEVELTICK_All, nullptr);
	TestTrue(
		TEXT("Reloaded C# module Tick uses the new rotation speed"),
		FMath::IsNearlyEqual(FMath::Abs(Actor->GetActorRotation().Yaw), 90.0, 0.01));
	TestTrue(
		TEXT("Reloaded C# module Tick continues pre-reload accumulated state"),
		FMath::IsNearlyEqual(Actor->GetActorScale3D().X, 1.135, 0.001));

	TestTrue(
		TEXT("Project C# source can be corrupted during automatic reload"),
		FFileHelper::SaveStringToFile(TEXT("public class {\n"), *WorkspaceResult.SourcePath));
	FakeWatchHost->Emit(WorkspaceResult.SourcePath);
	FakeWatchHost->Emit(WorkspaceResult.SourcePath);
	LiveReloadServicePtr->Tick();
	Now = LiveReloadServicePtr->GetStats().PendingDeadlineSeconds;
	LiveReloadServicePtr->Tick();
	if (!TestTrue(TEXT("Bad source reload reaches a terminal state"), PumpLiveReloadBuild()))
	{
		return false;
	}
	TestEqual(TEXT("Bad source triggers one trailing real build"), ReloadBuildCount, 2);
	TestEqual(
		TEXT("Bad source reports automatic build failure"),
		LiveReloadServicePtr->GetLastResult().Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed);
	TestTrue(TEXT("Bad source leaves live reload watching"), LiveReloadServicePtr->IsRunning());
	TestEqual(
		TEXT("Bad source preserves committed runtime manifest"),
		BindingResult.Component->GetRuntimeStats().ScriptManifestPath,
		ReloadedManifestPath);
	FString ManifestTextAfterBuildFailure;
	TArray<uint8> WasmBytesAfterBuildFailure;
	TestTrue(
		TEXT("Bad source preserves committed manifest artifact"),
		FFileHelper::LoadFileToString(
			ManifestTextAfterBuildFailure,
			*ReloadedManifestPath));
	TestEqual(
		TEXT("Bad source preserves committed manifest bytes"),
		ManifestTextAfterBuildFailure,
		CommittedManifestText);
	TestTrue(
		TEXT("Bad source preserves committed WASM artifact"),
		FFileHelper::LoadFileToArray(
			WasmBytesAfterBuildFailure,
			*ReloadedWasmPath));
	TestTrue(
		TEXT("Bad source preserves committed WASM bytes"),
		WasmBytesAfterBuildFailure == CommittedWasmBytes);
	Actor->SetActorRotation(FRotator::ZeroRotator);
	BindingResult.Component->TickComponent(0.5f, LEVELTICK_All, nullptr);
	TestTrue(
		TEXT("Previous C# runtime Tick continues after automatic build failure"),
		FMath::IsNearlyEqual(FMath::Abs(Actor->GetActorRotation().Yaw), 90.0, 0.01));
	TestTrue(
		TEXT("Project C# source can be restored after automatic build failure"),
		FFileHelper::SaveStringToFile(OriginalSourceText, *WorkspaceResult.SourcePath));

	bRejectNextReloadCandidate = true;
	FakeWatchHost->Emit(WorkspaceResult.SourcePath);
	LiveReloadServicePtr->Tick();
	Now = LiveReloadServicePtr->GetStats().PendingDeadlineSeconds;
	LiveReloadServicePtr->Tick();
	if (!TestTrue(
			TEXT("Rejected candidate reload reaches a terminal state"),
			PumpLiveReloadBuild()))
	{
		return false;
	}
	TestEqual(
		TEXT("Rejected candidate reports automatic binding failure"),
		LiveReloadServicePtr->GetLastResult().Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed);
	TestEqual(
		TEXT("Rejected candidate preserves binding failure cause"),
		LiveReloadServicePtr->GetLastResult().BuildResult.BindingResult.ErrorCategory,
		FString(TEXT("reload_rejected")));
	TestEqual(
		TEXT("Rejected candidate does not add a successful runtime reload"),
		BindingResult.Component->GetRuntimeStats().SuccessfulReloadCount,
		1);
	TestEqual(
		TEXT("Rejected candidate records one runtime rejection"),
		BindingResult.Component->GetRuntimeStats().RejectedReloadCount,
		1);
	Actor->SetActorRotation(FRotator::ZeroRotator);
	BindingResult.Component->TickComponent(0.5f, LEVELTICK_All, nullptr);
	TestTrue(
		TEXT("Previous C# runtime Tick continues after candidate binding rejection"),
		FMath::IsNearlyEqual(
			FMath::Abs(Actor->GetActorRotation().Yaw),
			90.0,
			0.01));

	FAvidScriptEditorCSharpLiveReloadServiceResult StopResult;
	TestTrue(TEXT("Project C# live reload stop is idempotent"), Module.ExecuteStopCSharpWorkspaceLiveReload(StopResult));
	TestTrue(TEXT("Project C# live reload repeated stop succeeds"), Module.ExecuteStopCSharpWorkspaceLiveReload(StopResult));
	TestFalse(TEXT("Project C# live reload stops"), Module.IsCSharpWorkspaceLiveReloadRunning());
	TestEqual(TEXT("Project C# live reload unregisters watcher"), FakeWatchHost->StopCount, 1);

	TestTrue(
		TEXT("Project C# source can be corrupted for a real build failure"),
		FFileHelper::SaveStringToFile(TEXT("public class {\n"), *WorkspaceResult.SourcePath));
	FAvidScriptEditorCSharpWorkspaceResult BuildFailureWorkspaceResult;
	FAvidScriptEditorCSharpBuildResult BuildFailureBuildResult;
	FAvidScriptEditorComponentBindingResult BuildFailureBindingResult;
	FAvidScriptEditorCSharpLiveReloadServiceResult BuildFailureLiveReloadResult;
	TestFalse(
		TEXT("Project C# live reload rejects a real initial build failure"),
		Module.ExecuteStartCSharpWorkspaceLiveReload(
			WorkspaceConfig,
			BuildFailureWorkspaceResult,
			BuildFailureBuildResult,
			BuildFailureBindingResult,
			BuildFailureLiveReloadResult));
	TestFalse(TEXT("Failed initial build returns no successful build"), BuildFailureBuildResult.bSucceeded);
	TestFalse(TEXT("Failed initial build binds no Actor"), BuildFailureBindingResult.bSucceeded);
	TestEqual(
		TEXT("Failed initial build has live reload stage category"),
		BuildFailureLiveReloadResult.ErrorCategory,
		FString(TEXT("live_reload_initial_build_failed")));
	TestEqual(TEXT("Failed initial build starts no watcher"), FakeWatchHost->StartCount, 1);

	TestTrue(
		TEXT("Project C# source can be restored for a binding failure"),
		FFileHelper::SaveStringToFile(OriginalSourceText, *WorkspaceResult.SourcePath));
	GEditor->SelectNone(false, true, false);
	FAvidScriptEditorCSharpWorkspaceResult BindingFailureWorkspaceResult;
	FAvidScriptEditorCSharpBuildResult BindingFailureBuildResult;
	FAvidScriptEditorComponentBindingResult BindingFailureBindingResult;
	FAvidScriptEditorCSharpLiveReloadServiceResult BindingFailureLiveReloadResult;
	TestFalse(
		TEXT("Project C# live reload rejects a real initial binding failure"),
		Module.ExecuteStartCSharpWorkspaceLiveReload(
			WorkspaceConfig,
			BindingFailureWorkspaceResult,
			BindingFailureBuildResult,
			BindingFailureBindingResult,
			BindingFailureLiveReloadResult));
	TestTrue(TEXT("Binding failure occurs after a successful build"), BindingFailureBuildResult.bSucceeded);
	TestFalse(TEXT("Binding failure returns no successful binding"), BindingFailureBindingResult.bSucceeded);
	TestEqual(
		TEXT("Failed initial binding has live reload stage category"),
		BindingFailureLiveReloadResult.ErrorCategory,
		FString(TEXT("live_reload_initial_binding_failed")));
	TestEqual(TEXT("Failed initial binding starts no watcher"), FakeWatchHost->StartCount, 1);

	FAvidScriptEditorCSharpWorkspaceConfig OutsideConfig;
	OutsideConfig.WorkspaceRoot = FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT(".."),
		TEXT("AvidScriptP44ModuleOutside"));
	FAvidScriptEditorCSharpWorkspaceResult OutsideWorkspaceResult;
	FAvidScriptEditorCSharpBuildResult OutsideBuildResult;
	FAvidScriptEditorComponentBindingResult OutsideBindingResult;
	FAvidScriptEditorCSharpLiveReloadServiceResult OutsideLiveReloadResult;
	TestFalse(
		TEXT("Project C# workspace command rejects outside workspace"),
		Module.ExecuteStartCSharpWorkspaceLiveReload(
			OutsideConfig,
			OutsideWorkspaceResult,
			OutsideBuildResult,
			OutsideBindingResult,
			OutsideLiveReloadResult));
	TestEqual(
		TEXT("Outside workspace category reaches command build result"),
		OutsideBuildResult.ErrorCategory,
		FString(TEXT("workspace_path_outside_project")));
	TestEqual(TEXT("Outside workspace invokes no build process"), OutsideBuildResult.BuildInvocationCount, 0);
	TestEqual(TEXT("Outside workspace invokes no Frontend"), OutsideBuildResult.FrontendInvocationCount, 0);
	TestFalse(TEXT("Outside workspace binds no Actor"), OutsideBindingResult.bSucceeded);
	TestEqual(TEXT("Failed initialization starts no new watcher"), FakeWatchHost->StartCount, 1);
	TestEqual(
		TEXT("Outside workspace reaches live reload cause"),
		OutsideLiveReloadResult.CauseErrorCategory,
		FString(TEXT("workspace_path_outside_project")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorModuleCSharpProfileTemplateTest,
	"AvidScript.Editor.Module.CSharpProfileTemplateSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorModuleCSharpProfileTemplateTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = GetAvidScriptEditorModuleCSharpProfileTestRoot();
	TestTrue(TEXT("Module C# profile template test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString TemplatePath = NormalizeAvidScriptEditorModuleCSharpProfilePath(FPaths::Combine(TestRoot, TEXT("module_default_template.csharp-profile.json")));
	IFileManager::Get().Delete(*TemplatePath, false, true, true);

	FAvidScriptEditorModule& Module =
		FModuleManager::LoadModuleChecked<FAvidScriptEditorModule>(TEXT("AvidScriptEditor"));

	FAvidScriptEditorCSharpProfileTemplateResult TemplateResult;
	TestTrue(TEXT("Module C# profile template command succeeds"), Module.ExecuteCreateCSharpProfileTemplate(TemplatePath, TemplateResult, false));
	TestTrue(TEXT("Module C# profile template result succeeds"), TemplateResult.bSucceeded);
	TestTrue(TEXT("Module C# profile template creates file"), TemplateResult.bCreated);
	TestEqual(TEXT("Module C# profile template path"), TemplateResult.NormalizedProfilePath, TemplatePath);

	FAvidScriptEditorCSharpProfileLoadResult LoadResult;
	TestTrue(TEXT("Module C# profile template loads"), FAvidScriptEditorCSharpProfileService::LoadProfile(TemplatePath, LoadResult));
	TestEqual(TEXT("Module C# profile template module id"), LoadResult.BuildConfig.ModuleId, FString(TEXT("csharp_profile_actor_lifecycle")));
	TestEqual(TEXT("Module C# profile template artifact stem"), LoadResult.BuildConfig.ArtifactStem, FString(TEXT("profile_actor_lifecycle")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorModuleCSharpProfileMissingNextActionTest,
	"AvidScript.Editor.Module.CSharpProfileMissingNextActionSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorModuleCSharpProfileMissingNextActionTest::RunTest(const FString& Parameters)
{
	const FString TestRoot = GetAvidScriptEditorModuleCSharpProfileTestRoot();
	TestTrue(TEXT("Module missing C# profile test root can be created"), IFileManager::Get().MakeDirectory(*TestRoot, true));

	const FString MissingProfilePath = NormalizeAvidScriptEditorModuleCSharpProfilePath(FPaths::Combine(TestRoot, TEXT("missing_profile.csharp-profile.json")));
	IFileManager::Get().Delete(*MissingProfilePath, false, true, true);

	FAvidScriptEditorModule& Module =
		FModuleManager::LoadModuleChecked<FAvidScriptEditorModule>(TEXT("AvidScriptEditor"));

	FAvidScriptEditorCSharpBuildResult BuildResult;
	FAvidScriptEditorComponentBindingResult BindingResult;
	TestFalse(TEXT("Module missing C# profile command fails"), Module.ExecuteCSharpProfileBuildAndBinding(MissingProfilePath, BuildResult, BindingResult));
	TestFalse(TEXT("Module missing C# profile build result fails"), BuildResult.bSucceeded);
	TestEqual(TEXT("Module missing C# profile error category"), BuildResult.ErrorCategory, FString(TEXT("profile_missing")));
	TestFalse(TEXT("Module missing C# profile next action is set"), BuildResult.NextAction.IsEmpty());
	TestTrue(TEXT("Module missing C# profile next action mentions profile"), BuildResult.NextAction.Contains(TEXT("profile")) || BuildResult.NextAction.Contains(TEXT("Create Default C# Profile")));
	TestFalse(TEXT("Module missing C# profile binding does not succeed"), BindingResult.bSucceeded);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
