#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCommandLauncher.h"

#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
FString GetAvidScriptEditorCommandLauncherTestRoot()
{
	FString TestRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptEditorTests"), TEXT("CommandLauncher"));
	TestRoot = FPaths::ConvertRelativePathToFull(TestRoot);
	FPaths::NormalizeFilename(TestRoot);
	return TestRoot;
}

FString GetAvidScriptEditorCommandLauncherReportPath(const TCHAR* FileName)
{
	return FPaths::Combine(GetAvidScriptEditorCommandLauncherTestRoot(), FileName);
}

FString GetAvidScriptEditorCommandLauncherSampleSourcePath()
{
	FString SourcePath = FPaths::Combine(
		FPaths::ProjectDir(),
		TEXT("Plugins"),
		TEXT("AvidScript"),
		TEXT("Samples"),
		TEXT("AvidScript"),
		TEXT("ActorSetLocation"),
		TEXT("actor_set_location.avid"));
	SourcePath = FPaths::ConvertRelativePathToFull(SourcePath);
	FPaths::NormalizeFilename(SourcePath);
	return SourcePath;
}

FString GetAvidScriptEditorCommandLauncherMissingSourcePath()
{
	FString SourcePath = FPaths::Combine(
		GetAvidScriptEditorCommandLauncherTestRoot(),
		TEXT("missing_source.avid"));
	SourcePath = FPaths::ConvertRelativePathToFull(SourcePath);
	FPaths::NormalizeFilename(SourcePath);
	return SourcePath;
}

FString GetAvidScriptEditorCommandLauncherToolchainRoot()
{
	FString ToolchainRoot = TEXT("C:/tmp/AvidScriptToolchains/ldc2-1.42.0-windows-x64/ldc2-1.42.0-windows-x64");
	FPaths::NormalizeFilename(ToolchainRoot);
	return ToolchainRoot;
}

bool CreateAvidScriptEditorCommandLauncherWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptEditorCommandLauncherWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyAvidScriptEditorCommandLauncherWorld(UWorld*& World)
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

