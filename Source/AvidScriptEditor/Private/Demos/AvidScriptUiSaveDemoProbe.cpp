#include "Demos/AvidScriptUiSaveDemoProbe.h"
#include "Demos/AvidScriptUiSaveDemoEdges.h"
#include "Demos/AvidScriptUiSaveDemoReload.h"
#include "Demos/AvidScriptUiSaveDemoObservation.h"

#include "AvidScriptComponent.h"
#include "AvidScriptHash.h"
#include "AvidScriptWorldSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Containers/Ticker.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptUiSaveProbe, Log, All);

namespace AvidScript::UiSaveDemo
{
namespace
{
constexpr TCHAR MapPath[] = TEXT("/AvidScript/Demos/UiSave/L_UiSave");
constexpr TCHAR HostClassPath[] = TEXT("/AvidScript/Demos/UiSave/BP_UiSaveHost.BP_UiSaveHost_C");
constexpr TCHAR ModuleId[] = TEXT("avidscript.ui_save_demo");
constexpr TCHAR SaveName[] = TEXT("AvidScript_UiSaveDemo_v1.sav");
constexpr double TimeoutSeconds = 30.0;

FString FullPath(const FString& Path)
{
	FString Result = FPaths::ConvertRelativePathToFull(Path);
	FPaths::CollapseRelativeDirectories(Result);
	FPaths::NormalizeFilename(Result);
	FPaths::RemoveDuplicateSlashes(Result);
	Result.RemoveFromEnd(TEXT("/"));
	return Result;
}

bool IsHexIdentity(const FString& Value)
{
	if (Value.Len() != 64) { return false; }
	for (TCHAR Character : Value)
	{
		if (!((Character >= TEXT('0') && Character <= TEXT('9'))
			|| (Character >= TEXT('a') && Character <= TEXT('f'))
			|| (Character >= TEXT('A') && Character <= TEXT('F')))) { return false; }
	}
	return true;
}

bool HasSafeParents(const FString& Path)
{
	IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
	FString Current = Path;
	while (!Current.IsEmpty())
	{
		if ((Files.DirectoryExists(*Current) || Files.FileExists(*Current))
			&& Files.IsSymlink(*Current) != ESymlinkResult::NonSymlink) { return false; }
		const FString Parent = FPaths::GetPath(Current);
		if (Parent == Current) { break; }
		Current = Parent;
	}
	return true;
}

class FProbe
{
public:
	FTSTicker::FDelegateHandle Ticker;
	FString Mode;
	FString ReportPath;
	FString ExpectedPackage;
	FString UserRoot;
	FString SavePath;
	FString InitialSaveHash;
	FString FinalSaveHash;
	int32 FinalSaveBytes = 0;
	FString InitializationError;
	FString LastScore;
	FString LastStatus;
	FString ActualMap;
	FString StartedUtc = FDateTime::UtcNow().ToIso8601();
	double Started = FPlatformTime::Seconds();
	double StepStarted = 0.0;
	int32 StepIndex = 0;
	bool bDispatched = false;
	bool bFinished = false;
	bool bReportAllowed = false;
	bool bGcPerformed = false;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AActor> Host;
	TWeakObjectPtr<UAvidScriptComponent> Component;
	TWeakObjectPtr<UUserWidget> Widget;
	TWeakObjectPtr<UObject> SavedBeforeGc;
	TArray<FProbeStep> Steps;
	TUniquePtr<FUiSaveEdges> Edges;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TSharedRef<FJsonObject> Runtime = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Startup = MakeShared<FJsonObject>();
	TUniquePtr<FUiSaveReload> Reload;

