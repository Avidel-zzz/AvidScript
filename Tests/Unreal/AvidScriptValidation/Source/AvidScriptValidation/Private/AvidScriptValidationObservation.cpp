#include "AvidScriptValidationObservation.h"

#include "AvidScriptComponent.h"
#include "AvidScriptWorldSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace AvidScript::Validation
{
namespace ObservationPrivate
{
template <typename T>
T* ReadObject(UObject* Object, FName Name)
{
	const FObjectPropertyBase* Property = Object ? FindFProperty<FObjectPropertyBase>(Object->GetClass(), Name) : nullptr;
	return Property ? Cast<T>(Property->GetObjectPropertyValue_InContainer(Object)) : nullptr;
}
}

FUiSaveObservation::FUiSaveObservation()
{
	for (const TCHAR* Key : { TEXT("module_id"), TEXT("package_id"), TEXT("error_message") }) { Runtime->SetStringField(Key, TEXT("")); }
	for (const TCHAR* Key : { TEXT("resolved_from_package"), TEXT("runtime_loaded"), TEXT("begin_play"),
		TEXT("owner_registered"), TEXT("owner_handle_valid") }) { Runtime->SetBoolField(Key, false); }
	Runtime->SetNumberField(TEXT("events"), 0);
	Runtime->SetNumberField(TEXT("dropped_events"), 0);
	Startup->SetBoolField(TEXT("active"), false);
	for (const TCHAR* Key : { TEXT("scenario_id"), TEXT("error_category"), TEXT("error_message") }) { Startup->SetStringField(Key, TEXT("")); }
	CaptureValues();
}

void FUiSaveObservation::CaptureValues()
{
	Backend->SetBoolField(TEXT("measured"), bCaptured);
	Backend->SetStringField(TEXT("source"), TEXT("CaptureRuntimeDiagnostics.BackendInfo"));
	Backend->SetStringField(TEXT("backend_id"), BackendInfo.StableBackendId);
	Backend->SetStringField(TEXT("backend_kind"), !bCaptured ? TEXT("unknown")
		: BackendInfo.Kind == EAvidScriptVmBackendKind::Wasmtime ? TEXT("wasmtime") : TEXT("wamr"));
	const TCHAR* Mode = TEXT("unknown");
	const TCHAR* Format = TEXT("unknown");
	if (bCaptured)
	{
		switch (BackendInfo.ExecutionMode)
		{
		case EAvidScriptVmExecutionMode::Auto: Mode = TEXT("auto"); break;
		case EAvidScriptVmExecutionMode::Interpreter: Mode = TEXT("interpreter"); break;
		case EAvidScriptVmExecutionMode::Aot: Mode = TEXT("aot"); break;
		case EAvidScriptVmExecutionMode::Jit: Mode = TEXT("jit"); break;
		}
		switch (BackendInfo.ArtifactFormat)
		{
		case EAvidScriptVmArtifactFormat::WasmBytecode: Format = TEXT("wasm_bytecode"); break;
		case EAvidScriptVmArtifactFormat::WamrAot: Format = TEXT("wamr_aot"); break;
		case EAvidScriptVmArtifactFormat::WasmtimeSerialized: Format = TEXT("wasmtime_serialized"); break;
		}
	}
	Backend->SetStringField(TEXT("execution_mode"), Mode);
	Backend->SetStringField(TEXT("artifact_format"), Format);
	Backend->SetStringField(TEXT("runtime_version"), BackendInfo.RuntimeVersion);
	Backend->SetStringField(TEXT("runtime_build_identity"), BackendInfo.RuntimeBuildIdentity);
	Backend->SetStringField(TEXT("runtime_artifact_sha256"), BackendInfo.RuntimeArtifactSha256);
	Backend->SetStringField(TEXT("target_triple"), BackendInfo.TargetTriple);
	Resources->SetNumberField(TEXT("active_subscriptions"), Snapshot.ActiveDelegateSubscriptionCount);
	Resources->SetNumberField(TEXT("bound_buttons"), BoundButtons);
	Resources->SetNumberField(TEXT("owned_entries"), Snapshot.OwnedObjectEntryCount);
	Resources->SetNumberField(TEXT("borrowed_entries"), Snapshot.BorrowedHandleEntryCount);
	Resources->SetNumberField(TEXT("pending_timers"), Snapshot.PendingTimerCount);
	Resources->SetNumberField(TEXT("pending_continuations"), Snapshot.PendingContinuationCount);
	Resources->SetNumberField(TEXT("prepared_continuations"), Snapshot.PreparedContinuationCount);
	Resources->SetNumberField(TEXT("prepared_subscriptions"), Snapshot.PreparedDelegateSubscriptionCount);
}

bool FUiSaveObservation::Refresh(const FString& ExpectedPackage, bool bRequireStable, FString& Error)
{
	bCaptured = false;
	Snapshot = {};
	BackendInfo = {};
	BoundButtons = 0;
	CaptureValues();
	if (!IsInGameThread()) { Error = TEXT("probe_requires_game_thread"); return false; }
	if (!GEngine) { return false; }
	UWorld* FoundWorld = nullptr;
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		UWorld* Candidate = Context.World();
		if (!Candidate || Candidate->WorldType != EWorldType::Game || !Candidate->HasBegunPlay()) { continue; }
		Map = Candidate->GetOutermost()->GetName();
		if (Map != UiSaveMap) { Error = TEXT("unexpected_game_map"); return false; }
		if (FoundWorld) { Error = TEXT("ambiguous_game_world"); return false; }
		FoundWorld = Candidate;
	}
	if (!FoundWorld)
	{
		if (bRequireStable) { Error = TEXT("active_world_lost"); }
		return false;
	}
	UAvidScriptWorldSubsystem* Subsystem = FoundWorld->GetSubsystem<UAvidScriptWorldSubsystem>();
	if (!Subsystem) { Error = TEXT("startup_subsystem_missing"); return false; }
	const auto& WorldStats = Subsystem->GetRuntimeStats();
	Startup->SetBoolField(TEXT("active"), WorldStats.bStartupScenarioActive);
	Startup->SetStringField(TEXT("scenario_id"), WorldStats.StartupScenarioId);
	Startup->SetStringField(TEXT("error_category"), WorldStats.LastErrorCategory);
	Startup->SetStringField(TEXT("error_message"), WorldStats.LastErrorMessage);
	if (!WorldStats.LastErrorCategory.IsEmpty() || !WorldStats.LastErrorMessage.IsEmpty())
	{
		Error = TEXT("startup_activation_failed"); return false;
	}
	if (!WorldStats.bStartupScenarioActive)
	{
		if (bRequireStable) { Error = TEXT("startup_became_inactive"); }
		return false;
	}
	if (WorldStats.StartupScenarioId != TEXT("ui_save_demo") || WorldStats.StartupBindingCount != 1
		|| WorldStats.StartupComponentCount != 1 || WorldStats.StartupOwnedActorCount != 0)
	{
		Error = TEXT("startup_identity_or_count_mismatch"); return false;
	}
	AActor* FoundHost = nullptr;
	UAvidScriptComponent* FoundComponent = nullptr;
	for (TActorIterator<AActor> It(FoundWorld); It; ++It)
	{
		if (It->GetClass()->GetPathName() == TEXT("/AvidScript/Demos/UiSave/BP_UiSaveHost.BP_UiSaveHost_C"))
		{
			if (FoundHost) { Error = TEXT("ambiguous_ui_host"); return false; }
			FoundHost = *It;
		}
		TArray<UAvidScriptComponent*> Components;
		It->GetComponents(Components);
		for (UAvidScriptComponent* Candidate : Components)
		{
			if (Candidate->GetScriptModuleId() != FName(UiSaveModule) && Candidate->GetRuntimeStats().ModuleId != UiSaveModule) { continue; }
			if (FoundComponent) { Error = TEXT("ambiguous_ui_module"); return false; }
			FoundComponent = Candidate;
		}
	}
	if (!FoundHost || !FoundComponent || FoundComponent->GetOwner() != FoundHost)
	{
		Error = TEXT("unique_ui_host_component_missing"); return false;
	}
	const auto& Stats = FoundComponent->GetRuntimeStats();
	Runtime->SetStringField(TEXT("module_id"), Stats.ModuleId);
	Runtime->SetStringField(TEXT("package_id"), Stats.PackageId);
	Runtime->SetBoolField(TEXT("resolved_from_package"), Stats.bResolvedFromPackage);
	Runtime->SetBoolField(TEXT("runtime_loaded"), Stats.bRuntimeLoaded);
	Runtime->SetBoolField(TEXT("begin_play"), Stats.bBeginPlayCalled);
	Runtime->SetBoolField(TEXT("owner_registered"), Stats.bOwnerRegistered);
	Runtime->SetBoolField(TEXT("owner_handle_valid"), Stats.OwnerHandle.IsValid());
	Runtime->SetStringField(TEXT("error_message"), Stats.LastErrorMessage);
	Runtime->SetNumberField(TEXT("events"), Stats.EventCallbackCount);
	Runtime->SetNumberField(TEXT("dropped_events"), Stats.DroppedGameplayEventCount);
	Events = Stats.EventCallbackCount;
	if (!Stats.LastErrorMessage.IsEmpty() || !Stats.bRuntimeLoaded || !Stats.bBeginPlayCalled)
	{
		Error = TEXT("runtime_activation_failed"); return false;
	}
	if (Stats.ModuleId != UiSaveModule || Stats.PackageId != ExpectedPackage || !Stats.bResolvedFromPackage)
	{
		Error = TEXT("runtime_package_identity_mismatch"); return false;
	}
	if (!Stats.bOwnerRegistered || !Stats.OwnerHandle.IsValid() || Stats.DroppedGameplayEventCount != 0)
	{
		Error = TEXT("runtime_owner_or_event_integrity_failed"); return false;
	}
	UUserWidget* FoundWidget = ObservationPrivate::ReadObject<UUserWidget>(FoundHost, TEXT("RootWidget"));
	if (!FoundWidget || !FoundWidget->IsInViewport())
	{
		if (bRequireStable) { Error = TEXT("root_widget_lost"); }
		return false;
	}
	if (bRequireStable && (World.Get() != FoundWorld || Host.Get() != FoundHost
		|| Component.Get() != FoundComponent || Widget.Get() != FoundWidget))
	{
		Error = TEXT("active_ui_identity_changed"); return false;
	}
	World = FoundWorld;
	Host = FoundHost;
	Component = FoundComponent;
	Widget = FoundWidget;
	UTextBlock* ScoreWidget = ObservationPrivate::ReadObject<UTextBlock>(FoundWidget, TEXT("ScoreText"));
	UTextBlock* StatusWidget = ObservationPrivate::ReadObject<UTextBlock>(FoundWidget, TEXT("StatusText"));
	if (!ScoreWidget || !StatusWidget) { Error = TEXT("ui_text_contract_invalid"); return false; }
	Score = ScoreWidget->GetText().ToString();
	Status = StatusWidget->GetText().ToString();
	if (!FoundComponent->CaptureRuntimeDiagnostics(Snapshot, BackendInfo)) { return false; }
	bCaptured = true;
	for (const TCHAR* Name : { TEXT("CollectButton"), TEXT("SaveButton"), TEXT("LoadButton"), TEXT("ResetButton") })
	{
		const UButton* Button = FindButton(Name);
		if (Button && Button->OnClicked.IsBound()) { ++BoundButtons; }
	}
	CaptureValues();
	if (!Snapshot.bHasActiveRuntime || Snapshot.bFaultQuarantined || !Snapshot.FaultDiagnostic.IsEmpty()
		|| Snapshot.ModuleId != UiSaveModule)
	{
		Error = TEXT("runtime_diagnostics_invalid"); return false;
	}
	if (BackendInfo.Kind != EAvidScriptVmBackendKind::Wasmtime || BackendInfo.ExecutionMode != EAvidScriptVmExecutionMode::Aot
		|| BackendInfo.ArtifactFormat != EAvidScriptVmArtifactFormat::WasmtimeSerialized)
	{
		Error = TEXT("actual_backend_requires_wasmtime_aot_serialized"); return false;
	}
	return true;
}

bool FUiSaveObservation::CheckResources(FString& Error) const
{
	if (!bCaptured || Snapshot.ActiveDelegateSubscriptionCount != 4 || BoundButtons != 4
		|| Snapshot.OwnedObjectEntryCount != 0 || Snapshot.BorrowedHandleEntryCount < 7 || Snapshot.BorrowedHandleEntryCount > 8
		|| Snapshot.PendingTimerCount != 0 || Snapshot.PendingContinuationCount != 0
		|| Snapshot.PreparedContinuationCount != 0 || Snapshot.PreparedDelegateSubscriptionCount != 0)
	{
		Error = TEXT("resource_counts_mismatch"); return false;
	}
	return true;
}

bool FUiSaveObservation::CheckEvents(int32 Expected, FString& Error) const
{
	if (!bCaptured || Events != Expected || Snapshot.EventCallbackCount != Expected)
	{
		Error = TEXT("event_count_mismatch"); return false;
	}
	return true;
}

UButton* FUiSaveObservation::FindButton(FName Name) const
{
	return ObservationPrivate::ReadObject<UButton>(Widget.Get(), Name);
}
}
