#include "Demos/AvidScriptUiSaveDemoWorld.h"
#include "Demos/AvidScriptUiSaveDemoObservation.h"

#include "AvidScriptComponent.h"
#include "AvidScriptHash.h"
#include "AvidScriptVmDiagnostics.h"
#include "AvidScriptWorldSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectIterator.h"

namespace AvidScript::UiSaveDemo
{
namespace
{
constexpr TCHAR WorldProbeMap[] = TEXT("/AvidScript/Demos/UiSave/L_UiSave");
constexpr TCHAR WorldProbeHost[] = TEXT("/AvidScript/Demos/UiSave/BP_UiSaveHost.BP_UiSaveHost_C");
constexpr TCHAR WorldProbeModule[] = TEXT("avidscript.ui_save_demo");

UObject* ReadRetiredWorldProbeObject(UObject& Owner, const TCHAR* Name)
{
	const FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(Owner.GetClass(), Name);
	return Property ? Property->GetObjectPropertyValue_InContainer(&Owner) : nullptr;
}

int32 CountRetiredWorldProbeButtons(UUserWidget& Widget)
{
	int32 Count = 0;
	for (const TCHAR* Name : { TEXT("CollectButton"), TEXT("SaveButton"), TEXT("LoadButton"), TEXT("ResetButton") })
	{
		const UButton* Button = Cast<UButton>(ReadRetiredWorldProbeObject(Widget, Name));
		if (Button && Button->OnClicked.IsBound()) { ++Count; }
	}
	return Count;
}

bool ReadWorldProbeOption(const TCHAR* Name, int32& Value, int32 Min, int32 Max)
{
	FString Text;
	if (!FParse::Value(FCommandLine::Get(), Name, Text)) { return true; }
	if (Text.IsEmpty() || Text.Len() > 8) { return false; }
	for (TCHAR Character : Text) { if (Character < '0' || Character > '9') { return false; } }
	return LexTryParseString(Value, *Text) && Value >= Min && Value <= Max;
}
}

FUiSaveWorld::FUiSaveWorld(FString InPackage, FString InSavePath, TFunction<bool()> InValidateDirectory)
	: ExpectedPackage(MoveTemp(InPackage)), SavePath(MoveTemp(InSavePath)), ValidateDirectory(MoveTemp(InValidateDirectory))
{
}

FUiSaveWorld::~FUiSaveWorld()
{
	FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
}

bool FUiSaveWorld::Initialize(FString& Error)
{
	Started = StageStarted = FPlatformTime::Seconds();
	if (!ReadWorldProbeOption(TEXT("AvidScriptUiSaveWorldCycles="), RequestedCycles, 2, 1000)
		|| !ReadWorldProbeOption(TEXT("AvidScriptUiSaveSoakSeconds="), SoakSeconds, 0, 21600))
	{
		Error = TEXT("world_probe_parameters_invalid"); return false;
	}
	MemorySummary->SetBoolField(TEXT("baseline_available"), false);
	for (const TCHAR* Field : { TEXT("baseline_physical_bytes"), TEXT("final_physical_bytes"), TEXT("peak_physical_bytes"),
		TEXT("baseline_virtual_bytes"), TEXT("final_virtual_bytes"), TEXT("peak_virtual_bytes") })
	{
		MemorySummary->SetNumberField(Field, 0);
	}
	CleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(this, &FUiSaveWorld::HandleWorldCleanup);
	return CheckSaveFile(Error);
}

bool FUiSaveWorld::CheckSaveFile(FString& Error) const
{
	if (!ValidateDirectory()) { Error = TEXT("world_save_directory_changed"); return false; }
	if (SaveHash.IsEmpty())
	{
		if (IFileManager::Get().FileExists(*SavePath)) { Error = TEXT("world_unexpected_save_file"); return false; }
		return true;
	}
	TArray<uint8> Bytes;
	if (IFileManager::Get().FileSize(*SavePath) != SaveBytes || SaveBytes <= 0 || SaveBytes > 4 * 1024 * 1024
		|| !FFileHelper::LoadFileToArray(Bytes, *SavePath) || FAvidScriptHash::Sha256Hex(Bytes) != SaveHash)
	{
		Error = TEXT("world_save_hash_changed_outside_save"); return false;
	}
	return true;
}

bool FUiSaveWorld::ReadText(FString& Score, FString& Status) const
{
	const UTextBlock* ScoreText = ReadUiObject<UTextBlock>(Widget.Get(), TEXT("ScoreText"));
	const UTextBlock* StatusText = ReadUiObject<UTextBlock>(Widget.Get(), TEXT("StatusText"));
	if (!ScoreText || !StatusText) { return false; }
	Score = ScoreText->GetText().ToString();
	Status = StatusText->GetText().ToString();
	return true;
}

bool FUiSaveWorld::FindReadyWorld(UWorld*& OutWorld, AActor*& OutHost, UAvidScriptComponent*& OutComponent,
	UUserWidget*& OutWidget, FString& Error)
{
	if (!GEngine) { return false; }
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* Candidate = Context.World();
		if (!IsValid(Candidate) || Candidate->WorldType != EWorldType::Game
			|| Candidate->GetOutermost()->GetName() != WorldProbeMap || !Candidate->HasBegunPlay()) { continue; }
		if (OutWorld) { Error = TEXT("world_probe_ambiguous_world"); return false; }
		OutWorld = Candidate;
	}
	if (!OutWorld || (Stage == EStage::Travelling && FObjectKey(OutWorld) == PreviousWorld)) { return false; }
	for (TActorIterator<AActor> It(OutWorld); It; ++It)
	{
		if (It->GetClass()->GetPathName() != WorldProbeHost) { continue; }
		if (OutHost) { Error = TEXT("world_probe_ambiguous_host"); return false; }
		OutHost = *It;
	}
	if (!OutHost) { return false; }
	TArray<UAvidScriptComponent*> Components;
	OutHost->GetComponents(Components);
	for (UAvidScriptComponent* Candidate : Components)
	{
		if (Candidate->GetScriptModuleId() != FName(WorldProbeModule)) { continue; }
		if (OutComponent) { Error = TEXT("world_probe_ambiguous_component"); return false; }
		OutComponent = Candidate;
	}
	if (!OutComponent) { return false; }
	const auto& Stats = OutComponent->GetRuntimeStats();
	if (!Stats.LastErrorMessage.IsEmpty()) { Error = TEXT("world_script_startup_failed: ") + Stats.LastErrorMessage; return false; }
	if (!Stats.bRuntimeLoaded || !Stats.bBeginPlayCalled) { return false; }
	if (Stats.ModuleId != WorldProbeModule || Stats.PackageId != ExpectedPackage || !Stats.bResolvedFromPackage
		|| !Stats.bOwnerRegistered || !Stats.OwnerHandle.IsValid() || Stats.DroppedGameplayEventCount != 0)
	{
		Error = TEXT("world_runtime_identity_or_owner_invalid"); return false;
	}
	const auto* Subsystem = OutWorld->GetSubsystem<UAvidScriptWorldSubsystem>();
	if (!Subsystem || !Subsystem->GetRuntimeStats().bStartupScenarioActive
		|| Subsystem->GetRuntimeStats().StartupScenarioId != TEXT("ui_save_demo")) { return false; }
	OutWidget = ReadUiObject<UUserWidget>(OutHost, TEXT("RootWidget"));
	const UTextBlock* Score = ReadUiObject<UTextBlock>(OutWidget, TEXT("ScoreText"));
	const UTextBlock* Status = ReadUiObject<UTextBlock>(OutWidget, TEXT("StatusText"));
	if (!OutWidget || !OutWidget->IsInViewport() || !Score || !Status) { return false; }
	if (Status->GetText().ToString() != TEXT("Ready")) { return false; }
	if (Score->GetText().ToString() != TEXT("0") || Stats.EventCallbackCount != 0
		|| ReadUiObject<UObject>(OutHost, TEXT("SavedObject")))
	{
		Error = TEXT("world_new_session_did_not_reset"); return false;
	}
	return true;
}

