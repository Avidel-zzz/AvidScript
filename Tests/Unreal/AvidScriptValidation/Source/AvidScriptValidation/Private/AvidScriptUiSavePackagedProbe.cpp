#include "AvidScriptUiSavePackagedProbe.h"

#include "Components/Button.h"
#include "CoreGlobals.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptPackagedUiSave, Log, All);

namespace AvidScript::Validation
{
FUiSavePackagedProbe::~FUiSavePackagedProbe()
{
	FTSTicker::RemoveTicker(Ticker);
}

void FUiSavePackagedProbe::Start()
{
	Started = FPlatformTime::Seconds();
	StartedUtc = FDateTime::UtcNow().ToIso8601();
	Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FUiSavePackagedProbe::Tick));
}

bool FUiSavePackagedProbe::Initialize(FString& Error)
{
	const TCHAR* Command = FCommandLine::Get();
	FParse::Value(Command, TEXT("AvidScriptUiSavePackagedProbe="), Mode);
	FParse::Value(Command, TEXT("AvidScriptUiSaveExpectedPackage="), ExpectedPackage);
	FParse::Value(Command, TEXT("AvidScriptUiSaveRunId="), RunId);
	FString Scenario;
	FParse::Value(Command, TEXT("AvidScriptScenario="), Scenario);
	// Establish confinement before any failure report is allowed to touch the filesystem.
	if (!Paths.Initialize(Error)) { return false; }
	if (!IsCookedGameProcess()) { Error = TEXT("probe_requires_actual_cooked_game"); return false; }
	if (BuildConfiguration() != TEXT("Development") && BuildConfiguration() != TEXT("Shipping"))
	{
		Error = TEXT("unsupported_build_configuration"); return false;
	}
	if (Mode != TEXT("write") && Mode != TEXT("read")) { Error = TEXT("unknown_packaged_ui_probe_mode"); return false; }
	if (Scenario != TEXT("ui_save_demo")) { Error = TEXT("scenario_must_be_ui_save_demo"); return false; }
	if (!IsHexIdentity(ExpectedPackage, 64)) { Error = TEXT("expected_package_requires_64hex"); return false; }
	if (!IsHexIdentity(RunId, 32)) { Error = TEXT("run_id_requires_32hex"); return false; }
	ExpectedPackage.ToLowerInline();
	RunId.ToLowerInline();
	if (!Paths.ReadSave(Error)) { return false; }
	if (Mode == TEXT("write") && Paths.bSaveExists) { Error = TEXT("write_requires_absent_save"); return false; }
	if (Mode == TEXT("read") && (!Paths.bSaveExists || Paths.SaveBytes == 0))
	{
		Error = TEXT("read_requires_previous_process_nonempty_save"); return false;
	}
	Paths.InitialSaveHash = Paths.SaveHash;
	Steps.Add({ TEXT("ready"), NAME_None, TEXT("0"), TEXT("Ready") });
	if (Mode == TEXT("write"))
	{
		for (int32 Score = 1; Score <= 3; ++Score)
		{
			Steps.Add({ TEXT("collect"), TEXT("CollectButton"), FString::FromInt(Score), TEXT("Collected") });
		}
		Steps.Add({ TEXT("save"), TEXT("SaveButton"), TEXT("3"), TEXT("Saved") });
	}
	else
	{
		Steps.Add({ TEXT("load"), TEXT("LoadButton"), TEXT("3"), TEXT("Loaded") });
	}
	return true;
}

bool FUiSavePackagedProbe::CheckSaveState(bool bRequireFinal, FString& Error)
{
	if (!Paths.ReadSave(Error)) { return false; }
	if (Mode == TEXT("read") && (!Paths.bSaveExists || Paths.SaveBytes == 0 || Paths.SaveHash != Paths.InitialSaveHash))
	{
		Error = TEXT("read_save_changed"); return false;
	}
	if (Mode == TEXT("write") && !bSaveDispatched && Paths.bSaveExists)
	{
		Error = TEXT("save_appeared_before_script_save"); return false;
	}
	if (bRequireFinal && (!Paths.bSaveExists || Paths.SaveBytes == 0))
	{
		Error = TEXT("script_save_missing_or_empty"); return false;
	}
	return true;
}

