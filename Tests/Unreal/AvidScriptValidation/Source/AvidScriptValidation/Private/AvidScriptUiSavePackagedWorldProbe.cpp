#include "AvidScriptUiSavePackagedWorldProbe.h"

#include "AvidScriptComponent.h"
#include "AvidScriptHash.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "CoreGlobals.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "UObject/GarbageCollection.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptPackagedWorld, Log, All);

namespace AvidScript::Validation
{
namespace PackagedWorldPrivate
{
template <typename T>
T* ReadObject(UObject* Object, FName Name)
{
	const FObjectPropertyBase* Property = Object ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
	return Property ? Cast<T>(Property->GetObjectPropertyValue_InContainer(Object)) : nullptr;
}

int32 CountBoundButtons(UUserWidget* Widget)
{
	int32 Count = 0;
	for (const TCHAR* Name : { TEXT("CollectButton"), TEXT("SaveButton"), TEXT("LoadButton"), TEXT("ResetButton") })
	{
		const UButton* Button = ReadObject<UButton>(Widget, Name);
		if (Button && Button->OnClicked.IsBound()) { ++Count; }
	}
	return Count;
}

bool ReadIntegerOption(const TCHAR* Name, int32& Value, int32 Minimum, int32 Maximum)
{
	FString Text;
	if (!FParse::Value(FCommandLine::Get(), Name, Text)) { return true; }
	if (Text.IsEmpty() || Text.Len() > 8) { return false; }
	for (const TCHAR Character : Text)
	{
		if (Character < TEXT('0') || Character > TEXT('9')) { return false; }
	}
	return LexTryParseString(Value, *Text) && Value >= Minimum && Value <= Maximum;
}
}

FUiSavePackagedWorldProbe::~FUiSavePackagedWorldProbe()
{
	FTSTicker::RemoveTicker(Ticker);
	FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
}

void FUiSavePackagedWorldProbe::Start()
{
	Started = StageStarted = FPlatformTime::Seconds();
	StartedUtc = FDateTime::UtcNow().ToIso8601();
	Ticker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FUiSavePackagedWorldProbe::Tick));
}

double FUiSavePackagedWorldProbe::GetTimeoutSeconds() const
{
	return SoakSeconds + FMath::Max(120.0, RequestedCycles * 20.0);
}

bool FUiSavePackagedWorldProbe::Initialize(FString& Error)
{
	const TCHAR* Command = FCommandLine::Get();
	FString Mode;
	FString Scenario;
	FParse::Value(Command, TEXT("AvidScriptUiSavePackagedProbe="), Mode);
	FParse::Value(Command, TEXT("AvidScriptUiSaveExpectedPackage="), ExpectedPackage);
	FParse::Value(Command, TEXT("AvidScriptUiSaveRunId="), RunId);
	FParse::Value(Command, TEXT("AvidScriptScenario="), Scenario);
	if (!Paths.Initialize(Error)) { return false; }
	if (!IsCookedGameProcess()) { Error = TEXT("probe_requires_actual_cooked_game"); return false; }
	if (BuildConfiguration() != TEXT("Development") && BuildConfiguration() != TEXT("Shipping"))
	{
		Error = TEXT("unsupported_build_configuration"); return false;
	}
	if (Mode != TEXT("world")) { Error = TEXT("packaged_world_probe_mode_required"); return false; }
	if (Scenario != TEXT("ui_save_demo")) { Error = TEXT("scenario_must_be_ui_save_demo"); return false; }
	if (!IsHexIdentity(ExpectedPackage, 64)) { Error = TEXT("expected_package_requires_64hex"); return false; }
	if (!IsHexIdentity(RunId, 32)) { Error = TEXT("run_id_requires_32hex"); return false; }
	if (!PackagedWorldPrivate::ReadIntegerOption(
			TEXT("AvidScriptUiSaveWorldCycles="), RequestedCycles, 2, 1000)
		|| !PackagedWorldPrivate::ReadIntegerOption(
			TEXT("AvidScriptUiSaveSoakSeconds="), SoakSeconds, 0, 21600))
	{
		Error = TEXT("world_probe_parameters_invalid"); return false;
	}
	ExpectedPackage.ToLowerInline();
	RunId.ToLowerInline();
	if (!Paths.ReadSave(Error)) { return false; }
	if (Paths.bSaveExists) { Error = TEXT("world_requires_absent_save"); return false; }
	Paths.InitialSaveHash.Reset();
	EvidenceChain = FAvidScriptHash::Sha256HexUtf8(
		TEXT("AvidScriptPackagedWorld/v1|") + RunId + TEXT("|") + ExpectedPackage);
	CleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(
		this, &FUiSavePackagedWorldProbe::HandleWorldCleanup);
	return true;
}