bool FUiSaveWorld::AcceptWorld(UWorld& NewWorld, AActor& NewHost, UAvidScriptComponent& NewComponent,
	UUserWidget& NewWidget, FString& Error)
{
	if (!CheckSaveFile(Error)) { return false; }
	if (Stage == EStage::Travelling)
	{
		if (!CurrentCycle->HasField(TEXT("cleanup"))) { Error = TEXT("world_cleanup_not_observed"); return false; }
		CollectGarbage(RF_NoFlags, true);
		const TSharedRef<FJsonObject> Gc = MakeShared<FJsonObject>();
		const bool Values[] = { World.GetEvenIfUnreachable() == nullptr, Host.GetEvenIfUnreachable() == nullptr,
			Component.GetEvenIfUnreachable() == nullptr, Widget.GetEvenIfUnreachable() == nullptr,
			SavedObject.GetEvenIfUnreachable() == nullptr, FObjectKey(&NewWorld) != PreviousWorld,
			FObjectKey(&NewHost) != PreviousHost, FObjectKey(&NewComponent) != PreviousComponent };
		const TCHAR* Names[] = { TEXT("world_collected"), TEXT("host_collected"), TEXT("component_collected"),
			TEXT("widget_collected"), TEXT("saved_object_collected"), TEXT("new_world_identity"),
			TEXT("new_host_identity"), TEXT("new_component_identity") };
		CurrentCycle->SetObjectField(TEXT("gc"), Gc);
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Values); ++Index)
		{
			Gc->SetBoolField(Names[Index], Values[Index]);
			if (!Values[Index]) { Error = TEXT("world_lifetime_not_released: ") + FString(Names[Index]); return false; }
		}
		int32 ActiveSessions = 0;
		for (TObjectIterator<UAvidScriptComponent> It; It; ++It)
		{
			if (IsValid(*It) && !It->HasAnyFlags(RF_ClassDefaultObject) && It->GetRuntimeSessionForEditorDebugging()) { ++ActiveSessions; }
		}
		Gc->SetNumberField(TEXT("active_sessions"), ActiveSessions);
		if (ActiveSessions != 1) { Error = TEXT("world_active_session_growth"); return false; }
		++CompletedCycles;
		AddMemorySample(*Gc);
		CurrentCycle->SetBoolField(TEXT("passed"), true);
	}
	World = &NewWorld; Host = &NewHost; Component = &NewComponent; Widget = &NewWidget;
	SavedObject.Reset();
	++ActivatedWorlds;
	bFinalRecovery = CompletedCycles >= RequestedCycles && FPlatformTime::Seconds() - Started >= SoakSeconds;
	if (!bFinalRecovery && CompletedCycles >= 10000) { Error = TEXT("world_cycle_limit_reached_before_duration"); return false; }
	Actions.Reset(); Steps.Reset(); StepIndex = 0; bDispatched = false;
	Steps.Add({ TEXT("ready"), NAME_None, TEXT("0"), TEXT("Ready") });
	if (CompletedCycles > 0) { Steps.Add({ TEXT("load"), TEXT("LoadButton"), FString::FromInt(CompletedCycles), TEXT("Loaded") }); }
	if (!bFinalRecovery)
	{
		CurrentCycle = MakeShared<FJsonObject>();
		CurrentCycle->SetNumberField(TEXT("cycle"), CompletedCycles + 1);
		CurrentCycle->SetStringField(TEXT("save_sha256_before"), SaveHash);
		CurrentCycle->SetBoolField(TEXT("passed"), false);
		Cycles.Add(MakeShared<FJsonValueObject>(CurrentCycle));
		const FString Score = FString::FromInt(CompletedCycles + 1);
		Steps.Add({ TEXT("collect"), TEXT("CollectButton"), Score, TEXT("Collected") });
		Steps.Add({ TEXT("save"), TEXT("SaveButton"), Score, TEXT("Saved") });
		Steps.Add({ TEXT("gc"), NAME_None, Score, TEXT("Saved") });
	}
	Stage = EStage::Actions; StageStarted = FPlatformTime::Seconds();
	return true;
}

