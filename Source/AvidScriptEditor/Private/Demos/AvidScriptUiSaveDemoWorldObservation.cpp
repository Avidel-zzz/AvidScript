#include "Demos/AvidScriptUiSaveDemoWorld.h"
#include "Demos/AvidScriptUiSaveDemoObservation.h"

#include "AvidScriptComponent.h"
#include "AvidScriptWorldSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"

namespace AvidScript::UiSaveDemo
{
bool FUiSaveWorld::CaptureResources(FString& Error)
{
	const FAvidScriptRuntimeSession* Session = Component.IsValid() ? Component->GetRuntimeSessionForEditorDebugging() : nullptr;
	if (!Session || !Widget.IsValid()) { Error = TEXT("world_resource_owner_missing"); return false; }
	const auto Snapshot = Session->GetSnapshot();
	int32 BoundButtons = 0;
	for (const TCHAR* Name : { TEXT("CollectButton"), TEXT("SaveButton"), TEXT("LoadButton"), TEXT("ResetButton") })
	{
		const UButton* Button = ReadUiObject<UButton>(Widget.Get(), Name);
		if (Button && Button->OnClicked.IsBound()) { ++BoundButtons; }
	}
	const TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();
	const int32 Counts[] = { Snapshot.ActiveDelegateSubscriptionCount, BoundButtons, Snapshot.OwnedObjectEntryCount,
		Snapshot.BorrowedHandleEntryCount, Snapshot.PendingTimerCount, Snapshot.PendingContinuationCount,
		Snapshot.PreparedContinuationCount, Snapshot.PreparedDelegateSubscriptionCount };
	const TCHAR* Names[] = { TEXT("active_subscriptions"), TEXT("bound_buttons"), TEXT("owned_entries"), TEXT("borrowed_entries"),
		TEXT("pending_timers"), TEXT("pending_continuations"), TEXT("prepared_continuations"), TEXT("prepared_subscriptions") };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Counts); ++Index) { Resources->SetNumberField(Names[Index], Counts[Index]); }
	if (!bFinalRecovery) { CurrentCycle->SetObjectField(TEXT("resources"), Resources); }
	if (Counts[0] != 4 || Counts[1] != 4 || Counts[2] != 0 || Counts[3] < 7 || Counts[3] > 8
		|| Counts[4] != 0 || Counts[5] != 0 || Counts[6] != 0 || Counts[7] != 0
		|| Snapshot.bFaultQuarantined || !Snapshot.bHasActiveRuntime)
	{
		Error = TEXT("world_resource_count_invalid"); return false;
	}
	FAvidScriptWasmSmokeResult Live;
	if (!Session->CaptureLiveSnapshot(Live)) { Error = TEXT("world_backend_snapshot_unavailable"); return false; }
	const auto& Info = Live.BackendInfo;
	const TSharedRef<FJsonObject> Backend = MakeShared<FJsonObject>();
	Backend->SetBoolField(TEXT("measured"), true);
	Backend->SetStringField(TEXT("source"), TEXT("CaptureLiveSnapshot.BackendInfo"));
	Backend->SetStringField(TEXT("backend_id"), Info.StableBackendId);
	Backend->SetStringField(TEXT("backend_kind"), Info.Kind == EAvidScriptVmBackendKind::Wasmtime ? TEXT("wasmtime") : TEXT("wamr"));
	const TCHAR* Mode = TEXT("auto");
	switch (Info.ExecutionMode)
	{
	case EAvidScriptVmExecutionMode::Interpreter: Mode = TEXT("interpreter"); break;
	case EAvidScriptVmExecutionMode::Aot: Mode = TEXT("aot"); break;
	case EAvidScriptVmExecutionMode::Jit: Mode = TEXT("jit"); break;
	default: break;
	}
	const TCHAR* Format = TEXT("wasm_bytecode");
	switch (Info.ArtifactFormat)
	{
	case EAvidScriptVmArtifactFormat::WamrAot: Format = TEXT("wamr_aot"); break;
	case EAvidScriptVmArtifactFormat::WasmtimeSerialized: Format = TEXT("wasmtime_serialized"); break;
	default: break;
	}
	Backend->SetStringField(TEXT("execution_mode"), Mode);
	Backend->SetStringField(TEXT("artifact_format"), Format);
	Backend->SetStringField(TEXT("runtime_version"), Info.RuntimeVersion);
	Backend->SetStringField(TEXT("runtime_build_identity"), Info.RuntimeBuildIdentity);
	Backend->SetStringField(TEXT("runtime_artifact_sha256"), Info.RuntimeArtifactSha256);
	Backend->SetStringField(TEXT("target_triple"), Info.TargetTriple);
	Resources->SetObjectField(TEXT("backend"), Backend);
	return true;
}