	void Initialize()
	{
		const TCHAR* Command = FCommandLine::Get();
		FParse::Value(Command, TEXT("AvidScriptUiSaveProbe="), Mode);
		FParse::Value(Command, TEXT("AvidScriptUiSaveReport="), ReportPath);
		FParse::Value(Command, TEXT("AvidScriptUiSaveExpectedPackage="), ExpectedPackage);
		FParse::Value(Command, TEXT("UserDir="), UserRoot);
		if (!ReportPath.IsEmpty() && !FPaths::IsRelative(ReportPath)
			&& FPaths::GetExtension(ReportPath).Equals(TEXT("json"), ESearchCase::IgnoreCase))
		{
			ReportPath = FullPath(ReportPath);
			bReportAllowed = !IFileManager::Get().FileExists(*ReportPath)
				&& !IFileManager::Get().DirectoryExists(*ReportPath) && HasSafeParents(ReportPath);
		}
		if (!bReportAllowed) { InitializationError = TEXT("report_requires_unique_absolute_new_json"); return; }
		if (!FParse::Param(Command, TEXT("game")) || IsRunningCommandlet())
		{
			InitializationError = TEXT("probe_requires_editor_binary_game_process"); return;
		}
		FString Scenario;
		FParse::Value(Command, TEXT("AvidScriptScenario="), Scenario);
		if (Scenario != TEXT("ui_save_demo")) { InitializationError = TEXT("scenario_must_be_ui_save_demo"); return; }
		if (Mode != TEXT("write") && Mode != TEXT("read") && Mode != TEXT("missing")
			&& Mode != TEXT("gc") && Mode != TEXT("edges") && Mode != TEXT("reload"))
		{
			InitializationError = TEXT("unknown_ui_save_probe_mode"); return;
		}
		if (!IsHexIdentity(ExpectedPackage)) { InitializationError = TEXT("expected_package_requires_64hex"); return; }
		ExpectedPackage.ToLowerInline();
		if (UserRoot.IsEmpty() || FPaths::IsRelative(UserRoot))
		{
			InitializationError = TEXT("explicit_absolute_isolated_UserDir_required"); return;
		}
		UserRoot = FullPath(UserRoot);
		const FString Project = FullPath(FPaths::ProjectDir());
		const FString Engine = FullPath(FPaths::EngineDir());
		if (UserRoot == Project || UserRoot == Engine || FPaths::IsUnderDirectory(UserRoot, Project)
			|| FPaths::IsUnderDirectory(Project, UserRoot) || FPaths::IsUnderDirectory(UserRoot, Engine)
			|| FPaths::IsUnderDirectory(Engine, UserRoot)
			|| FullPath(FPaths::ProjectSavedDir()) != FullPath(UserRoot / TEXT("Saved")))
		{
			InitializationError = TEXT("UserDir_does_not_isolate_effective_Saved_directory"); return;
		}
		SavePath = FullPath(UserRoot / TEXT("Saved/SaveGames") / SaveName);
		if (!CheckSaveDirectory()) { InitializationError = TEXT("save_directory_is_not_isolated_or_contains_other_files"); return; }
		const bool bExists = IFileManager::Get().FileExists(*SavePath);
		if (Mode != TEXT("read") && Mode != TEXT("gc") && bExists)
		{
			InitializationError = TEXT("probe_requires_a_new_save_directory"); return;
		}
		if (Mode == TEXT("read") || Mode == TEXT("gc"))
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *SavePath) || Bytes.IsEmpty())
			{
				InitializationError = TEXT("read_and_gc_require_previous_process_save"); return;
			}
			InitialSaveHash = FAvidScriptHash::Sha256Hex(Bytes);
		}
		Steps.Add({ TEXT("ready"), NAME_None, TEXT("0"), TEXT("Ready") });
		if (Mode == TEXT("reload"))
		{
			Reload = MakeUnique<FUiSaveReload>();
			if (!Reload->Initialize(InitializationError)) { return; }
			Reload->AppendSteps(Steps);
		}
		else if (Mode == TEXT("edges"))
		{
			Edges = MakeUnique<FUiSaveEdges>(SavePath, [this]() { return CheckSaveDirectory(); });
			FUiSaveEdges::AppendSteps(Steps);
		}
		else if (Mode == TEXT("write"))
		{
			for (int32 Score = 1; Score <= 3; ++Score)
			{
				Steps.Add({ TEXT("collect"), TEXT("CollectButton"), FString::FromInt(Score), TEXT("Collected") });
			}
			Steps.Add({ TEXT("save"), TEXT("SaveButton"), TEXT("3"), TEXT("Saved") });
		}
		else
		{
			Steps.Add({ TEXT("load"), TEXT("LoadButton"), Mode == TEXT("missing") ? TEXT("0") : TEXT("3"),
				Mode == TEXT("missing") ? TEXT("No saved score") : TEXT("Loaded") });
			if (Mode == TEXT("gc"))
			{
				Steps.Add({ TEXT("collect_garbage"), NAME_None, TEXT("3"), TEXT("Loaded") });
				Steps.Add({ TEXT("collect"), TEXT("CollectButton"), TEXT("4"), TEXT("Collected") });
			}
		}
	}

	bool CheckSaveDirectory() const
	{
		if (SavePath.IsEmpty() || !HasSafeParents(SavePath)) { return false; }
		IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
		const FString Directory = FPaths::GetPath(SavePath);
		if (!Files.DirectoryExists(*Directory)) { return true; }
		bool bSafe = true;
		const bool bVisited = Files.IterateDirectory(*Directory, [&bSafe, this](const TCHAR* Filename, bool bDirectory)
		{
			bSafe = !bDirectory && FullPath(Filename) == SavePath && HasSafeParents(Filename);
			return bSafe;
		});
		return bVisited && bSafe;
	}

	void Finish(bool bSucceeded, const FString& Failure, bool bExit = true)
	{
		if (bFinished) { return; }
		bFinished = true;
		if (Edges) { Edges->Unlock(); }
		FString FinalFailure = Failure;
		if (Reload)
		{
			FString ReloadError;
			if (!Reload->Finish(ReloadError))
			{
				bSucceeded = false;
				if (FinalFailure.IsEmpty()) { FinalFailure = ReloadError; }
			}
		}
		const TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
		Report->SetNumberField(TEXT("schema_version"), 1);
		Report->SetStringField(TEXT("result"), bSucceeded ? TEXT("avidscript_ui_save_probe_passed") : TEXT("avidscript_ui_save_probe_failed"));
		Report->SetBoolField(TEXT("succeeded"), bSucceeded);
		Report->SetStringField(TEXT("failure_category"), FinalFailure);
		Report->SetStringField(TEXT("mode"), Mode);
		Report->SetStringField(TEXT("process_mode"), IsRunningCommandlet() ? TEXT("commandlet")
			: FParse::Param(FCommandLine::Get(), TEXT("game")) ? TEXT("editor_binary_game") : TEXT("editor"));
		Report->SetNumberField(TEXT("process_id"), FPlatformProcess::GetCurrentProcessId());
		Report->SetStringField(TEXT("input_kind"), TEXT("synthetic_ue_button_onclicked_broadcast"));
		Report->SetBoolField(TEXT("physical_click_verified"), false);
		Report->SetBoolField(TEXT("visual_verified"), false);
		Report->SetBoolField(TEXT("long_run_verified"), false);
		Report->SetStringField(TEXT("expected_module_id"), ModuleId);
		Report->SetStringField(TEXT("expected_package_id"), ExpectedPackage);
		Report->SetStringField(TEXT("map"), ActualMap);
		Report->SetStringField(TEXT("user_dir"), UserRoot);
		Report->SetStringField(TEXT("save_path"), SavePath);
		Report->SetStringField(TEXT("initial_save_sha256"), InitialSaveHash);
		Report->SetStringField(TEXT("save_file_sha256"), FinalSaveHash);
		Report->SetNumberField(TEXT("save_file_bytes"), FinalSaveBytes);
		Report->SetStringField(TEXT("score_text"), LastScore);
		Report->SetStringField(TEXT("status_text"), LastStatus);
		Report->SetStringField(TEXT("started_utc"), StartedUtc);
		Report->SetStringField(TEXT("finished_utc"), FDateTime::UtcNow().ToIso8601());
		Report->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
		Report->SetNumberField(TEXT("timeout_seconds"), Reload ? Reload->GetTimeoutSeconds() : TimeoutSeconds);
		Report->SetBoolField(TEXT("gc_performed"), bGcPerformed);
		Report->SetObjectField(TEXT("runtime"), Runtime);
		Report->SetStringField(TEXT("runtime_snapshot_phase"), (Edges && Edges->IsStopped()) || (Reload && Reload->IsStopped())
			? TEXT("before_teardown") : TEXT("final_active"));
		if (Edges) { Report->SetObjectField(TEXT("edges"), Edges->GetReport()); }
		if (Reload) { Report->SetObjectField(TEXT("reload"), Reload->GetReport()); }
		Report->SetObjectField(TEXT("startup"), Startup);
		Report->SetArrayField(TEXT("actions"), Actions);
		Report->SetBoolField(TEXT("save_file_exists"), !SavePath.IsEmpty() && IFileManager::Get().FileExists(*SavePath));
		FString Json;
		auto Serialize = [&]()
		{
			Json.Reset();
			return FJsonSerializer::Serialize(Report,
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
		};
		const bool bWritten = Serialize() && bReportAllowed && HasSafeParents(ReportPath)
			&& IFileManager::Get().MakeDirectory(*FPaths::GetPath(ReportPath), true)
			&& FFileHelper::SaveStringToFile(Json, *ReportPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
				&IFileManager::Get(), FILEWRITE_NoReplaceExisting);
		if (!bWritten)
		{
			bSucceeded = false;
			Report->SetBoolField(TEXT("succeeded"), false);
			Report->SetStringField(TEXT("result"), TEXT("avidscript_ui_save_probe_failed"));
			Report->SetStringField(TEXT("report_error"), TEXT("new_report_could_not_be_written_original_path_not_overwritten"));
			Serialize();
		}
		UE_LOG(LogAvidScriptUiSaveProbe, Display, TEXT("AVIDSCRIPT_UI_SAVE_PROBE_RESULT %s"), *Json);
		if (bExit) { FPlatformMisc::RequestExitWithStatus(false, bSucceeded ? 0 : 1); }
	}

	bool RefreshRuntime()
	{
		if (Reload && Reload->IsStopped())
		{
			FString Error;
			if (!Reload->RefreshStopped(Host.Get(), Component.Get(), Widget.Get(), Error)) { Finish(false, Error); return false; }
			return ReadUiText();
		}
		if (Edges && Edges->IsStopped())
		{
			FString Error;
			if (!Edges->RefreshStopped(Host.Get(), Component.Get(), Widget.Get(), Error))
			{
				Finish(false, Error); return false;
			}
			return ReadUiText();
		}
		if (!GEngine) { return false; }
		UWorld* FoundWorld = nullptr;
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* Candidate = Context.World();
			if (Candidate && Candidate->WorldType == EWorldType::Game && Candidate->GetOutermost()->GetName() == MapPath)
			{
				if (FoundWorld) { Finish(false, TEXT("ambiguous_game_world")); return false; }
				FoundWorld = Candidate;
			}
		}
		if (!FoundWorld || !FoundWorld->HasBegunPlay()) { return false; }
		World = FoundWorld;
		ActualMap = FoundWorld->GetOutermost()->GetName();
		if (UAvidScriptWorldSubsystem* Subsystem = FoundWorld->GetSubsystem<UAvidScriptWorldSubsystem>())
		{
			const FAvidScriptWorldRuntimeStats& Stats = Subsystem->GetRuntimeStats();
			Startup->SetBoolField(TEXT("active"), Stats.bStartupScenarioActive);
			Startup->SetStringField(TEXT("scenario_id"), Stats.StartupScenarioId);
			Startup->SetStringField(TEXT("error_category"), Stats.LastErrorCategory);
			Startup->SetStringField(TEXT("error_message"), Stats.LastErrorMessage);
			if (!Stats.LastErrorCategory.IsEmpty() || !Stats.LastErrorMessage.IsEmpty())
			{
				Finish(false, TEXT("runtime_startup_activation_failed")); return false;
			}
		}
		AActor* FoundHost = nullptr;
		for (TActorIterator<AActor> It(FoundWorld); It; ++It)
		{
			if (It->GetClass()->GetPathName() == HostClassPath)
			{
				if (FoundHost) { Finish(false, TEXT("ambiguous_ui_host")); return false; }
				FoundHost = *It;
			}
		}
		if (!FoundHost) { return false; }
		TArray<UAvidScriptComponent*> Components;
		FoundHost->GetComponents(Components);
		UAvidScriptComponent* FoundComponent = nullptr;
		for (UAvidScriptComponent* Candidate : Components)
		{
			if ((Reload && Component.IsValid()) ? Candidate == Component.Get() : Candidate->GetScriptModuleId() == FName(ModuleId))
			{
				if (FoundComponent) { Finish(false, TEXT("ambiguous_ui_module")); return false; }
				FoundComponent = Candidate;
			}
		}
		if (!FoundComponent) { return false; }
		const FAvidScriptComponentRuntimeStats& Stats = FoundComponent->GetRuntimeStats();
		Runtime->SetStringField(TEXT("module_id"), Stats.ModuleId);
		Runtime->SetStringField(TEXT("package_id"), Stats.PackageId);
		Runtime->SetStringField(TEXT("script_manifest_path"), Stats.ScriptManifestPath);
		Runtime->SetBoolField(TEXT("resolved_from_package"), Stats.bResolvedFromPackage);
		Runtime->SetBoolField(TEXT("runtime_loaded"), Stats.bRuntimeLoaded);
		Runtime->SetBoolField(TEXT("begin_play"), Stats.bBeginPlayCalled);
		Runtime->SetBoolField(TEXT("owner_registered"), Stats.bOwnerRegistered);
		Runtime->SetBoolField(TEXT("owner_handle_valid"), Stats.OwnerHandle.IsValid());
		Runtime->SetStringField(TEXT("owner"), FoundHost->GetPathName());
		Runtime->SetStringField(TEXT("error_message"), Stats.LastErrorMessage);
		Runtime->SetNumberField(TEXT("ticks"), Stats.TickCallCount);
		Runtime->SetNumberField(TEXT("events"), Stats.EventCallbackCount);
		Runtime->SetNumberField(TEXT("dropped_events"), Stats.DroppedGameplayEventCount);
		if (!Reload && !Stats.LastErrorMessage.IsEmpty()) { Finish(false, TEXT("runtime_activation_failed")); return false; }
		if (!Stats.bRuntimeLoaded || !Stats.bBeginPlayCalled)
		{
			if (StepIndex > 0) { Finish(false, TEXT("runtime_became_inactive")); }
			return false;
		}
		if ((!Reload || !Reload->IsLoose()) && (Stats.ModuleId != ModuleId || Stats.PackageId != ExpectedPackage || !Stats.bResolvedFromPackage))
		{
			Finish(false, TEXT("runtime_package_identity_mismatch")); return false;
		}
		if (Reload)
		{
			FString Error;
			if (!Reload->ValidateRuntime(*FoundComponent, Error)) { Finish(false, Error); return false; }
		}
		if (!Stats.bOwnerRegistered || !Stats.OwnerHandle.IsValid() || Stats.DroppedGameplayEventCount != 0)
		{
			Finish(false, TEXT("runtime_owner_or_event_integrity_failed")); return false;
		}
		UUserWidget* RootWidget = ReadUiObject<UUserWidget>(FoundHost, TEXT("RootWidget"));
		if (!RootWidget || !RootWidget->IsInViewport())
		{
			if (StepIndex > 0) { Finish(false, TEXT("root_widget_lost")); }
			return false;
		}
		if (StepIndex > 0 && (Host.Get() != FoundHost || Component.Get() != FoundComponent || Widget.Get() != RootWidget))
		{
			Finish(false, TEXT("live_ui_or_component_replaced")); return false;
		}
		Host = FoundHost;
		Component = FoundComponent;
		Widget = RootWidget;
		return ReadUiText();
	}

	bool ReadUiText()
	{
		UTextBlock* Score = ReadUiObject<UTextBlock>(Widget.Get(), TEXT("ScoreText"));
		UTextBlock* Status = ReadUiObject<UTextBlock>(Widget.Get(), TEXT("StatusText"));
		if (!Score || !Status) { Finish(false, TEXT("ui_text_contract_invalid")); return false; }
		LastScore = Score->GetText().ToString();
		LastStatus = Status->GetText().ToString();
		return true;
	}

	bool Tick(float DeltaTime)
	{
		static_cast<void>(DeltaTime);
		if (bFinished) { return false; }
		if (!InitializationError.IsEmpty()) { Finish(false, InitializationError); return false; }
		if (FPlatformTime::Seconds() - Started >= (Reload ? Reload->GetTimeoutSeconds() : TimeoutSeconds))
		{
			Finish(false, StepIndex == 0 ? TEXT("runtime_or_ready_timeout") : TEXT("probe_step_timeout")); return false;
		}
		if (!RefreshRuntime()) { return !bFinished; }
		if (!CheckSaveDirectory()) { Finish(false, TEXT("save_directory_safety_changed")); return false; }
		const FProbeStep& Step = Steps[StepIndex];
		if (!bDispatched)
		{
			if (Step.Action == TEXT("ready")
				&& (LastStatus == TEXT("Input unavailable") || LastStatus == TEXT("UI unavailable")))
			{
				Finish(false, TEXT("script_initialization_rejected")); return false;
			}
			if (Step.Action == TEXT("ready") && (LastScore != Step.Score || LastStatus != Step.Status)) { return true; }
			const TSharedRef<FJsonObject> Action = MakeShared<FJsonObject>();
			Action->SetStringField(TEXT("action"), Step.Action);
			Action->SetStringField(TEXT("button"), Step.Button.ToString());
			Action->SetStringField(TEXT("expected_score"), Step.Score);
			Action->SetStringField(TEXT("expected_status"), Step.Status);
			Action->SetStringField(TEXT("before_score"), LastScore);
			Action->SetStringField(TEXT("before_status"), LastStatus);
			Action->SetNumberField(TEXT("dispatch_frame"), static_cast<double>(GFrameCounter));
			Action->SetBoolField(TEXT("synthetic_ue_event"), !Step.Button.IsNone());
			Actions.Add(MakeShared<FJsonValueObject>(Action));
			StepStarted = FPlatformTime::Seconds();
			bDispatched = true;
			FString EdgeError;
			if (Edges && !Edges->BeforeStep(Step, Host.Get(), Component.Get(), Widget.Get(), *Action, EdgeError))
			{
				Finish(false, EdgeError); return false;
			}
			if (Reload && !Reload->BeforeStep(Step, Host.Get(), Component.Get(), Widget.Get(), *Action, EdgeError))
			{
				Finish(false, EdgeError); return false;
			}
			if (Step.Action == TEXT("collect_garbage"))
			{
				SavedBeforeGc = ReadUiObject<UObject>(Host.Get(), TEXT("SavedObject"));
				if (!SavedBeforeGc.IsValid()) { Finish(false, TEXT("saved_object_not_strongly_held_before_gc")); return false; }
				CollectGarbage(RF_NoFlags, true);
				bGcPerformed = true;
			}
			else if (!Step.Button.IsNone())
			{
				if (Step.Action == TEXT("save") && IFileManager::Get().FileExists(*SavePath))
				{
					Finish(false, TEXT("refusing_to_overwrite_existing_save")); return false;
				}
				UButton* Button = ReadUiObject<UButton>(Widget.Get(), Step.Button);
				const bool bRequireSubscription = Step.Action != TEXT("late_collect");
				if (!Button || !Button->GetIsEnabled() || Button->OnClicked.IsBound() != bRequireSubscription)
				{
					Finish(false, TEXT("button_or_csharp_subscription_unavailable")); return false;
				}
				Button->OnClicked.Broadcast();
			}
			return true;
		}
		if (Reload && !Reload->CanObserveStep()) { return true; }
		if (bGcPerformed && (!SavedBeforeGc.IsValid() || ReadUiObject<UObject>(Host.Get(), TEXT("SavedObject")) != SavedBeforeGc.Get()))
		{
			Finish(false, TEXT("saved_object_lost_after_gc")); return false;
		}
		const TSharedPtr<FJsonObject> Action = Actions.Last()->AsObject();
		Action->SetStringField(TEXT("observed_score"), LastScore);
		Action->SetStringField(TEXT("observed_status"), LastStatus);
		Action->SetNumberField(TEXT("check_frame"), static_cast<double>(GFrameCounter));
		if (LastScore != Step.Score || LastStatus != Step.Status)
		{
			if (FPlatformTime::Seconds() - StepStarted > 3.0) { Finish(false, TEXT("csharp_ui_assertion_failed")); return false; }
			return true;
		}
		FString EdgeError;
		if (Edges && !Edges->AfterStep(Step, Host.Get(), Component.Get(), Widget.Get(), *Action, EdgeError))
		{
			Finish(false, EdgeError); return false;
		}
		if (Reload && !Reload->AfterStep(Step, Host.Get(), Component.Get(), Widget.Get(), *Action, EdgeError))
		{
			Finish(false, EdgeError); return false;
		}
		Action->SetBoolField(TEXT("passed"), true);
		++StepIndex;
		bDispatched = false;
		if (StepIndex == Steps.Num())
		{
			TArray<uint8> Bytes;
			const bool bExists = IFileManager::Get().FileExists(*SavePath);
			if (Mode == TEXT("missing") || Mode == TEXT("reload"))
			{
				if (bExists) { Finish(false, TEXT("non_saving_probe_created_a_save")); return false; }
			}
			else if (!FFileHelper::LoadFileToArray(Bytes, *SavePath) || Bytes.IsEmpty())
			{
				Finish(false, TEXT("save_file_missing_or_empty")); return false;
			}
			else if ((Mode == TEXT("read") || Mode == TEXT("gc")) && FAvidScriptHash::Sha256Hex(Bytes) != InitialSaveHash)
			{
				Finish(false, TEXT("read_or_gc_modified_save")); return false;
			}
			else if (Edges && FAvidScriptHash::Sha256Hex(Bytes) != Edges->GetExpectedHash())
			{
				Finish(false, TEXT("edges_save_modified_outside_fixture")); return false;
			}
			FinalSaveBytes = Bytes.Num();
			if (!Bytes.IsEmpty()) { FinalSaveHash = FAvidScriptHash::Sha256Hex(Bytes); }
			Finish(true, FString());
			return false;
		}
		return true;
	}
};

TUniquePtr<FProbe> Probe;
}

bool StartProbe()
{
	FString Mode;
	if (!FParse::Value(FCommandLine::Get(), TEXT("AvidScriptUiSaveProbe="), Mode)
		&& !FParse::Param(FCommandLine::Get(), TEXT("AvidScriptUiSaveProbe"))) { return false; }
	if (Probe) { return true; }
	Probe = MakeUnique<FProbe>();
	Probe->Initialize();
	Probe->Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(Probe.Get(), &FProbe::Tick), 0.1f);
	return true;
}

void StopProbe()
{
	if (!Probe) { return; }
	FTSTicker::GetCoreTicker().RemoveTicker(Probe->Ticker);
	if (!Probe->bFinished) { Probe->Finish(false, TEXT("process_shutdown_before_completion"), false); }
	Probe.Reset();
}
}