bool FUiSaveWorld::TickAction(FString& Error)
{
	if (!World.IsValid() || !Host.IsValid() || !Component.IsValid() || !Widget.IsValid())
	{
		Error = TEXT("world_observer_lost_during_actions"); return false;
	}
	const FProbeStep& Step = Steps[StepIndex];
	if (!bDispatched)
	{
		if (!CheckSaveFile(Error)) { return false; }
		const TSharedRef<FJsonObject> Action = MakeShared<FJsonObject>();
		Action->SetStringField(TEXT("action"), Step.Action);
		Action->SetStringField(TEXT("expected_score"), Step.Score);
		Action->SetStringField(TEXT("expected_status"), Step.Status);
		Action->SetStringField(TEXT("save_sha256_before"), SaveHash);
		Action->SetNumberField(TEXT("dispatch_frame"), static_cast<double>(GFrameCounter));
		Action->SetBoolField(TEXT("passed"), false);
		Actions.Add(MakeShared<FJsonValueObject>(Action));
		if (!bFinalRecovery) { CurrentCycle->SetArrayField(TEXT("actions"), Actions); }
		DispatchFrame = GFrameCounter; bDispatched = true; StageStarted = FPlatformTime::Seconds();
		if (!Step.Button.IsNone())
		{
			UButton* Button = ReadUiObject<UButton>(Widget.Get(), Step.Button);
			if (!Button || !Button->GetIsEnabled() || !Button->OnClicked.IsBound()) { Error = TEXT("world_button_unavailable"); return false; }
			Button->OnClicked.Broadcast();
		}
		else if (Step.Action == TEXT("gc")) { CollectGarbage(RF_NoFlags, true); }
		return true;
	}
	if (GFrameCounter <= DispatchFrame + 1) { return true; }
	FString Score, Status;
	if (!ReadText(Score, Status)) { Error = TEXT("world_text_unavailable"); return false; }
	const TSharedPtr<FJsonObject> Action = Actions.Last()->AsObject();
	Action->SetStringField(TEXT("observed_score"), Score);
	Action->SetStringField(TEXT("observed_status"), Status);
	Action->SetNumberField(TEXT("check_frame"), static_cast<double>(GFrameCounter));
	if (Score != Step.Score || Status != Step.Status)
	{
		if (FPlatformTime::Seconds() - StageStarted < 3.0) { return true; }
		Error = TEXT("world_csharp_ui_assertion_failed"); return false;
	}
	if (Step.Action == TEXT("save"))
	{
		if (!ValidateDirectory()) { Error = TEXT("world_save_directory_changed"); return false; }
		TArray<uint8> Bytes;
		SaveBytes = IFileManager::Get().FileSize(*SavePath);
		if (SaveBytes <= 0 || SaveBytes > 4 * 1024 * 1024 || !FFileHelper::LoadFileToArray(Bytes, *SavePath))
		{
			Error = TEXT("world_save_missing_or_oversized"); return false;
		}
		const FString NewHash = FAvidScriptHash::Sha256Hex(Bytes);
		if (NewHash == SaveHash) { Error = TEXT("world_save_did_not_change"); return false; }
		SaveHash = NewHash;
		SavedObject = ReadUiObject<UObject>(Host.Get(), TEXT("SavedObject"));
		CurrentCycle->SetNumberField(TEXT("saved_score"), CompletedCycles + 1);
		CurrentCycle->SetStringField(TEXT("save_sha256"), SaveHash);
		CurrentCycle->SetNumberField(TEXT("save_file_bytes"), static_cast<double>(SaveBytes));
	}
	else if (!CheckSaveFile(Error)) { return false; }
	if (Step.Action == TEXT("load") || Step.Action == TEXT("save") || Step.Action == TEXT("gc"))
	{
		UObject* Saved = ReadUiObject<UObject>(Host.Get(), TEXT("SavedObject"));
		const FIntProperty* Property = IsValid(Saved) ? FindFProperty<FIntProperty>(Saved->GetClass(), TEXT("Score")) : nullptr;
		if (!Property || Property->GetPropertyValue_InContainer(Saved) != FCString::Atoi(*Step.Score))
		{
			Error = TEXT("world_save_object_score_invalid"); return false;
		}
		if (Step.Action == TEXT("gc") && Saved != SavedObject.Get()) { Error = TEXT("world_current_save_collected"); return false; }
	}
	Action->SetStringField(TEXT("save_sha256_after"), SaveHash);
	Action->SetBoolField(TEXT("passed"), true);
	if (++StepIndex < Steps.Num()) { bDispatched = false; return true; }
	if (!CaptureResources(Error)) { return false; }
	if (bFinalRecovery) { Finished = FPlatformTime::Seconds(); Stage = EStage::Complete; }
	else
	{
		CurrentCycle->SetNumberField(TEXT("events_before_travel"), Component->GetRuntimeStats().EventCallbackCount);
		Stage = EStage::Dwell; StageStarted = FPlatformTime::Seconds();
	}
	return true;
}

