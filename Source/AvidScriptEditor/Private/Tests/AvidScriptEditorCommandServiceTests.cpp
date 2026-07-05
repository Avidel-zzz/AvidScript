#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptEditorCommandService.h"

#include "AvidScriptObjectRegistry.h"

#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
const uint8 GAvidScriptEditorCommandServiceCompatibleWasmModule[] = {
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

FAvidScriptEditorCompileResult MakeAvidScriptEditorCommandServiceReloadableCompileResult(
	const FString& ModuleId)
{
	FAvidScriptEditorCompileResult CompileResult;
	CompileResult.bSucceeded = true;
	CompileResult.bReloadable = true;
	CompileResult.Status = EAvidScriptEditorCompileStatus::SucceededReloadable;
	CompileResult.Manifest = FAvidScriptWasmReloadManifest::MakeSmoke(ModuleId);
	CompileResult.Bytecode.Append(
		GAvidScriptEditorCommandServiceCompatibleWasmModule,
		UE_ARRAY_COUNT(GAvidScriptEditorCommandServiceCompatibleWasmModule));
	return CompileResult;
}

FAvidScriptEditorCompileResult MakeAvidScriptEditorCommandServiceGeneratedOnlyCompileResult()
{
	FAvidScriptEditorCompileResult CompileResult;
	CompileResult.bSucceeded = true;
	CompileResult.bReloadable = false;
	CompileResult.Status = EAvidScriptEditorCompileStatus::SucceededGeneratedOnly;
	return CompileResult;
}

FAvidScriptEditorCompileResult MakeAvidScriptEditorCommandServiceFailedCompileResult()
{
	FAvidScriptEditorCompileResult CompileResult;
	CompileResult.bSucceeded = false;
	CompileResult.bReloadable = false;
	CompileResult.Status = EAvidScriptEditorCompileStatus::FailedDiagnostics;
	CompileResult.ErrorCategory = TEXT("frontend_diagnostics");
	CompileResult.ErrorMessage = TEXT("Unknown binding actor.teleport");
	CompileResult.NextAction = TEXT("fix the frontend diagnostic before reloading");
	return CompileResult;
}

FString GetAvidScriptEditorCommandServiceTestRoot()
{
	FString TestRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("AvidScriptEditorTests"), TEXT("CommandService"));
	TestRoot = FPaths::ConvertRelativePathToFull(TestRoot);
	FPaths::NormalizeFilename(TestRoot);
	return TestRoot;
}

FString GetAvidScriptEditorCommandServiceReportPath(const TCHAR* FileName)
{
	return FPaths::Combine(GetAvidScriptEditorCommandServiceTestRoot(), FileName);
}

FString GetAvidScriptEditorCommandServiceSampleSourcePath()
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

FString GetAvidScriptEditorCommandServiceExistingManifestPath()
{
	FString ManifestPath = FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScriptGenerated"),
		TEXT("actor_set_location"),
		TEXT("actor_set_location.avidscript.json"));
	ManifestPath = FPaths::ConvertRelativePathToFull(ManifestPath);
	FPaths::NormalizeFilename(ManifestPath);
	return ManifestPath;
}

FAvidScriptFrontendInvocationResult MakeAvidScriptEditorCommandServiceBuiltInvocation(
	const FString& ManifestPath)
{
	FAvidScriptFrontendInvocationResult InvocationResult;
	InvocationResult.bSucceeded = true;
	InvocationResult.ProcessExitCode = 0;
	InvocationResult.ReportLoadResult.bSucceeded = true;
	InvocationResult.Report.SchemaVersion = 1;
	InvocationResult.Report.ExitCode = 0;
	InvocationResult.Report.bSucceeded = true;

	FAvidScriptFrontendBuildEvent BuildEvent;
	BuildEvent.Result = TEXT("built");
	BuildEvent.Fields.Add(TEXT("manifest"), ManifestPath);
	InvocationResult.Report.BuildEvents.Add(MoveTemp(BuildEvent));
	return InvocationResult;
}

bool CreateAvidScriptEditorCommandServiceWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptEditorCommandServiceWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyAvidScriptEditorCommandServiceWorld(UWorld*& World)
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

AActor* SpawnAvidScriptEditorCommandServiceActor(UWorld& World)
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
	FAvidScriptEditorCommandServiceApplyEvaluatedReloadableTest,
	"AvidScript.Editor.CommandService.ApplyEvaluatedReloadableSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceApplyEvaluatedReloadableTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorCommandResult CommandResult;

	TestTrue(
		TEXT("Reloadable compile result applies through command facade"),
		FAvidScriptEditorCommandService::ApplyEvaluatedCompileResult(
			MakeAvidScriptEditorCommandServiceReloadableCompileResult(TEXT("command_reloadable")),
			Session,
			CommandResult));

	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::ReloadApplied);
	TestTrue(TEXT("Command marks reload applied"), CommandResult.bReloadApplied);
	TestTrue(TEXT("Session has live module"), Session.IsLiveLoaded());
	TestEqual(TEXT("Live module id"), Session.GetLiveModuleId(), FString(TEXT("command_reloadable")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandServiceApplyGeneratedOnlyTest,
	"AvidScript.Editor.CommandService.ApplyGeneratedOnlySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceApplyGeneratedOnlyTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorCommandResult CommandResult;

	TestTrue(
		TEXT("Generated-only compile result succeeds without reload"),
		FAvidScriptEditorCommandService::ApplyEvaluatedCompileResult(
			MakeAvidScriptEditorCommandServiceGeneratedOnlyCompileResult(),
			Session,
			CommandResult));

	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::GeneratedOnly);
	TestFalse(TEXT("Generated-only does not apply reload"), CommandResult.bReloadApplied);
	TestFalse(TEXT("Generated-only keeps session unloaded"), Session.IsLiveLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandServiceApplyFailedCompileTest,
	"AvidScript.Editor.CommandService.ApplyFailedCompileSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceApplyFailedCompileTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorCommandResult CommandResult;

	TestFalse(
		TEXT("Failed compile result fails command facade"),
		FAvidScriptEditorCommandService::ApplyEvaluatedCompileResult(
			MakeAvidScriptEditorCommandServiceFailedCompileResult(),
			Session,
			CommandResult));

	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::CompileFailed);
	TestEqual(TEXT("Command category"), CommandResult.ErrorCategory, FString(TEXT("frontend_diagnostics")));
	TestFalse(TEXT("Failed compile keeps session unloaded"), Session.IsLiveLoaded());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandServiceMissingReloadSessionTest,
	"AvidScript.Editor.CommandService.MissingReloadSessionSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceMissingReloadSessionTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCommandConfig Config;
	FAvidScriptEditorCommandResult CommandResult;

	TestFalse(
		TEXT("Missing reload session fails command facade"),
		FAvidScriptEditorCommandService::CompileAndApply(Config, CommandResult));

	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::CompileFailed);
	TestEqual(TEXT("Command category"), CommandResult.ErrorCategory, FString(TEXT("reload_session_missing")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandServiceCompileAndApplyGeneratedOnlyTest,
	"AvidScript.Editor.CommandService.CompileAndApplyGeneratedOnlySmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceCompileAndApplyGeneratedOnlyTest::RunTest(const FString& Parameters)
{
	FAvidScriptWasmReloadSession Session;
	FAvidScriptEditorCommandConfig Config;
	Config.ReloadSession = &Session;
	Config.CompileConfig.InvocationConfig.SourcePath = GetAvidScriptEditorCommandServiceSampleSourcePath();
	Config.CompileConfig.InvocationConfig.ReportPath = GetAvidScriptEditorCommandServiceReportPath(TEXT("compile_and_apply_generated.report.json"));
	Config.CompileConfig.InvocationConfig.bSkipCompile = true;

	FAvidScriptEditorCommandResult CommandResult;
	TestTrue(
		TEXT("CompileAndApply succeeds for generated-only frontend report"),
		FAvidScriptEditorCommandService::CompileAndApply(Config, CommandResult));

	TestTrue(TEXT("Command result succeeded"), CommandResult.bSucceeded);
	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::GeneratedOnly);
	TestEqual(
		TEXT("Compile status"),
		CommandResult.CompileResult.Status,
		EAvidScriptEditorCompileStatus::SucceededGeneratedOnly);
	TestFalse(TEXT("Generated-only command does not apply reload"), CommandResult.bReloadApplied);
	TestFalse(TEXT("Generated-only command keeps session unloaded"), Session.IsLiveLoaded());
	TestTrue(TEXT("Frontend report was loaded"), CommandResult.CompileResult.InvocationResult.ReportLoadResult.bSucceeded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCommandServiceExistingBuiltManifestCommandTest,
	"AvidScript.Editor.CommandService.ExistingBuiltManifestCommandSmoke",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCommandServiceExistingBuiltManifestCommandTest::RunTest(const FString& Parameters)
{
	const FString ManifestPath = GetAvidScriptEditorCommandServiceExistingManifestPath();
	if (!FPaths::FileExists(ManifestPath))
	{
		AddWarning(FString::Printf(
			TEXT("Built AvidScript manifest is missing; run InvokeAvidScriptFrontend.ps1 with LDC before relying on this command smoke. missing=%s"),
			*ManifestPath));
		return true;
	}

	FAvidScriptEditorCompileResult CompileResult;
	TestTrue(
		TEXT("Existing built manifest evaluates through compile gate"),
		FAvidScriptEditorCompileService::EvaluateInvocationResult(
			MakeAvidScriptEditorCommandServiceBuiltInvocation(ManifestPath),
			CompileResult));
	TestEqual(
		TEXT("Built manifest compile status"),
		CompileResult.Status,
		EAvidScriptEditorCompileStatus::SucceededReloadable);
	TestTrue(TEXT("Built manifest bytecode is loaded"), CompileResult.Bytecode.Num() > 0);

	UWorld* World = nullptr;
	if (!CreateAvidScriptEditorCommandServiceWorld(World))
	{
		AddError(TEXT("Failed to create AvidScript command service test world."));
		DestroyAvidScriptEditorCommandServiceWorld(World);
		return true;
	}

	AActor* Actor = SpawnAvidScriptEditorCommandServiceActor(*World);
	TestNotNull(TEXT("Test actor spawns"), Actor);
	if (Actor == nullptr)
	{
		DestroyAvidScriptEditorCommandServiceWorld(World);
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

	FAvidScriptWasmReloadSession Session;
	Session.SetHostContext(HostContext);

	FAvidScriptEditorCommandResult CommandResult;
	const bool bCommandApplied = FAvidScriptEditorCommandService::ApplyEvaluatedCompileResult(
		CompileResult,
		Session,
		CommandResult);
	if (!bCommandApplied)
	{
		AddError(CommandResult.ErrorMessage);
		DestroyAvidScriptEditorCommandServiceWorld(World);
		return true;
	}

	TestTrue(TEXT("Command applies built manifest"), bCommandApplied);
	TestEqual(TEXT("Command status"), CommandResult.Status, EAvidScriptEditorCommandStatus::ReloadApplied);
	TestTrue(TEXT("Command marks reload applied"), CommandResult.bReloadApplied);
	TestTrue(TEXT("Session has live module"), Session.IsLiveLoaded());
	TestEqual(TEXT("Live module id"), Session.GetLiveModuleId(), FString(TEXT("actor_set_location")));
	TestEqual(TEXT("Actor moved by command-applied artifact"), Actor->GetActorLocation(), FVector(123.0, 456.0, 789.0));

	DestroyAvidScriptEditorCommandServiceWorld(World);
	return true;
}
#endif // WITH_DEV_AUTOMATION_TESTS
