#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorModule.h"

#include "AvidScriptComponent.h"
#include "AvidScriptEditorCommandLauncher.h"
#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorMenuRegistrar.h"

#include "Components/SceneComponent.h"
#include "Editor.h"
#include "HAL/FileManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

namespace
{
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
