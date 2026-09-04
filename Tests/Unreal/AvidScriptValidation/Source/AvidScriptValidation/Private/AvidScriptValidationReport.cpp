#include "AvidScriptUiSavePackagedProbe.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"

namespace AvidScript::Validation
{
bool FUiSavePackagedProbe::WriteReport(bool bSucceeded, const FString& Failure, FString& Error) const
{
	const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	Report->SetNumberField(TEXT("schema_version"), 1);
	Report->SetStringField(TEXT("result"), bSucceeded ? TEXT("avidscript_ui_save_probe_passed") : TEXT("avidscript_ui_save_probe_failed"));
	Report->SetBoolField(TEXT("succeeded"), bSucceeded);
	Report->SetStringField(TEXT("failure_category"), Failure);
	Report->SetStringField(TEXT("mode"), Mode);
	Report->SetStringField(TEXT("process_mode"), IsCookedGameProcess() ? TEXT("packaged_game") : TEXT("unsupported_process"));
	Report->SetStringField(TEXT("configuration"), BuildConfiguration());
	Report->SetBoolField(TEXT("requires_cooked_data"), FPlatformProperties::RequiresCookedData());
	Report->SetStringField(TEXT("validation_plugin"), TEXT("AvidScriptValidation"));
	Report->SetStringField(TEXT("run_id"), RunId);
	Report->SetNumberField(TEXT("process_id"), FPlatformProcess::GetCurrentProcessId());
	Report->SetStringField(TEXT("input_kind"), TEXT("synthetic_ue_button_onclicked_broadcast"));
	Report->SetBoolField(TEXT("physical_click_verified"), false);
	Report->SetBoolField(TEXT("visual_verified"), false);
	Report->SetBoolField(TEXT("long_run_verified"), false);
	Report->SetStringField(TEXT("expected_module_id"), UiSaveModule);
	Report->SetStringField(TEXT("expected_package_id"), ExpectedPackage);
	Report->SetStringField(TEXT("map"), Observation.Map);
	Report->SetStringField(TEXT("user_dir"), Paths.UserDir);
	Report->SetStringField(TEXT("effective_saved_dir"), Paths.EffectiveSavedDir);
	Report->SetStringField(TEXT("save_path"), Paths.SavePath);
	Report->SetStringField(TEXT("initial_save_sha256"), Paths.InitialSaveHash);
	Report->SetStringField(TEXT("save_file_sha256"), Paths.SaveHash);
	Report->SetNumberField(TEXT("save_file_bytes"), static_cast<double>(Paths.SaveBytes));
	Report->SetBoolField(TEXT("save_file_exists"), Paths.bSaveExists);
	Report->SetStringField(TEXT("score_text"), Observation.Score);
	Report->SetStringField(TEXT("status_text"), Observation.Status);
	Report->SetStringField(TEXT("started_utc"), StartedUtc);
	Report->SetStringField(TEXT("finished_utc"), FDateTime::UtcNow().ToIso8601());
	Report->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
	Report->SetNumberField(TEXT("timeout_seconds"), TimeoutSeconds);
	Report->SetStringField(TEXT("runtime_snapshot_phase"), Observation.bCaptured && Observation.Snapshot.bHasActiveRuntime
		? TEXT("final_active") : TEXT("unavailable"));
	Report->SetObjectField(TEXT("runtime"), Observation.Runtime);
	Report->SetObjectField(TEXT("startup"), Observation.Startup);
	Report->SetObjectField(TEXT("backend"), Observation.Backend);
	Report->SetObjectField(TEXT("resources"), Observation.Resources);
	Report->SetArrayField(TEXT("actions"), Actions);
	FString Json;
	if (!FJsonSerializer::Serialize(Report, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json)))
	{
		Error = TEXT("report_serialization_failed"); return false;
	}
	return Paths.WriteNewReport(Json, Error);
}
}