bool FUiSavePackagedProbe::Tick(float DeltaSeconds)
{
	if (bFinished) { return false; }
	FString Error;
	if (!bInitialized)
	{
		bInitialized = true;
		if (!Initialize(Error)) { Finish(false, Error); return false; }
	}
	// A ticker may be pumped more than once during one engine frame.
	if (bDispatched && GFrameCounter <= DispatchFrame)
	{
		if (FPlatformTime::Seconds() - Started >= TimeoutSeconds) { Finish(false, TEXT("probe_timeout")); return false; }
		return true;
	}
	if (!Observation.Refresh(ExpectedPackage, StepIndex > 0 || bDispatched, Error))
	{
		if (!Error.IsEmpty()) { Finish(false, Error); return false; }
		if (FPlatformTime::Seconds() - Started >= TimeoutSeconds) { Finish(false, TEXT("activation_timeout")); return false; }
		return true;
	}
	if (FPlatformTime::Seconds() - Started >= TimeoutSeconds) { Finish(false, TEXT("probe_timeout")); return false; }
	if (!CheckSaveState(false, Error)) { Finish(false, Error); return false; }
	if (StepIndex == 0 && !bDispatched)
	{
		if (Observation.Status == TEXT("UI unavailable") || Observation.Status == TEXT("Input unavailable"))
		{
			Finish(false, TEXT("script_ui_initialization_failed")); return false;
		}
		if (Observation.Status != TEXT("Ready")) { return true; }
		if (Observation.Score != TEXT("0")) { Finish(false, TEXT("ready_score_mismatch")); return false; }
	}
	if (!Observation.CheckResources(Error)) { Finish(false, Error); return false; }
	const FStep& Step = Steps[StepIndex];
	if (!bDispatched)
	{
		const int32 BeforeEvents = FMath::Max(0, StepIndex - 1);
		if (!Observation.CheckEvents(BeforeEvents, Error)) { Finish(false, Error); return false; }
		const TSharedRef<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("action"), Step.Action);
		Action->SetStringField(TEXT("expected_score"), Step.Score);
		Action->SetStringField(TEXT("expected_status"), Step.Status);
		Action->SetStringField(TEXT("observed_score"), Observation.Score);
		Action->SetStringField(TEXT("observed_status"), Observation.Status);
		DispatchFrame = GFrameCounter;
		Action->SetNumberField(TEXT("dispatch_frame"), static_cast<double>(DispatchFrame));
		Action->SetNumberField(TEXT("check_frame"), 0);
		Action->SetBoolField(TEXT("passed"), false);
		Actions.Add(MakeShared<FJsonValueObject>(Action));
		bDispatched = true;
		if (!Step.Button.IsNone())
		{
			UButton* Button = Observation.FindButton(Step.Button);
			if (!Button || !Button->GetIsEnabled() || !Button->OnClicked.IsBound())
			{
				Finish(false, TEXT("button_unavailable")); return false;
			}
			if (Step.Action == TEXT("save")) { bSaveDispatched = true; }
			Button->OnClicked.Broadcast();
		}
		return true;
	}
	const TSharedPtr<FJsonObject> Action = Actions.Last()->AsObject();
	Action->SetNumberField(TEXT("check_frame"), static_cast<double>(GFrameCounter));
	Action->SetStringField(TEXT("observed_score"), Observation.Score);
	Action->SetStringField(TEXT("observed_status"), Observation.Status);
	if (Observation.Score != Step.Score || Observation.Status != Step.Status)
	{
		Finish(false, TEXT("ui_action_result_mismatch")); return false;
	}
	if (!Observation.CheckEvents(StepIndex, Error)) { Finish(false, Error); return false; }
	const bool bFinal = StepIndex + 1 == Steps.Num();
	if (!CheckSaveState(bFinal, Error)) { Finish(false, Error); return false; }
	Action->SetBoolField(TEXT("passed"), true);
	++StepIndex;
	bDispatched = false;
	if (bFinal)
	{
		const int32 ExpectedActions = Mode == TEXT("write") ? 5 : 2;
		if (Actions.Num() != ExpectedActions || !Observation.CheckEvents(ExpectedActions - 1, Error))
		{
			Finish(false, TEXT("final_action_or_event_count_mismatch")); return false;
		}
		Finish(true, TEXT(""));
		return false;
	}
	return true;
}

void FUiSavePackagedProbe::Finish(bool bSucceeded, const FString& Failure)
{
	if (bFinished) { return; }
	bFinished = true;
	FString ReportError;
	const bool bWritten = WriteReport(bSucceeded, Failure, ReportError);
	UE_LOG(LogAvidScriptPackagedUiSave, Display, TEXT("%s category=%s report_error=%s"),
		bSucceeded && bWritten ? TEXT("avidscript_ui_save_probe_passed") : TEXT("avidscript_ui_save_probe_failed"),
		*Failure, *ReportError);
	FPlatformMisc::RequestExitWithStatus(false, bSucceeded && bWritten ? 0 : 1);
}
}