void FUiSaveWorld::CaptureRuntime(FJsonObject& Runtime, FJsonObject& Startup) const
{
	if (Component.IsValid())
	{
		const auto& Stats = Component->GetRuntimeStats();
		Runtime.SetStringField(TEXT("module_id"), Stats.ModuleId);
		Runtime.SetStringField(TEXT("package_id"), Stats.PackageId);
		Runtime.SetStringField(TEXT("script_manifest_path"), Stats.ScriptManifestPath);
		Runtime.SetBoolField(TEXT("resolved_from_package"), Stats.bResolvedFromPackage);
		Runtime.SetBoolField(TEXT("runtime_loaded"), Stats.bRuntimeLoaded);
		Runtime.SetBoolField(TEXT("begin_play"), Stats.bBeginPlayCalled);
		Runtime.SetBoolField(TEXT("owner_registered"), Stats.bOwnerRegistered);
		Runtime.SetBoolField(TEXT("owner_handle_valid"), Stats.OwnerHandle.IsValid());
		Runtime.SetStringField(TEXT("owner"), Host.IsValid() ? Host->GetPathName() : FString());
		Runtime.SetStringField(TEXT("error_message"), Stats.LastErrorMessage);
		Runtime.SetNumberField(TEXT("ticks"), Stats.TickCallCount);
		Runtime.SetNumberField(TEXT("events"), Stats.EventCallbackCount);
		Runtime.SetNumberField(TEXT("dropped_events"), Stats.DroppedGameplayEventCount);
	}
	if (World.IsValid())
	{
		if (const auto* Subsystem = World->GetSubsystem<UAvidScriptWorldSubsystem>())
		{
			const auto& Stats = Subsystem->GetRuntimeStats();
			Startup.SetBoolField(TEXT("active"), Stats.bStartupScenarioActive);
			Startup.SetStringField(TEXT("scenario_id"), Stats.StartupScenarioId);
			Startup.SetStringField(TEXT("error_category"), Stats.LastErrorCategory);
			Startup.SetStringField(TEXT("error_message"), Stats.LastErrorMessage);
		}
	}
}

void FUiSaveWorld::AppendReport(FJsonObject& Report) const
{
	const double Elapsed = (Finished > 0.0 ? Finished : FPlatformTime::Seconds()) - Started;
	const bool bSucceeded = Stage == EStage::Complete && Failure.IsEmpty();
	const bool bLongRun = bSucceeded && Elapsed >= 3600.0 && CompletedCycles >= 10;
	const TSharedRef<FJsonObject> Worlds = MakeShared<FJsonObject>();
	Worlds->SetNumberField(TEXT("requested_cycles"), RequestedCycles);
	Worlds->SetNumberField(TEXT("requested_soak_seconds"), SoakSeconds);
	Worlds->SetNumberField(TEXT("completed_cycles"), CompletedCycles);
	Worlds->SetNumberField(TEXT("activated_worlds"), ActivatedWorlds);
	Worlds->SetNumberField(TEXT("travel_count"), TravelCount);
	Worlds->SetNumberField(TEXT("cleanup_count"), CleanupCount);
	Worlds->SetNumberField(TEXT("elapsed_seconds"), Elapsed);
	Worlds->SetNumberField(TEXT("warmup_cycles"), 3);
	Worlds->SetBoolField(TEXT("world_lifecycle_verified"), bSucceeded);
	Worlds->SetBoolField(TEXT("long_run_verified"), bLongRun);
	Worlds->SetBoolField(TEXT("memory_measured"), CompletedCycles > 0);
	Worlds->SetNumberField(TEXT("final_recovered_score"), bSucceeded ? CompletedCycles : -1);
	Worlds->SetStringField(TEXT("final_save_sha256"), SaveHash);
	Worlds->SetArrayField(TEXT("cycles"), Cycles);
	Worlds->SetArrayField(TEXT("final_actions"), bFinalRecovery ? Actions : TArray<TSharedPtr<FJsonValue>>());
	Worlds->SetObjectField(TEXT("memory_summary"), MemorySummary);
	Report.SetObjectField(TEXT("world_lifecycle"), Worlds);
	Report.SetBoolField(TEXT("long_run_verified"), bLongRun);
	Report.SetBoolField(TEXT("gc_performed"), CompletedCycles > 0);
	Report.SetStringField(TEXT("map"), TEXT("/AvidScript/Demos/UiSave/L_UiSave"));
	Report.SetStringField(TEXT("save_file_sha256"), SaveHash);
	Report.SetNumberField(TEXT("save_file_bytes"), static_cast<double>(SaveBytes));
	Report.SetBoolField(TEXT("save_file_exists"), IFileManager::Get().FileExists(*SavePath));
	FString Score, Status;
	ReadText(Score, Status);
	Report.SetStringField(TEXT("score_text"), Score);
	Report.SetStringField(TEXT("status_text"), Status);
	const TSharedRef<FJsonObject> Runtime = MakeShared<FJsonObject>();
	const TSharedRef<FJsonObject> Startup = MakeShared<FJsonObject>();
	CaptureRuntime(*Runtime, *Startup);
	Report.SetObjectField(TEXT("runtime"), Runtime);
	Report.SetObjectField(TEXT("startup"), Startup);
}
}
