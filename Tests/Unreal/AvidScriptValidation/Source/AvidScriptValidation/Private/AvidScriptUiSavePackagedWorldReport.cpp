#include "AvidScriptUiSavePackagedWorldProbe.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "Misc/DateTime.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace AvidScript::Validation
{
TSharedRef<FJsonObject> FUiSavePackagedWorldProbe::BuildFailureCycle() const
{
	const TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	const TCHAR* StageName = TEXT("await_world");
	switch (Stage)
	{
	case EStage::Actions: StageName = TEXT("actions"); break;
	case EStage::Dwell: StageName = TEXT("dwell"); break;
	case EStage::Travelling: StageName = TEXT("travelling"); break;
	case EStage::Complete: StageName = TEXT("complete"); break;
	default: break;
	}
	Result->SetNumberField(TEXT("cycle"), CurrentCycleNumber);
	Result->SetStringField(TEXT("stage"), StageName);
	Result->SetNumberField(TEXT("step_index"), StepIndex);
	Result->SetBoolField(TEXT("cleanup_observed"), bCleanupObserved);
	Result->SetStringField(TEXT("save_sha256"), SaveHash);
	Result->SetNumberField(TEXT("save_file_bytes"), static_cast<double>(SaveBytes));
	Result->SetArrayField(TEXT("actions"), CurrentActions);
	return Result;
}

bool FUiSavePackagedWorldProbe::WriteReport(
	bool bSucceeded,
	const FString& FailureCategory,
	FString& Error) const
{
	const double Elapsed = FMath::Max(0.0, Finished - Started);
	const bool bComplete = bSucceeded && FailureCategory.IsEmpty() && Stage == EStage::Complete;
	const bool bLongRun = bComplete && Elapsed >= 3600.0 && CompletedCycles >= 10;
	const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetNumberField(TEXT("schema_version"), 1);
	Report->SetStringField(TEXT("result"), bComplete
		? TEXT("avidscript_ui_save_world_probe_passed") : TEXT("avidscript_ui_save_world_probe_failed"));
	Report->SetBoolField(TEXT("succeeded"), bComplete);
	Report->SetStringField(TEXT("failure_category"), FailureCategory);
	Report->SetStringField(TEXT("mode"), TEXT("world"));
	Report->SetStringField(TEXT("process_mode"), IsCookedGameProcess() ? TEXT("packaged_game") : TEXT("unsupported_process"));
	Report->SetStringField(TEXT("configuration"), BuildConfiguration());
	Report->SetBoolField(TEXT("requires_cooked_data"), FPlatformProperties::RequiresCookedData());
	Report->SetStringField(TEXT("validation_plugin"), TEXT("AvidScriptValidation"));
	Report->SetStringField(TEXT("run_id"), RunId);
	Report->SetNumberField(TEXT("process_id"), FPlatformProcess::GetCurrentProcessId());
	Report->SetStringField(TEXT("input_kind"), TEXT("synthetic_ue_button_onclicked_broadcast"));
	Report->SetBoolField(TEXT("physical_click_verified"), false);
	Report->SetBoolField(TEXT("visual_verified"), false);
	Report->SetBoolField(TEXT("long_run_verified"), bLongRun);
	Report->SetStringField(TEXT("expected_module_id"), UiSaveModule);
	Report->SetStringField(TEXT("expected_package_id"), ExpectedPackage);
	Report->SetStringField(TEXT("map"), Observation.Map);
	Report->SetStringField(TEXT("user_dir"), Paths.UserDir);
	Report->SetStringField(TEXT("effective_saved_dir"), Paths.EffectiveSavedDir);
	Report->SetStringField(TEXT("save_path"), Paths.SavePath);
	Report->SetStringField(TEXT("initial_save_sha256"), Paths.InitialSaveHash);
	Report->SetStringField(TEXT("save_file_sha256"), SaveHash);
	Report->SetNumberField(TEXT("save_file_bytes"), static_cast<double>(SaveBytes));
	Report->SetBoolField(TEXT("save_file_exists"), Paths.bSaveExists);
	Report->SetStringField(TEXT("score_text"), Observation.Score);
	Report->SetStringField(TEXT("status_text"), Observation.Status);
	Report->SetStringField(TEXT("started_utc"), StartedUtc);
	Report->SetStringField(TEXT("finished_utc"), FDateTime::UtcNow().ToIso8601());
	Report->SetNumberField(TEXT("elapsed_seconds"), Elapsed);
	Report->SetNumberField(TEXT("timeout_seconds"), GetTimeoutSeconds());
	Report->SetStringField(TEXT("runtime_snapshot_phase"),
		Observation.bCaptured && Observation.Snapshot.bHasActiveRuntime ? TEXT("final_active") : TEXT("unavailable"));
	Report->SetObjectField(TEXT("runtime"), Observation.Runtime);
	Report->SetObjectField(TEXT("startup"), Observation.Startup);
	Report->SetObjectField(TEXT("backend"), Observation.Backend);
	Report->SetObjectField(TEXT("resources"), Observation.Resources);

	const TSharedRef<FJsonObject> Checks = MakeShared<FJsonObject>();
	Checks->SetNumberField(TEXT("cycles_passed"), CompletedCycles);
	Checks->SetNumberField(TEXT("actions_passed"), ActionChecksPassed);
	Checks->SetNumberField(TEXT("cleanup_cycles_passed"), CleanupChecksPassed);
	Checks->SetNumberField(TEXT("gc_cycles_passed"), GcChecksPassed);
	Checks->SetNumberField(TEXT("resource_snapshots_passed"), ResourceChecksPassed);

	const TSharedRef<FJsonObject> Memory = MakeShared<FJsonObject>();
	Memory->SetBoolField(TEXT("baseline_available"), bBaselineAvailable);
	Memory->SetNumberField(TEXT("baseline_cycle"), bBaselineAvailable ? 3 : 0);
	Memory->SetNumberField(TEXT("baseline_physical_bytes"), static_cast<double>(BaselinePhysical));
	Memory->SetNumberField(TEXT("baseline_virtual_bytes"), static_cast<double>(BaselineVirtual));
	Memory->SetNumberField(TEXT("final_physical_bytes"), static_cast<double>(FinalPhysical));
	Memory->SetNumberField(TEXT("final_virtual_bytes"), static_cast<double>(FinalVirtual));
	Memory->SetNumberField(TEXT("peak_physical_bytes"), static_cast<double>(PeakPhysical));
	Memory->SetNumberField(TEXT("peak_virtual_bytes"), static_cast<double>(PeakVirtual));

	const TSharedRef<FJsonObject> Observer = MakeShared<FJsonObject>();
	Observer->SetStringField(TEXT("retention_policy"), TEXT("fixed_milestones_plus_final"));
	Observer->SetNumberField(TEXT("sample_count"), Samples.Num());
	Observer->SetNumberField(TEXT("sample_capacity"), MaximumSamples);
	Observer->SetNumberField(TEXT("retained_completed_cycles"), 0);
	Observer->SetNumberField(TEXT("retained_actions"), CurrentActions.Num());
	Observer->SetNumberField(TEXT("json_estimated_bytes"), static_cast<double>(EstimateObserverBytes()));

	const TSharedRef<FJsonObject> Worlds = MakeShared<FJsonObject>();
	Worlds->SetNumberField(TEXT("requested_cycles"), RequestedCycles);
	Worlds->SetNumberField(TEXT("requested_soak_seconds"), SoakSeconds);
	Worlds->SetNumberField(TEXT("completed_cycles"), CompletedCycles);
	Worlds->SetNumberField(TEXT("activated_worlds"), ActivatedWorlds);
	Worlds->SetNumberField(TEXT("travel_count"), TravelCount);
	Worlds->SetNumberField(TEXT("cleanup_count"), CleanupCount);
	Worlds->SetNumberField(TEXT("elapsed_seconds"), Elapsed);
	Worlds->SetNumberField(TEXT("warmup_cycles"), 3);
	Worlds->SetBoolField(TEXT("world_lifecycle_verified"), bComplete);
	Worlds->SetBoolField(TEXT("long_run_verified"), bLongRun);
	Worlds->SetNumberField(TEXT("final_recovered_score"), bComplete ? CompletedCycles : -1);
	Worlds->SetStringField(TEXT("final_save_sha256"), SaveHash);
	Worlds->SetStringField(TEXT("evidence_chain_sha256"), EvidenceChain);
	Worlds->SetObjectField(TEXT("checks"), Checks);
	Worlds->SetObjectField(TEXT("memory_summary"), Memory);
	Worlds->SetObjectField(TEXT("observer"), Observer);
	Worlds->SetArrayField(TEXT("samples"), Samples);
	Worlds->SetArrayField(TEXT("final_actions"), bFinalRecovery ? CurrentActions : TArray<TSharedPtr<FJsonValue>>());
	if (!bComplete) { Worlds->SetObjectField(TEXT("failure_cycle"), BuildFailureCycle()); }
	else { Worlds->SetField(TEXT("failure_cycle"), MakeShared<FJsonValueNull>()); }
	Report->SetObjectField(TEXT("world_lifecycle"), Worlds);

	FString Json;
	if (!FJsonSerializer::Serialize(
		Report,
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json)))
	{
		Error = TEXT("report_serialization_failed"); return false;
	}
	return Paths.WriteNewReport(Json, Error);
}
}