bool FUiSavePackagedWorldProbe::FindReadyObservation(FUiSaveObservation& Candidate, FString& Error) const
{
	if (!Candidate.Refresh(ExpectedPackage, false, Error)) { return false; }
	if (Candidate.Status == TEXT("UI unavailable") || Candidate.Status == TEXT("Input unavailable"))
	{
		Error = TEXT("script_ui_initialization_failed"); return false;
	}
	if (Candidate.Status != TEXT("Ready")) { return false; }
	if (Candidate.Score != TEXT("0")) { Error = TEXT("world_new_session_score_not_reset"); return false; }
	if (!Candidate.CheckEvents(0, Error) || !Candidate.CheckResources(Error)) { return false; }
	if (PackagedWorldPrivate::ReadObject<UObject>(Candidate.Host.Get(), TEXT("SavedObject")))
	{
		Error = TEXT("world_new_session_saved_object_not_reset"); return false;
	}
	return true;
}

bool FUiSavePackagedWorldProbe::AcceptWorld(FUiSaveObservation&& Candidate, FString& Error)
{
	if (Stage == EStage::Travelling)
	{
		if (!bCleanupObserved) { Error = TEXT("world_cleanup_not_observed"); return false; }
		CollectGarbage(RF_NoFlags, true);
		const bool bRetiredReleased = RetiredWorld.GetEvenIfUnreachable() == nullptr
			&& RetiredHost.GetEvenIfUnreachable() == nullptr
			&& RetiredComponent.GetEvenIfUnreachable() == nullptr
			&& RetiredWidget.GetEvenIfUnreachable() == nullptr
			&& RetiredSavedObject.GetEvenIfUnreachable() == nullptr;
		if (!bRetiredReleased) { Error = TEXT("world_lifetime_not_released_after_gc"); return false; }
		if (FObjectKey(Candidate.World.Get()) == PreviousWorld
			|| FObjectKey(Candidate.Host.Get()) == PreviousHost
			|| FObjectKey(Candidate.Component.Get()) == PreviousComponent)
		{
			Error = TEXT("world_identity_not_replaced"); return false;
		}
		++CompletedCycles;
		++GcChecksPassed;
	}

	Observation = MoveTemp(Candidate);
	++ActivatedWorlds;
	bFinalRecovery = CompletedCycles >= RequestedCycles
		&& FPlatformTime::Seconds() - Started >= SoakSeconds;
	if (!bFinalRecovery && CompletedCycles >= MaximumCycles)
	{
		Error = TEXT("world_cycle_limit_reached_before_duration"); return false;
	}
	if (CompletedCycles > 0)
	{
		if (!CaptureMemorySample(bFinalRecovery, Error)) { return false; }
		RetiredWorld.Reset();
		RetiredHost.Reset();
		RetiredComponent.Reset();
		RetiredWidget.Reset();
		RetiredSavedObject.Reset();
	}

	Steps.Reset();
	CurrentActions.Reset();
	StepIndex = 0;
	bDispatched = false;
	ActiveSavedObject.Reset();
	Steps.Add({ TEXT("ready"), NAME_None, TEXT("0"), TEXT("Ready"), 0 });
	if (CompletedCycles > 0)
	{
		Steps.Add({ TEXT("load"), TEXT("LoadButton"), FString::FromInt(CompletedCycles), TEXT("Loaded"), 1 });
	}
	if (!bFinalRecovery)
	{
		CurrentCycleNumber = CompletedCycles + 1;
		const FString Score = FString::FromInt(CurrentCycleNumber);
		const int32 LoadEvents = CompletedCycles > 0 ? 1 : 0;
		Steps.Add({ TEXT("collect"), TEXT("CollectButton"), Score, TEXT("Collected"), LoadEvents + 1 });
		Steps.Add({ TEXT("save"), TEXT("SaveButton"), Score, TEXT("Saved"), LoadEvents + 2 });
		Steps.Add({ TEXT("gc"), NAME_None, Score, TEXT("Saved"), LoadEvents + 2 });
	}
	Stage = EStage::Actions;
	StageStarted = FPlatformTime::Seconds();
	return true;
}