void FUiSaveWorld::BeginTravel()
{
	PreviousWorld = FObjectKey(World.Get()); PreviousHost = FObjectKey(Host.Get()); PreviousComponent = FObjectKey(Component.Get());
	Stage = EStage::Travelling; StageStarted = FPlatformTime::Seconds(); ++TravelCount;
	UGameplayStatics::OpenLevel(World.Get(), FName(WorldProbeMap), true);
}

void FUiSaveWorld::HandleWorldCleanup(UWorld* CleanedWorld, bool bSessionEnded, bool bCleanupResources)
{
	if (Stage != EStage::Travelling || FObjectKey(CleanedWorld) != PreviousWorld) { return; }
	if (CurrentCycle->HasField(TEXT("cleanup"))) { Failure = TEXT("world_cleanup_duplicate"); return; }
	// Cleanup precedes GC purge. Read stopped-object diagnostics only; never invoke old gameplay.
	const UAvidScriptComponent* OldComponent = Component.GetEvenIfUnreachable();
	UUserWidget* OldWidget = Widget.GetEvenIfUnreachable();
	AActor* OldHost = Host.GetEvenIfUnreachable();
	if (!OldComponent || !OldWidget || !OldHost) { Failure = TEXT("world_cleanup_observers_missing"); return; }
	const auto& Stats = OldComponent->GetRuntimeStats();
	const TSharedRef<FJsonObject> Cleanup = MakeShared<FJsonObject>();
	CurrentCycle->SetObjectField(TEXT("cleanup"), Cleanup);
	const bool Values[] = { true, Stats.bComponentEndPlayObserved, Stats.bEndPlayCalled,
		OldComponent->GetRuntimeSessionForEditorDebugging() == nullptr, Stats.bOwnerReleased,
		CountRetiredWorldProbeButtons(*OldWidget) == 0, !OldWidget->IsInViewport(), ReadRetiredWorldProbeObject(*OldHost, TEXT("SavedObject")) == nullptr };
	const TCHAR* Names[] = { TEXT("observed"), TEXT("component_end_play"), TEXT("guest_end_play"), TEXT("session_released"),
		TEXT("owner_released"), TEXT("buttons_unbound"), TEXT("widget_removed"), TEXT("saved_reference_cleared") };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Values); ++Index)
	{
		Cleanup->SetBoolField(Names[Index], Values[Index]);
		if (!Values[Index] && Failure.IsEmpty()) { Failure = TEXT("world_cleanup_retained_resource: ") + FString(Names[Index]); }
	}
	const int32 Before = static_cast<int32>(CurrentCycle->GetNumberField(TEXT("events_before_travel")));
	Cleanup->SetNumberField(TEXT("events_before"), Before);
	Cleanup->SetNumberField(TEXT("events_after"), Stats.EventCallbackCount);
	if (Stats.EventCallbackCount != Before) { Failure = TEXT("world_teardown_dispatched_unexpected_event"); }
	++CleanupCount;
}

