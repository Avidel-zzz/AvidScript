#include "Demos/AvidScriptUiSaveDemoEdges.h"
#include "Demos/AvidScriptUiSaveDemoObservation.h"

#include "AvidScriptComponent.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "GameFramework/Actor.h"
#include "UObject/UnrealType.h"

namespace AvidScript::UiSaveDemo
{
namespace
{
bool IsInvalidLoad(const FProbeStep& Step)
{
	return Step.Action == TEXT("load") && Step.Status != TEXT("Loaded");
}
}

FUiSaveEdges::FUiSaveEdges(const FString& SavePath, TFunction<bool()> CheckSafe)
	: Fixtures(SavePath, MoveTemp(CheckSafe))
{
	Report->SetBoolField(TEXT("reset_preserved_save"), false);
	Report->SetNumberField(TEXT("invalid_loads_preserved_count"), 0);
	Report->SetBoolField(TEXT("save_failure_preserved_save"), false);
	Report->SetBoolField(TEXT("save_lock_released"), false);
	Report->SetBoolField(TEXT("late_event_ignored"), false);
}

void FUiSaveEdges::AppendSteps(TArray<FProbeStep>& Steps)
{
	Steps.Append({
		{ TEXT("collect"), TEXT("CollectButton"), TEXT("1"), TEXT("Collected") },
		{ TEXT("save"), TEXT("SaveButton"), TEXT("1"), TEXT("Saved") },
		{ TEXT("reset"), TEXT("ResetButton"), TEXT("0"), TEXT("Reset") },
		{ TEXT("load"), TEXT("LoadButton"), TEXT("1"), TEXT("Loaded") },
		{ TEXT("fixture_wrong_type"), NAME_None, TEXT("1"), TEXT("Loaded") },
		{ TEXT("load"), TEXT("LoadButton"), TEXT("1"), TEXT("Wrong save type") },
		{ TEXT("fixture_negative"), NAME_None, TEXT("1"), TEXT("Wrong save type") },
		{ TEXT("load"), TEXT("LoadButton"), TEXT("1"), TEXT("Invalid saved score") },
		{ TEXT("fixture_overflow"), NAME_None, TEXT("1"), TEXT("Invalid saved score") },
		{ TEXT("load"), TEXT("LoadButton"), TEXT("1"), TEXT("Invalid saved score") },
		{ TEXT("fixture_empty"), NAME_None, TEXT("1"), TEXT("Invalid saved score") },
		{ TEXT("load"), TEXT("LoadButton"), TEXT("1"), TEXT("Load failed") },
		{ TEXT("fixture_valid"), NAME_None, TEXT("1"), TEXT("Load failed") },
		{ TEXT("lock_save"), NAME_None, TEXT("1"), TEXT("Load failed") },
		{ TEXT("save_failed"), TEXT("SaveButton"), TEXT("1"), TEXT("Save failed") },
		{ TEXT("teardown"), NAME_None, TEXT("1"), TEXT("Save failed") },
		{ TEXT("late_collect"), TEXT("CollectButton"), TEXT("1"), TEXT("Save failed") }
	});
}

bool FUiSaveEdges::BeforeStep(const FProbeStep& Step, AActor* Host, UAvidScriptComponent* Component,
	UUserWidget* Widget, FJsonObject& Action, FString& Error)
{
	if (!Fixtures.GetExpectedHash().IsEmpty() && !Fixtures.CheckUnchanged(Error)) { return false; }
	if (Step.Action.StartsWith(TEXT("fixture_")))
	{
		return Fixtures.Prepare(Step.Action.RightChop(8), Error);
	}
	if (Step.Action == TEXT("reset") || Step.Action == TEXT("save_failed"))
	{
		Action.SetStringField(TEXT("save_sha256_before"), Fixtures.GetExpectedHash());
	}
	if (IsInvalidLoad(Step))
	{
		SavedBeforeInvalidLoad = ReadUiObject<UObject>(Host, TEXT("SavedObject"));
		if (!SavedBeforeInvalidLoad.IsValid()) { Error = TEXT("invalid_load_requires_previous_save_object"); return false; }
	}
	if (Step.Action == TEXT("lock_save")) { return Fixtures.LockForSaveFailure(Error); }
	if (Step.Action == TEXT("save_failed") && !Fixtures.IsLocked())
	{
		Error = TEXT("save_failure_requires_owned_file_lock"); return false;
	}
	if (Step.Action == TEXT("teardown"))
	{
		EventsBeforeTeardown = Component->GetRuntimeStats().EventCallbackCount;
		if (EventsBeforeTeardown != 9 || Fixtures.IsLocked())
		{
			Error = TEXT("teardown_requires_completed_edges_and_released_lock"); return false;
		}
		// Exercise the public component lifecycle without ending the world that owns the observed UI.
		Component->EndPlay(EEndPlayReason::RemovedFromWorld);
		Component->UnregisterComponent();
		bStopped = true;
		return RefreshStopped(Host, Component, Widget, Error);
	}
	if (Step.Action == TEXT("late_collect"))
	{
		if (!RefreshStopped(Host, Component, Widget, Error)) { return false; }
		EventsBeforeLate = Component->GetRuntimeStats().EventCallbackCount;
		Report->SetNumberField(TEXT("late_event_events_before"), EventsBeforeLate);
	}
	return true;
}

bool FUiSaveEdges::AfterStep(const FProbeStep& Step, AActor* Host, UAvidScriptComponent* Component,
	UUserWidget* Widget, FJsonObject& Action, FString& Error)
{
	if (Step.Action == TEXT("save"))
	{
		if (!Fixtures.ObserveScriptSave(Error)) { return false; }
		Report->SetStringField(TEXT("script_save_sha256"), Fixtures.GetExpectedHash());
	}
	if (!Fixtures.GetExpectedHash().IsEmpty() && !Fixtures.CheckUnchanged(Error)) { return false; }
	if (Step.Action == TEXT("reset") || Step.Action == TEXT("save_failed"))
	{
		Action.SetStringField(TEXT("save_sha256_after"), Fixtures.GetExpectedHash());
		Report->SetBoolField(Step.Action == TEXT("reset") ? TEXT("reset_preserved_save")
			: TEXT("save_failure_preserved_save"), true);
	}
	if (IsInvalidLoad(Step))
	{
		if (!SavedBeforeInvalidLoad.IsValid() || ReadUiObject<UObject>(Host, TEXT("SavedObject")) != SavedBeforeInvalidLoad.Get())
		{
			Error = TEXT("invalid_load_replaced_previous_save_object"); return false;
		}
		Action.SetBoolField(TEXT("saved_object_preserved"), true);
		Report->SetNumberField(TEXT("invalid_loads_preserved_count"), ++InvalidLoadsPreserved);
	}
	if (Step.Action == TEXT("save_failed"))
	{
		Fixtures.Unlock();
		if (!Fixtures.CheckUnchanged(Error)) { return false; }
		Report->SetBoolField(TEXT("save_lock_released"), !Fixtures.IsLocked());
	}
	if (Step.Action == TEXT("teardown") || Step.Action == TEXT("late_collect"))
	{
		if (!RefreshStopped(Host, Component, Widget, Error)) { return false; }
		if (Step.Action == TEXT("late_collect"))
		{
			const int32 EventsAfter = Component->GetRuntimeStats().EventCallbackCount;
			Report->SetNumberField(TEXT("late_event_events_after"), EventsAfter);
			if (EventsAfter != EventsBeforeLate) { Error = TEXT("late_event_entered_stopped_guest"); return false; }
			Report->SetBoolField(TEXT("late_event_ignored"), true);
		}
	}
	return true;
}

bool FUiSaveEdges::RefreshStopped(AActor* Host, UAvidScriptComponent* Component, UUserWidget* Widget, FString& Error)
{
	if (!bStopped || !IsValid(Host) || !IsValid(Component) || !IsValid(Widget)
		|| Component->GetOwner() != Host || ReadUiObject<UUserWidget>(Host, TEXT("RootWidget")) != Widget)
	{
		Error = TEXT("stopped_ui_identity_changed"); return false;
	}
	const FAvidScriptComponentRuntimeStats& Stats = Component->GetRuntimeStats();
	AActor* ResolvedOwner = nullptr;
	FAvidScriptObjectHandleResult OwnerResult;
	const bool bOwnerResolves = Component->ResolveOwnerActor(ResolvedOwner, OwnerResult);
	const bool bSessionPresent = Component->GetRuntimeSessionForEditorDebugging() != nullptr;
	int32 BoundButtons = 0;
	for (const FName Name : { FName(TEXT("CollectButton")), FName(TEXT("SaveButton")),
		FName(TEXT("LoadButton")), FName(TEXT("ResetButton")) })
	{
		UButton* Button = ReadUiObject<UButton>(Widget, Name);
		if (!Button) { Error = TEXT("stopped_button_missing"); return false; }
		BoundButtons += Button->OnClicked.IsBound() ? 1 : 0;
	}
	const bool bSavedObjectPresent = ReadUiObject<UObject>(Host, TEXT("SavedObject")) != nullptr;
	const TSharedRef<FJsonObject> Teardown = MakeShared<FJsonObject>();
	Teardown->SetStringField(TEXT("kind"), TEXT("component_end_play"));
	Teardown->SetBoolField(TEXT("component_end_play"), Stats.bComponentEndPlayObserved);
	Teardown->SetBoolField(TEXT("guest_end_play"), Stats.bEndPlayCalled);
	Teardown->SetBoolField(TEXT("runtime_loaded"), Stats.bRuntimeLoaded);
	Teardown->SetBoolField(TEXT("owner_released"), Stats.bOwnerReleased);
	Teardown->SetBoolField(TEXT("owner_resolves"), bOwnerResolves);
	Teardown->SetBoolField(TEXT("session_present"), bSessionPresent);
	Teardown->SetBoolField(TEXT("widget_in_viewport"), Widget->IsInViewport());
	Teardown->SetBoolField(TEXT("saved_object_present"), bSavedObjectPresent);
	Teardown->SetNumberField(TEXT("bound_buttons"), BoundButtons);
	Teardown->SetNumberField(TEXT("events_before"), EventsBeforeTeardown);
	Teardown->SetNumberField(TEXT("events_after"), Stats.EventCallbackCount);
	Teardown->SetNumberField(TEXT("dropped_events"), Stats.DroppedGameplayEventCount);
	Teardown->SetStringField(TEXT("error_message"), Stats.LastErrorMessage);
	Report->SetObjectField(TEXT("teardown"), Teardown);
	if (!Stats.bComponentEndPlayObserved || !Stats.bEndPlayCalled || Stats.bRuntimeLoaded
		|| !Stats.bOwnerReleased || bOwnerResolves || bSessionPresent || Widget->IsInViewport()
		|| bSavedObjectPresent || BoundButtons != 0 || Stats.bCollisionDelegatesBound || Component->IsRegistered()
		|| Stats.EventCallbackCount != EventsBeforeTeardown || Stats.DroppedGameplayEventCount != 0
		|| !Stats.LastErrorMessage.IsEmpty())
	{
		Error = TEXT("component_teardown_did_not_release_ui_session"); return false;
	}
	return true;
}

TSharedRef<FJsonObject> FUiSaveEdges::GetReport()
{
	Report->SetArrayField(TEXT("fixtures"), Fixtures.GetEvidence());
	return Report;
}
}