bool FUiSavePackagedWorldProbe::ValidateSaveUnchanged(FString& Error)
{
	if (!Paths.ReadSave(Error)) { return false; }
	if (SaveHash.IsEmpty())
	{
		if (Paths.bSaveExists) { Error = TEXT("save_appeared_outside_save_action"); return false; }
		return true;
	}
	if (!Paths.bSaveExists || Paths.SaveBytes != SaveBytes || Paths.SaveHash != SaveHash)
	{
		Error = TEXT("save_changed_outside_save_action"); return false;
	}
	return true;
}

bool FUiSavePackagedWorldProbe::CompleteSaveAction(FString& Error)
{
	if (!Paths.ReadSave(Error)) { return false; }
	if (!Paths.bSaveExists || Paths.SaveBytes <= 0 || Paths.SaveHash.IsEmpty() || Paths.SaveHash == SaveHash)
	{
		Error = TEXT("save_action_did_not_create_new_content"); return false;
	}
	SaveHash = Paths.SaveHash;
	SaveBytes = Paths.SaveBytes;
	ActiveSavedObject = PackagedWorldPrivate::ReadObject<UObject>(Observation.Host.Get(), TEXT("SavedObject"));
	if (!ActiveSavedObject.IsValid()) { Error = TEXT("save_action_did_not_retain_save_object"); return false; }
	return true;
}

bool FUiSavePackagedWorldProbe::CaptureResources(FString& Error)
{
	if (!Observation.CheckResources(Error)) { return false; }
	if (!Observation.bCaptured || Observation.BackendInfo.Kind != EAvidScriptVmBackendKind::Wasmtime
		|| Observation.BackendInfo.ExecutionMode != EAvidScriptVmExecutionMode::Aot
		|| Observation.BackendInfo.ArtifactFormat != EAvidScriptVmArtifactFormat::WasmtimeSerialized)
	{
		Error = TEXT("world_backend_requires_wasmtime_aot_serialized"); return false;
	}
	++ResourceChecksPassed;
	return true;
}