void FUiSaveWorld::AddMemorySample(FJsonObject& Gc)
{
	const FAvidScriptVmMemorySnapshot Vm = CaptureAvidScriptVmMemorySnapshot();
	const TSharedRef<FJsonObject> Attribution = MakeShared<FJsonObject>();
	Attribution->SetNumberField(TEXT("schema_version"), 1);
	Attribution->SetNumberField(TEXT("backend_created"), static_cast<double>(Vm.BackendCreatedCount));
	Attribution->SetNumberField(TEXT("backend_destroyed"), static_cast<double>(Vm.BackendDestroyedCount));
	Attribution->SetNumberField(TEXT("backend_live"), static_cast<double>(Vm.BackendLiveCount));
	Attribution->SetNumberField(TEXT("artifact_cache_entries"), Vm.ArtifactCacheEntries);
	Attribution->SetNumberField(TEXT("artifact_cache_capacity"), Vm.ArtifactCacheCapacity);
	Attribution->SetNumberField(TEXT("artifact_cache_allocated_bytes"), static_cast<double>(Vm.ArtifactCacheAllocatedBytes));
	Attribution->SetNumberField(TEXT("attestation_entries"), Vm.AttestationEntries);
	Attribution->SetNumberField(TEXT("attestation_capacity"), Vm.AttestationCapacity);
	Attribution->SetNumberField(TEXT("attestation_allocated_bytes"), static_cast<double>(Vm.AttestationAllocatedBytes));
	RetainedActionCount += Actions.Num();
	Attribution->SetNumberField(TEXT("observer_retained_cycles"), CompletedCycles);
	Attribution->SetNumberField(TEXT("observer_retained_actions"), static_cast<double>(RetainedActionCount));
	Attribution->SetStringField(TEXT("observer_estimate_kind"), TEXT("ue_json_memory_footprint"));
	Attribution->SetNumberField(TEXT("observer_json_estimated_bytes"), 0);
	Gc.SetObjectField(TEXT("attribution"), Attribution);
	for (const TCHAR* Field : { TEXT("physical_bytes"), TEXT("virtual_bytes"), TEXT("uobject_count") })
	{
		Gc.SetNumberField(Field, 0);
	}
	// Completed cycles are immutable after this boundary. Do not walk the growing history on every sample.
	RetainedCycleJsonBytes += Cycles.Last()->GetMemoryFootprint();
	Attribution->SetNumberField(TEXT("observer_json_estimated_bytes"),
		static_cast<double>(RetainedCycleJsonBytes + Cycles.GetAllocatedSize() + Actions.GetAllocatedSize()));
	const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
	Gc.SetNumberField(TEXT("physical_bytes"), static_cast<double>(Memory.UsedPhysical));
	Gc.SetNumberField(TEXT("virtual_bytes"), static_cast<double>(Memory.UsedVirtual));
	Gc.SetNumberField(TEXT("uobject_count"), GUObjectArray.GetObjectArrayNumMinusAvailable());
	PeakPhysical = FMath::Max(PeakPhysical, static_cast<uint64>(Memory.UsedPhysical));
	PeakVirtual = FMath::Max(PeakVirtual, static_cast<uint64>(Memory.UsedVirtual));
	if (CompletedCycles == 3)
	{
		MemorySummary->SetBoolField(TEXT("baseline_available"), true);
		MemorySummary->SetNumberField(TEXT("baseline_physical_bytes"), static_cast<double>(Memory.UsedPhysical));
		MemorySummary->SetNumberField(TEXT("baseline_virtual_bytes"), static_cast<double>(Memory.UsedVirtual));
	}
	MemorySummary->SetNumberField(TEXT("final_physical_bytes"), static_cast<double>(Memory.UsedPhysical));
	MemorySummary->SetNumberField(TEXT("final_virtual_bytes"), static_cast<double>(Memory.UsedVirtual));
	MemorySummary->SetNumberField(TEXT("peak_physical_bytes"), static_cast<double>(PeakPhysical));
	MemorySummary->SetNumberField(TEXT("peak_virtual_bytes"), static_cast<double>(PeakVirtual));
}