AActor* SpawnAvidScriptEditorCommandLauncherActor(UWorld& World)
{
	AActor* Actor = World.SpawnActor<AActor>();
	if (Actor == nullptr)
	{
		return nullptr;
	}

	USceneComponent* RootComponent = NewObject<USceneComponent>(Actor, TEXT("Root"));
	if (RootComponent == nullptr)
	{
		return Actor;
	}

	Actor->SetRootComponent(RootComponent);
	RootComponent->RegisterComponent();
	return Actor;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandLauncherDefaultConfigTest,
	"AvidScript.Editor.CommandLauncher.DefaultConfigForSourceSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandLauncherDefaultConfigTest::RunTest(const FString& Parameters)
{
	const FString SourcePath = GetAvidScriptEditorCommandLauncherSampleSourcePath();
	FAvidScriptEditorCommandLaunchConfig Config;
	FString ErrorMessage;

	TestTrue(
		TEXT("Default config builds for sample source"),
		FAvidScriptEditorCommandLauncher::MakeDefaultConfigForSource(SourcePath, Config, ErrorMessage));
	TestEqual(TEXT("Source path is normalized"), Config.SourcePath, SourcePath);
	TestTrue(TEXT("Bindings default points to ActorHostBindings"), Config.BindingsPath.EndsWith(TEXT("Bindings/ActorHostBindings.avidscript.json")));
	TestTrue(TEXT("Output root uses source name"), Config.OutputRoot.EndsWith(TEXT("Saved/AvidScriptGenerated/actor_set_location")));
	TestTrue(TEXT("Report path uses source name"), Config.ReportPath.EndsWith(TEXT("Saved/AvidScriptReports/actor_set_location.frontend.report.json")));
	TestFalse(TEXT("Default config compiles by default"), Config.bSkipCompile);
	TestTrue(TEXT("No error message on success"), ErrorMessage.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandLauncherMissingSourceTest,
	"AvidScript.Editor.CommandLauncher.MissingSourceFailsSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandLauncherMissingSourceTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandLaunchConfig Config;
	Config.SourcePath = GetAvidScriptEditorCommandLauncherMissingSourcePath();

	FAvidScriptEditorCommandLauncher Launcher;
	FAvidScriptEditorCommandLaunchResult Result;
	TestFalse(TEXT("Missing source fails launch"), Launcher.CompileSourceAndApply(Config, Result));
	TestFalse(TEXT("Missing source launch result does not succeed"), Result.bSucceeded);
	TestTrue(TEXT("Summary explains source missing"), Result.Summary.Contains(TEXT("source_missing")));
	TestFalse(TEXT("Missing source keeps session unloaded"), Launcher.GetReloadSession().IsLiveLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandLauncherGeneratedOnlyTest,
	"AvidScript.Editor.CommandLauncher.GeneratedOnlySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandLauncherGeneratedOnlyTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandLaunchConfig Config;
	FString ErrorMessage;
	TestTrue(
		TEXT("Default config builds for generated-only smoke"),
		FAvidScriptEditorCommandLauncher::MakeDefaultConfigForSource(
			GetAvidScriptEditorCommandLauncherSampleSourcePath(),
			Config,
			ErrorMessage));
	Config.ReportPath = GetAvidScriptEditorCommandLauncherReportPath(TEXT("generated_only.frontend.report.json"));
	Config.bSkipCompile = true;

	FAvidScriptEditorCommandLauncher Launcher;
	FAvidScriptEditorCommandLaunchResult Result;
	TestTrue(TEXT("Generated-only launch succeeds"), Launcher.CompileSourceAndApply(Config, Result));
	TestTrue(TEXT("Generated-only result succeeds"), Result.bSucceeded);
	TestFalse(TEXT("Generated-only does not reload"), Result.bReloadApplied);
	TestEqual(TEXT("Command status"), Result.CommandResult.Status, EAvidScriptEditorCommandStatus::GeneratedOnly);
	TestTrue(TEXT("Summary mentions generated-only"), Result.Summary.Contains(TEXT("generated")));
	TestFalse(TEXT("Generated-only keeps session unloaded"), Launcher.GetReloadSession().IsLiveLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandLauncherBuiltManifestReloadTest,
	"AvidScript.Editor.CommandLauncher.BuiltManifestReloadSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandLauncherBuiltManifestReloadTest::RunTest(const FString& Parameters)
{
	const FString ToolchainRoot = GetAvidScriptEditorCommandLauncherToolchainRoot();
	const FString Ldc2Path = FPaths::Combine(ToolchainRoot, TEXT("bin"), TEXT("ldc2.exe"));
	if (!FPaths::FileExists(Ldc2Path))
	{
		AddWarning(FString::Printf(
			TEXT("Portable LDC is missing; skipping launcher built-manifest reload smoke. missing=%s"),
			*Ldc2Path));
		return true;
	}

	UWorld* World = nullptr;
	if (!CreateAvidScriptEditorCommandLauncherWorld(World))
	{
		AddError(TEXT("Failed to create launcher test world."));
		DestroyAvidScriptEditorCommandLauncherWorld(World);
		return true;
	}

	AActor* Actor = SpawnAvidScriptEditorCommandLauncherActor(*World);
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyAvidScriptEditorCommandLauncherWorld(World);
		return true;
	}

	Actor->SetActorLocation(FVector(10.0, 20.0, 30.0));

	FAvidScriptObjectRegistry Registry;
	FAvidScriptObjectHandleResult RegisterResult;
	const FAvidScriptObjectHandle ActorHandle = Registry.RegisterObject(Actor, RegisterResult);
	TestTrue(TEXT("Actor registers"), RegisterResult.bSucceeded);
	TestEqual(TEXT("Sample slot matches first registry handle"), ActorHandle.Slot, static_cast<uint32>(1));
	TestEqual(TEXT("Sample generation matches first registry handle"), ActorHandle.Generation, static_cast<uint32>(1));

	FAvidScriptWasmHostContext HostContext;
	HostContext.ObjectRegistry = &Registry;
	HostContext.ActorWritePolicy = EAvidScriptActorWritePolicy::AllowWrites;

	FAvidScriptEditorCommandLaunchConfig Config;
	FString ErrorMessage;
	TestTrue(
		TEXT("Default config builds for built reload smoke"),
		FAvidScriptEditorCommandLauncher::MakeDefaultConfigForSource(
			GetAvidScriptEditorCommandLauncherSampleSourcePath(),
			Config,
			ErrorMessage));
	Config.ToolchainRoot = ToolchainRoot;
	Config.ReportPath = GetAvidScriptEditorCommandLauncherReportPath(TEXT("built_manifest.frontend.report.json"));

	FAvidScriptEditorCommandLauncher Launcher;
	Launcher.SetHostContext(HostContext);

	FAvidScriptEditorCommandLaunchResult Result;
	const bool bLaunched = Launcher.CompileSourceAndApply(Config, Result);
	if (!bLaunched)
	{
		AddError(Result.Summary);
		DestroyAvidScriptEditorCommandLauncherWorld(World);
		return true;
	}

	TestTrue(TEXT("Built manifest launch succeeds"), bLaunched);
	TestTrue(TEXT("Built manifest result succeeds"), Result.bSucceeded);
	TestTrue(TEXT("Built manifest reload applied"), Result.bReloadApplied);
	TestEqual(TEXT("Command status"), Result.CommandResult.Status, EAvidScriptEditorCommandStatus::ReloadApplied);
	TestTrue(TEXT("Launcher session has live module"), Launcher.GetReloadSession().IsLiveLoaded());
	TestEqual(TEXT("Live module id"), Launcher.GetReloadSession().GetLiveModuleId(), FString(TEXT("actor_set_location")));
	TestEqual(TEXT("Actor moved by launcher-applied artifact"), Actor->GetActorLocation(), FVector(123.0, 456.0, 789.0));

	DestroyAvidScriptEditorCommandLauncherWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