bool FUiSavePackagedWorldProbe::TickAction(FString& Error)
{
	if (!Observation.Refresh(ExpectedPackage, true, Error)) { return false; }
	if (StepIndex < 0 || StepIndex >= Steps.Num()) { Error = TEXT("world_action_index_invalid"); return false; }
	const FStep& Step = Steps[StepIndex];
	if ((!bDispatched || Step.Action != TEXT("save")) && !ValidateSaveUnchanged(Error)) { return false; }
	if (!bDispatched)
	{
		if (!Observation.CheckEvents(Step.Events - (Step.Button.IsNone() ? 0 : 1), Error)) { return false; }
		const TSharedRef<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("action"), Step.Action);
		Action->SetStringField(TEXT("expected_score"), Step.Score);
		Action->SetStringField(TEXT("expected_status"), Step.Status);
		Action->SetStringField(TEXT("observed_score"), Observation.Score);
		Action->SetStringField(TEXT("observed_status"), Observation.Status);
		Action->SetStringField(TEXT("save_sha256_before"), SaveHash);
		DispatchFrame = GFrameCounter;
		Action->SetNumberField(TEXT("dispatch_frame"), static_cast<double>(DispatchFrame));
		Action->SetNumberField(TEXT("check_frame"), 0);
		Action->SetBoolField(TEXT("passed"), false);
		CurrentActions.Add(MakeShared<FJsonValueObject>(Action));
		bDispatched = true;
		StageStarted = FPlatformTime::Seconds();
		if (!Step.Button.IsNone())
		{
			UButton* Button = Observation.FindButton(Step.Button);
			if (!Button || !Button->GetIsEnabled() || !Button->OnClicked.IsBound())
			{
				Error = TEXT("world_button_unavailable"); return false;
			}
			Button->OnClicked.Broadcast();
		}
		else if (Step.Action == TEXT("gc"))
		{
			CollectGarbage(RF_NoFlags, true);
		}
		return true;
	}
	if (GFrameCounter <= DispatchFrame + 1) { return true; }
	if (!Observation.Refresh(ExpectedPackage, true, Error)) { return false; }
	if (Observation.Score != Step.Score || Observation.Status != Step.Status)
	{
		if (FPlatformTime::Seconds() - StageStarted < MinimumDwellSeconds) { return true; }
		Error = TEXT("world_csharp_ui_assertion_failed"); return false;
	}
	if (!Observation.CheckEvents(Step.Events, Error)) { return false; }
	if (Step.Action == TEXT("save"))
	{
		if (!CompleteSaveAction(Error)) { return false; }
	}
	else if (!ValidateSaveUnchanged(Error)) { return false; }

	if (Step.Action == TEXT("load") || Step.Action == TEXT("save") || Step.Action == TEXT("gc"))
	{
		UObject* Saved = PackagedWorldPrivate::ReadObject<UObject>(Observation.Host.Get(), TEXT("SavedObject"));
		const FIntProperty* ScoreProperty = IsValid(Saved)
			? FindFProperty<FIntProperty>(Saved->GetClass(), TEXT("Score")) : nullptr;
		if (!ScoreProperty || ScoreProperty->GetPropertyValue_InContainer(Saved) != FCString::Atoi(*Step.Score))
		{
			Error = TEXT("world_save_object_score_invalid"); return false;
		}
		if (Step.Action == TEXT("load")) { ActiveSavedObject = Saved; }
		if (Step.Action == TEXT("gc") && Saved != ActiveSavedObject.Get())
		{
			Error = TEXT("world_current_save_object_collected"); return false;
		}
	}
	const TSharedPtr<FJsonObject> Action = CurrentActions.Last()->AsObject();
	Action->SetStringField(TEXT("observed_score"), Observation.Score);
	Action->SetStringField(TEXT("observed_status"), Observation.Status);
	Action->SetStringField(TEXT("save_sha256_after"), SaveHash);
	Action->SetNumberField(TEXT("check_frame"), static_cast<double>(GFrameCounter));
	Action->SetBoolField(TEXT("passed"), true);
	++ActionChecksPassed;
	++StepIndex;
	bDispatched = false;
	if (StepIndex < Steps.Num()) { return true; }
	if (!CaptureResources(Error)) { return false; }
	if (bFinalRecovery)
	{
		Finished = FPlatformTime::Seconds();
		Stage = EStage::Complete;
		return true;
	}
	CurrentCycleEvents = Observation.Events;
	Stage = EStage::Dwell;
	StageStarted = FPlatformTime::Seconds();
	return true;
}

void FUiSavePackagedWorldProbe::BeginTravel()
{
	RetiredWorld = Observation.World;
	RetiredHost = Observation.Host;
	RetiredComponent = Observation.Component;
	RetiredWidget = Observation.Widget;
	RetiredSavedObject = ActiveSavedObject;
	PreviousWorld = FObjectKey(Observation.World.Get());
	PreviousHost = FObjectKey(Observation.Host.Get());
	PreviousComponent = FObjectKey(Observation.Component.Get());
	bCleanupObserved = false;
	Stage = EStage::Travelling;
	StageStarted = FPlatformTime::Seconds();
	++TravelCount;
	UGameplayStatics::OpenLevel(Observation.World.Get(), FName(UiSaveMap), true);
}

void FUiSavePackagedWorldProbe::HandleWorldCleanup(
	UWorld* CleanedWorld,
	bool bSessionEnded,
	bool bCleanupResources)
{
	if (Stage != EStage::Travelling || FObjectKey(CleanedWorld) != PreviousWorld) { return; }
	if (bCleanupObserved) { Failure = TEXT("world_cleanup_duplicate"); return; }
	bCleanupObserved = true;
	UAvidScriptComponent* Component = RetiredComponent.GetEvenIfUnreachable();
	UUserWidget* Widget = RetiredWidget.GetEvenIfUnreachable();
	AActor* Host = RetiredHost.GetEvenIfUnreachable();
	if (!Component || !Widget || !Host)
	{
		Failure = TEXT("world_cleanup_observers_missing"); return;
	}
	const FAvidScriptComponentRuntimeStats& Stats = Component->GetRuntimeStats();
	FAvidScriptRuntimeSessionSnapshot StoppedSnapshot;
	FAvidScriptVmBackendInfo StoppedBackend;
	const bool bStoppedCaptureRejected = !Component->CaptureRuntimeDiagnostics(StoppedSnapshot, StoppedBackend)
		&& !StoppedSnapshot.bHasActiveRuntime && StoppedBackend.StableBackendId.IsEmpty();
	const bool bCleanupValid = Stats.bComponentEndPlayObserved && Stats.bEndPlayCalled
		&& !Stats.bRuntimeLoaded && Stats.bOwnerReleased && bStoppedCaptureRejected
		&& PackagedWorldPrivate::CountBoundButtons(Widget) == 0 && !Widget->IsInViewport()
		&& PackagedWorldPrivate::ReadObject<UObject>(Host, TEXT("SavedObject")) == nullptr
		&& Stats.EventCallbackCount == CurrentCycleEvents;
	if (!bCleanupValid)
	{
		Failure = TEXT("world_cleanup_retained_resource"); return;
	}
	++CleanupCount;
	++CleanupChecksPassed;
}