EWorldProbeResult FUiSaveWorld::Tick(FString& Error)
{
	if (!Failure.IsEmpty()) { Error = Failure; return EWorldProbeResult::Failed; }
	if (Stage == EStage::Complete) { return EWorldProbeResult::Succeeded; }
	if (FPlatformTime::Seconds() - Started >= GetTimeoutSeconds()) { Error = TEXT("world_probe_timeout"); return EWorldProbeResult::Failed; }
	if (Stage == EStage::AwaitWorld || Stage == EStage::Travelling)
	{
		UWorld* NewWorld = nullptr; AActor* NewHost = nullptr; UAvidScriptComponent* NewComponent = nullptr; UUserWidget* NewWidget = nullptr;
		if (FindReadyWorld(NewWorld, NewHost, NewComponent, NewWidget, Error))
		{
			if (!AcceptWorld(*NewWorld, *NewHost, *NewComponent, *NewWidget, Error)) { return EWorldProbeResult::Failed; }
		}
		else if (Error.IsEmpty() && FPlatformTime::Seconds() - StageStarted > 30.0) { Error = TEXT("world_ready_or_travel_timeout"); }
	}
	else if (Stage == EStage::Actions) { TickAction(Error); }
	else if (Stage == EStage::Dwell)
	{
		FString Score, Status;
		if (!Component.IsValid() || !ReadText(Score, Status) || Score != FString::FromInt(CompletedCycles + 1)
			|| Status != TEXT("Saved") || Component->GetRuntimeStats().EventCallbackCount != (CompletedCycles == 0 ? 2 : 3))
		{
			Error = TEXT("world_state_changed_during_dwell");
		}
		else if (CheckSaveFile(Error) && FPlatformTime::Seconds() - StageStarted >= 3.0) { BeginTravel(); }
	}
	if (!Error.IsEmpty()) { return EWorldProbeResult::Failed; }
	return Stage == EStage::Complete ? EWorldProbeResult::Succeeded : EWorldProbeResult::Pending;
}
}