bool FUiSavePackagedWorldProbe::CountActiveSessions(int32& OutCount, FString& Error) const
{
	OutCount = 0;
	for (TObjectIterator<UAvidScriptComponent> It; It; ++It)
	{
		if (!IsValid(*It)
			|| It->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
		{
			continue;
		}
		FAvidScriptRuntimeSessionSnapshot Snapshot;
		FAvidScriptVmBackendInfo Backend;
		if (!It->CaptureRuntimeDiagnostics(Snapshot, Backend))
		{
			if (It->GetRuntimeStats().bRuntimeLoaded)
			{
				Error = TEXT("world_active_session_diagnostics_unavailable");
				return false;
			}
			continue;
		}
		if (!Snapshot.bHasActiveRuntime || Backend.StableBackendId.IsEmpty())
		{
			Error = TEXT("world_active_session_diagnostics_invalid");
			return false;
		}
		++OutCount;
	}
	return true;
}

bool FUiSavePackagedWorldProbe::Tick(float DeltaSeconds)
{
	if (bFinished) { return false; }
	FString Error;
	if (!bInitialized)
	{
		bInitialized = true;
		if (!Initialize(Error)) { Finish(false, Error); return false; }
	}
	if (!Failure.IsEmpty()) { Finish(false, Failure); return false; }
	if (FPlatformTime::Seconds() - Started >= GetTimeoutSeconds())
	{
		Finish(false, TEXT("world_probe_timeout")); return false;
	}
	if (Stage == EStage::Complete)
	{
		Finish(true, TEXT("")); return false;
	}
	if (Stage == EStage::AwaitWorld || Stage == EStage::Travelling)
	{
		FUiSaveObservation Candidate;
		if (FindReadyObservation(Candidate, Error))
		{
			if (!AcceptWorld(MoveTemp(Candidate), Error)) { Finish(false, Error); return false; }
		}
		else if (!Error.IsEmpty()) { Finish(false, Error); return false; }
		else if (FPlatformTime::Seconds() - StageStarted > 30.0)
		{
			Finish(false, TEXT("world_ready_or_travel_timeout")); return false;
		}
	}
	else if (Stage == EStage::Actions)
	{
		if (!TickAction(Error)) { Finish(false, Error); return false; }
	}
	else if (Stage == EStage::Dwell)
	{
		if (!Observation.Refresh(ExpectedPackage, true, Error)
			|| Observation.Score != FString::FromInt(CurrentCycleNumber)
			|| Observation.Status != TEXT("Saved")
			|| !Observation.CheckEvents(CurrentCycleEvents, Error)
			|| !ValidateSaveUnchanged(Error))
		{
			if (Error.IsEmpty()) { Error = TEXT("world_state_changed_during_dwell"); }
			Finish(false, Error); return false;
		}
		if (FPlatformTime::Seconds() - StageStarted >= MinimumDwellSeconds) { BeginTravel(); }
	}
	return true;
}

void FUiSavePackagedWorldProbe::Finish(bool bSucceeded, const FString& FailureCategory)
{
	if (bFinished) { return; }
	bFinished = true;
	if (Finished <= 0.0) { Finished = FPlatformTime::Seconds(); }
	FString ReportError;
	const bool bWritten = WriteReport(bSucceeded, FailureCategory, ReportError);
	UE_LOG(LogAvidScriptPackagedWorld, Display, TEXT("%s category=%s report_error=%s"),
		bSucceeded && bWritten ? TEXT("avidscript_ui_save_world_probe_passed")
			: TEXT("avidscript_ui_save_world_probe_failed"),
		*FailureCategory,
		*ReportError);
	FPlatformMisc::RequestExitWithStatus(false, bSucceeded && bWritten ? 0 : 1);
}
}
