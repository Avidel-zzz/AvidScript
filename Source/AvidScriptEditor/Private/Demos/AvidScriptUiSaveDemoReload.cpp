#include "Demos/AvidScriptUiSaveDemoReload.h"
#include "Demos/AvidScriptUiSaveDemoObservation.h"

#include "AvidScriptComponent.h"
#include "AvidScriptHash.h"
#include "AvidScriptRuntimeSession.h"
#include "AvidScriptWasmReloadTypes.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "CoreGlobals.h"
#include "Dom/JsonObject.h"
#include "GameFramework/Actor.h"
#include "GameFramework/SaveGame.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/UnrealType.h"

namespace AvidScript::UiSaveDemo
{
namespace
{
constexpr TCHAR ReloadModuleId[] = TEXT("avidscript.ui_save_demo");
const TCHAR* ArtifactNames[] = { TEXT("baseline"), TEXT("changed"), TEXT("rejected") };
const TCHAR* ArtifactFlags[] = { TEXT("Baseline"), TEXT("Changed"), TEXT("Rejected") };
const TCHAR* ResourceNames[] = { TEXT("pending_timers"), TEXT("pending_continuations"), TEXT("prepared_continuations"),
	TEXT("active_subscriptions"), TEXT("prepared_subscriptions"), TEXT("owned_entries"), TEXT("borrowed_entries"), TEXT("bound_buttons") };

FString Canonical(const FString& Path)
{
	FString Result = FPaths::ConvertRelativePathToFull(Path);
	FPaths::CollapseRelativeDirectories(Result);
	FPaths::NormalizeFilename(Result);
	FPaths::RemoveDuplicateSlashes(Result);
	Result.RemoveFromEnd(TEXT("/"));
	return Result;
}

bool SafeFile(const FString& Path, bool bRequireSaved)
{
	if (FPaths::IsRelative(Path)) { return false; }
	// ProjectSavedDir is redirected by -UserDir; artifacts belong to the original project.
	if (bRequireSaved && !FPaths::IsUnderDirectory(Canonical(Path), Canonical(FPaths::ProjectDir() / TEXT("Saved")))) { return false; }
	IPlatformFile& Files = FPlatformFileManager::Get().GetPlatformFile();
	if (!Files.FileExists(*Path)) { return false; }
	FString Current = Canonical(Path);
	while (!Current.IsEmpty())
	{
		if ((Files.FileExists(*Current) || Files.DirectoryExists(*Current))
			&& Files.IsSymlink(*Current) != ESymlinkResult::NonSymlink) { return false; }
		const FString Parent = FPaths::GetPath(Current);
		if (Parent == Current) { break; }
		Current = Parent;
	}
	return true;
}

bool ReadHash(const FString& Path, FString& Hash)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.IsEmpty()) { return false; }
	Hash = FAvidScriptHash::Sha256Hex(Bytes);
	return true;
}

int32 CountButtons(UUserWidget& Widget)
{
	int32 Count = 0;
	for (const TCHAR* Name : { TEXT("CollectButton"), TEXT("SaveButton"), TEXT("LoadButton"), TEXT("ResetButton") })
	{
		UButton* Button = ReadUiObject<UButton>(&Widget, FName(Name));
		if (!Button) { return -1; }
		Count += Button->OnClicked.IsBound() ? 1 : 0;
	}
	return Count;
}

bool ReadReloadSavedScore(UObject* Object, int32& Score)
{
	if (!IsValid(Object) || !Object->IsA<USaveGame>()) { return false; }
	const FIntProperty* Property = FindFProperty<FIntProperty>(Object->GetClass(), TEXT("Score"));
	if (!Property) { return false; }
	Score = Property->GetPropertyValue_InContainer(Object);
	return true;
}

TSharedRef<FJsonObject> ReloadResultJson(const FAvidScriptWasmReloadResult& Result, bool bReturned)
{
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetBoolField(TEXT("returned"), bReturned);
	Json->SetBoolField(TEXT("succeeded"), Result.bSucceeded);
	Json->SetBoolField(TEXT("applied"), Result.bReloadApplied);
	Json->SetBoolField(TEXT("state_migrated"), Result.bStateMigrationApplied);
	Json->SetNumberField(TEXT("migrated_slots"), Result.StateMigrationMigratedSlotCount);
	Json->SetBoolField(TEXT("host_effects_attempted"), Result.bHostEffectTransactionAttempted);
	Json->SetBoolField(TEXT("host_effects_committed"), Result.bHostEffectTransactionCommitted);
	Json->SetBoolField(TEXT("host_effects_rollback_attempted"), Result.bHostEffectRollbackAttempted);
	Json->SetBoolField(TEXT("host_effects_rollback_succeeded"), Result.bHostEffectRollbackSucceeded);
	Json->SetBoolField(TEXT("rollback_preserved_runtime"), Result.bRollbackPreservedLiveRuntime);
	Json->SetStringField(TEXT("previous_module_id"), Result.PreviousModuleId);
	Json->SetStringField(TEXT("candidate_module_id"), Result.CandidateModuleId);
	Json->SetStringField(TEXT("active_module_id"), Result.ActiveModuleId);
	Json->SetStringField(TEXT("error_category"), Result.ErrorCategory);
	Json->SetStringField(TEXT("error_message"), Result.ErrorMessage);
	return Json;
}
}

bool FUiSaveReload::Initialize(const FString& InSavePath, FString& Error)
{
	Started = FPlatformTime::Seconds();
	SavePath = InSavePath;
	bWithSaveLoad = FParse::Param(FCommandLine::Get(), TEXT("AvidScriptUiSaveReloadWithSaveLoad"));
	Report->SetBoolField(TEXT("with_save_load"), bWithSaveLoad);
	FString CycleText;
	if (FParse::Value(FCommandLine::Get(), TEXT("AvidScriptUiSaveReloadCycles="), CycleText))
	{
		if (CycleText.IsEmpty() || CycleText.Len() > 3) { Error = TEXT("reload_cycles_invalid"); return false; }
		for (TCHAR C : CycleText) { if (C < TEXT('0') || C > TEXT('9')) { Error = TEXT("reload_cycles_invalid"); return false; } }
		Cycles = FCString::Atoi(*CycleText);
	}
	if (Cycles < 1 || Cycles > 100) { Error = TEXT("reload_cycles_out_of_range"); return false; }
	Report->SetNumberField(TEXT("requested_cycles"), Cycles);
	Report->SetNumberField(TEXT("completed_cycles"), 0);
	Report->SetNumberField(TEXT("successful_reloads"), 0);
	Report->SetNumberField(TEXT("rejected_reloads"), 0);
	Report->SetBoolField(TEXT("configuration_restored"), false);
	Report->SetBoolField(TEXT("artifacts_unchanged"), false);
	Report->SetBoolField(TEXT("instance_pointer_identity_measured"), false);
	Report->SetStringField(TEXT("identity_evidence"), TEXT("manifest_and_wasm_hashes_component_stats_session_identity_reload_result_and_observed_behavior"));
	Report->SetStringField(TEXT("object_count_kind"), TEXT("registered_entries_not_live_gc_memory"));
	Report->SetBoolField(TEXT("gc_memory_measured"), false);
	TSharedRef<FJsonObject> Identities = MakeShared<FJsonObject>();
	for (int32 Index = 0; Index < 3; ++Index)
	{
		FArtifact& Artifact = Artifacts[Index];
		const FString Prefix = FString(TEXT("AvidScriptUiSaveReload")) + ArtifactFlags[Index];
		FString ExpectedManifestHash, ExpectedWasmHash;
		FParse::Value(FCommandLine::Get(), *(Prefix + TEXT("Manifest=")), Artifact.ManifestPath);
		FParse::Value(FCommandLine::Get(), *(Prefix + TEXT("ManifestSha256=")), ExpectedManifestHash);
		FParse::Value(FCommandLine::Get(), *(Prefix + TEXT("WasmSha256=")), ExpectedWasmHash);
		if (!SafeFile(Artifact.ManifestPath, true)) { Error = TEXT("reload_manifest_must_be_safe_project_Saved_file"); return false; }
		Artifact.ManifestPath = Canonical(Artifact.ManifestPath);
		FString Json;
		TSharedPtr<FJsonObject> Root;
		const TSharedPtr<FJsonObject>* Wasm = nullptr;
		FString WasmFile;
		if (!FFileHelper::LoadFileToString(Json, *Artifact.ManifestPath)
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Root) || !Root.IsValid()
			|| !Root->TryGetObjectField(TEXT("wasm"), Wasm) || !(*Wasm)->TryGetStringField(TEXT("file"), WasmFile))
		{
			Error = TEXT("reload_manifest_wasm_reference_invalid"); return false;
		}
		FPaths::NormalizeFilename(WasmFile);
		Artifact.WasmPath = Canonical(WasmFile);
		if (FPaths::IsRelative(WasmFile))
		{
			// Match the existing manifest loader's ordered project/manifest-relative resolution.
			const FString Local = Canonical(FPaths::GetPath(Artifact.ManifestPath) / WasmFile);
			const FString Project = Canonical(FPaths::ProjectDir() / WasmFile);
			const bool bProjectRelative = WasmFile.StartsWith(TEXT("Saved/"), ESearchCase::IgnoreCase)
				|| WasmFile.StartsWith(TEXT("Content/"), ESearchCase::IgnoreCase)
				|| WasmFile.StartsWith(TEXT("Plugins/"), ESearchCase::IgnoreCase);
			const FString First = bProjectRelative ? Project : Local;
			const FString Second = bProjectRelative ? Local : Project;
			Artifact.WasmPath = FPaths::FileExists(First) || !FPaths::FileExists(Second) ? First : Second;
		}
		if (!SafeFile(Artifact.WasmPath, true)) { Error = TEXT("reload_wasm_must_be_safe_project_Saved_file"); return false; }
		FAvidScriptWasmReloadManifest Manifest;
		FAvidScriptWasmReloadManifestLoadResult Load;
		TArray<uint8> Bytes;
		if (!FAvidScriptWasmReloadManifestLoader::LoadFromFile(Artifact.ManifestPath, Manifest, Bytes, Load)
			|| Manifest.ModuleId != ReloadModuleId || Manifest.AbiVersion != 1 || Canonical(Load.ModulePath) != Artifact.WasmPath)
		{
			Error = TEXT("reload_manifest_invalid: ") + Load.ErrorCategory + TEXT(": ") + Load.ErrorMessage; return false;
		}
		Artifact.WasmHash = FAvidScriptHash::Sha256Hex(Bytes);
		if (!ReadHash(Artifact.ManifestPath, Artifact.ManifestHash)
			|| Artifact.ManifestHash != ExpectedManifestHash || Artifact.WasmHash != ExpectedWasmHash)
		{
			Error = TEXT("reload_artifact_runner_pin_mismatch"); return false;
		}
		TSharedRef<FJsonObject> Identity = MakeShared<FJsonObject>();
		Identity->SetStringField(TEXT("manifest_path"), Artifact.ManifestPath);
		Identity->SetStringField(TEXT("manifest_sha256"), Artifact.ManifestHash);
		Identity->SetStringField(TEXT("wasm_path"), Artifact.WasmPath);
		Identity->SetStringField(TEXT("wasm_sha256"), Artifact.WasmHash);
		Identity->SetStringField(TEXT("module_id"), Manifest.ModuleId);
		Identities->SetObjectField(ArtifactNames[Index], Identity);
	}
	Report->SetObjectField(TEXT("artifacts"), Identities);
	if (Artifacts[0].WasmHash == Artifacts[1].WasmHash || Artifacts[0].WasmHash == Artifacts[2].WasmHash
		|| Artifacts[1].WasmHash == Artifacts[2].WasmHash)
	{
		Error = TEXT("reload_requires_three_distinct_wasm_bodies"); return false;
	}
	return CheckArtifacts(Error);
}

bool FUiSaveReload::CheckArtifacts(FString& Error) const
{
	for (const FArtifact& Artifact : Artifacts)
	{
		FString ManifestHash, WasmHash;
		if (!SafeFile(Artifact.ManifestPath, true) || !SafeFile(Artifact.WasmPath, true)
			|| !ReadHash(Artifact.ManifestPath, ManifestHash) || !ReadHash(Artifact.WasmPath, WasmHash)
			|| ManifestHash != Artifact.ManifestHash || WasmHash != Artifact.WasmHash)
		{
			Error = TEXT("reload_pinned_artifact_changed"); return false;
		}
	}
	return true;
}

void FUiSaveReload::AppendSteps(TArray<FProbeStep>& Steps) const
{
	int32 Score = 1;
	Steps.Add({ TEXT("collect"), TEXT("CollectButton"), TEXT("1"), TEXT("Collected") });
	if (bWithSaveLoad) { Steps.Add({ TEXT("save"), TEXT("SaveButton"), TEXT("1"), TEXT("Saved") }); }
	for (int32 Cycle = 1; Cycle <= Cycles; ++Cycle)
	{
		const int32 Delta = Cycle % 2 == 1 ? 2 : 1;
		Steps.Add({ Delta == 2 ? TEXT("reload_changed") : TEXT("reload_baseline"), NAME_None, FString::FromInt(Score), TEXT("Ready") });
		Score += Delta;
		Steps.Add({ TEXT("collect"), TEXT("CollectButton"), FString::FromInt(Score), TEXT("Collected") });
		if (bWithSaveLoad)
		{
			Steps.Add({ TEXT("save"), TEXT("SaveButton"), FString::FromInt(Score), TEXT("Saved") });
			Steps.Add({ TEXT("reset"), TEXT("ResetButton"), TEXT("0"), TEXT("Reset") });
			Steps.Add({ TEXT("load"), TEXT("LoadButton"), FString::FromInt(Score), TEXT("Loaded") });
			Steps.Add({ TEXT("collect_garbage"), NAME_None, FString::FromInt(Score), TEXT("Loaded") });
		}
		Steps.Add({ TEXT("reject"), NAME_None, FString::FromInt(Score), bWithSaveLoad ? TEXT("Loaded") : TEXT("Collected") });
		Score += Delta;
		Steps.Add({ TEXT("collect_after_reject"), TEXT("CollectButton"), FString::FromInt(Score), TEXT("Collected") });
	}
	Steps.Add({ TEXT("teardown"), NAME_None, FString::FromInt(Score), TEXT("Collected") });
	Steps.Add({ TEXT("late_collect"), TEXT("CollectButton"), FString::FromInt(Score), TEXT("Collected") });
}

bool FUiSaveReload::ValidateRuntime(UAvidScriptComponent& Component, FString& Error) const
{
	const auto& Stats = Component.GetRuntimeStats();
	const FAvidScriptRuntimeSession* Session = Component.GetRuntimeSessionForEditorDebugging();
	if (Stats.ModuleId != ReloadModuleId || !Session || (OriginalSession && Session != OriginalSession)
		|| Session->GetSnapshot().bFaultQuarantined || !Session->GetSnapshot().bHasActiveRuntime
		|| Stats.LastErrorMessage != ExpectedRejectionError)
	{
		Error = TEXT("reload_live_session_or_error_changed: ") + Stats.LastErrorMessage; return false;
	}
	if (bLoose && (Stats.bResolvedFromPackage || !Stats.PackageId.IsEmpty()
		|| Canonical(Stats.ScriptManifestPath) != Artifacts[ActiveArtifact].ManifestPath))
	{
		Error = TEXT("reload_loose_runtime_identity_mismatch"); return false;
	}
	return true;
}

bool FUiSaveReload::CaptureResources(UAvidScriptComponent& Component, UUserWidget& Widget,
	TSharedRef<FJsonObject> Destination, bool bEstablishBaseline, FString& Error)
{
	const FAvidScriptRuntimeSession* Session = Component.GetRuntimeSessionForEditorDebugging();
	if (!Session) { Error = TEXT("reload_session_missing"); return false; }
	FAvidScriptWasmSmokeResult Live;
	if (!Session->CaptureLiveSnapshot(Live)) { Error = TEXT("reload_backend_snapshot_unavailable"); return false; }
	const FAvidScriptVmBackendInfo& Info = Live.BackendInfo;
	TSharedRef<FJsonObject> Backend = MakeShared<FJsonObject>();
	Backend->SetBoolField(TEXT("measured"), true);
	Backend->SetStringField(TEXT("source"), TEXT("CaptureLiveSnapshot.BackendInfo"));
	Backend->SetStringField(TEXT("backend_id"), Info.StableBackendId);
	Backend->SetStringField(TEXT("backend_kind"), Info.Kind == EAvidScriptVmBackendKind::Wasmtime ? TEXT("wasmtime") : TEXT("wamr"));
	const TCHAR* ExecutionMode = TEXT("auto");
	switch (Info.ExecutionMode)
	{
	case EAvidScriptVmExecutionMode::Interpreter: ExecutionMode = TEXT("interpreter"); break;
	case EAvidScriptVmExecutionMode::Aot: ExecutionMode = TEXT("aot"); break;
	case EAvidScriptVmExecutionMode::Jit: ExecutionMode = TEXT("jit"); break;
	default: break;
	}
	const TCHAR* ArtifactFormat = TEXT("wasm_bytecode");
	switch (Info.ArtifactFormat)
	{
	case EAvidScriptVmArtifactFormat::WamrAot: ArtifactFormat = TEXT("wamr_aot"); break;
	case EAvidScriptVmArtifactFormat::WasmtimeSerialized: ArtifactFormat = TEXT("wasmtime_serialized"); break;
	default: break;
	}
	Backend->SetStringField(TEXT("execution_mode"), ExecutionMode);
	Backend->SetStringField(TEXT("artifact_format"), ArtifactFormat);
	Backend->SetStringField(TEXT("runtime_version"), Info.RuntimeVersion);
	Backend->SetStringField(TEXT("runtime_build_identity"), Info.RuntimeBuildIdentity);
	Backend->SetStringField(TEXT("runtime_artifact_sha256"), Info.RuntimeArtifactSha256);
	Backend->SetStringField(TEXT("target_triple"), Info.TargetTriple);
	Destination->SetObjectField(TEXT("backend"), Backend);
	const auto Snapshot = Session->GetSnapshot();
	const int32 Counts[] = { Snapshot.PendingTimerCount, Snapshot.PendingContinuationCount, Snapshot.PreparedContinuationCount,
		Snapshot.ActiveDelegateSubscriptionCount, Snapshot.PreparedDelegateSubscriptionCount,
		Snapshot.OwnedObjectEntryCount, Snapshot.BorrowedHandleEntryCount, CountButtons(Widget) };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Counts); ++Index) { Destination->SetNumberField(ResourceNames[Index], Counts[Index]); }
	Destination->SetBoolField(TEXT("session_present"), true);
	Destination->SetBoolField(TEXT("session_preserved"), Session == OriginalSession);
	Destination->SetNumberField(TEXT("session_successful_reloads"), Snapshot.SuccessfulReloadCount);
	Destination->SetNumberField(TEXT("session_rejected_reloads"), Snapshot.RejectedReloadCount);
	if (Counts[0] != 0 || Counts[1] != 0 || Counts[2] != 0 || Counts[3] != 4 || Counts[4] != 0 || Counts[7] != 4
		|| Counts[5] < 0 || Counts[6] < 0 || (bWithSaveLoad && Counts[5] != 0)
		|| Snapshot.bFaultQuarantined || !Snapshot.bHasActiveRuntime)
	{
		Error = TEXT("reload_ready_resources_invalid"); return false;
	}
	if (bEstablishBaseline) { ResourceBaseline = Destination; Report->SetObjectField(TEXT("resources_baseline"), Destination); }
	if (!ResourceBaseline) { Error = TEXT("reload_resource_baseline_missing"); return false; }
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Counts); ++Index)
	{
		const int32 CurrentSaveAllowance = bWithSaveLoad && !bEstablishBaseline && Index == 6 ? 1 : 0;
		if (Counts[Index] > ResourceBaseline->GetNumberField(ResourceNames[Index]) + CurrentSaveAllowance)
		{
			Error = FString(TEXT("reload_resource_growth: ")) + ResourceNames[Index]; return false;
		}
	}
	return true;
}

bool FUiSaveReload::BeforeStep(const FProbeStep& Step, AActor* Host, UAvidScriptComponent* Component,
	UUserWidget* Widget, FJsonObject& Action, FString& Error)
{
	if (!IsValid(Host) || !IsValid(Component) || !IsValid(Widget)) { Error = TEXT("reload_objects_missing"); return false; }
	StepFrame = GFrameCounter;
	if (bWithSaveLoad && !BeforeSaveLoadStep(Step, *Host, Action, Error)) { return false; }
	if (Step.Action == TEXT("ready"))
	{
		OriginalModuleId = Component->GetScriptModuleId();
		OriginalManifestPath = Component->GetScriptManifestPath();
		ConfiguredComponent = Component;
		bHasConfiguration = true;
		OriginalSession = Component->GetRuntimeSessionForEditorDebugging();
		InitialSuccesses = Component->GetRuntimeStats().SuccessfulReloadCount;
		InitialRejections = Component->GetRuntimeStats().RejectedReloadCount;
		FAvidScriptWasmReloadManifest Manifest;
		FAvidScriptWasmReloadManifestLoadResult Load;
		TArray<uint8> Bytes;
		const auto& Stats = Component->GetRuntimeStats();
		if (!SafeFile(Stats.ScriptManifestPath, false)
			|| !FAvidScriptWasmReloadManifestLoader::LoadFromFile(Stats.ScriptManifestPath, Manifest, Bytes, Load)
			|| Manifest.ModuleId != ReloadModuleId || FAvidScriptHash::Sha256Hex(Bytes) != Artifacts[0].WasmHash)
		{
			Error = TEXT("reload_startup_package_is_not_baseline_body"); return false;
		}
		Report->SetStringField(TEXT("startup_package_id"), Stats.PackageId);
		Report->SetStringField(TEXT("startup_wasm_sha256"), FAvidScriptHash::Sha256Hex(Bytes));
		return CaptureResources(*Component, *Widget, MakeShared<FJsonObject>(), true, Error);
	}
	const bool bReject = Step.Action == TEXT("reject");
	if (Step.Action.StartsWith(TEXT("reload_")) || bReject)
	{
		if (!CheckArtifacts(Error)) { return false; }
		if (!bReject)
		{
			++CurrentCycle;
			CycleRecord = MakeShared<FJsonObject>();
			Records.Add(MakeShared<FJsonValueObject>(CycleRecord));
			Report->SetArrayField(TEXT("cycles"), Records);
			CycleRecord->SetNumberField(TEXT("cycle"), CurrentCycle);
			CycleRecord->SetStringField(TEXT("target"), ArtifactNames[CurrentCycle % 2]);
			CycleRecord->SetNumberField(TEXT("delta"), CurrentCycle % 2 == 1 ? 2 : 1);
			CycleRecord->SetStringField(TEXT("before_score"), Action.GetStringField(TEXT("before_score")));
			CycleRecord->SetStringField(TEXT("previous_wasm_sha256"), Artifacts[ActiveArtifact].WasmHash);
			CycleRecord->SetStringField(TEXT("active_wasm_sha256"), Artifacts[CurrentCycle % 2].WasmHash);
		}
		const int32 Candidate = bReject ? 2 : CurrentCycle % 2;
		const int32 EventsBefore = Component->GetRuntimeStats().EventCallbackCount;
		Component->SetScriptModuleId(NAME_None);
		Component->SetScriptManifestPath(Artifacts[Candidate].ManifestPath);
		FAvidScriptWasmReloadResult Result;
		const bool bReturned = Component->ReloadConfiguredScript(Result);
		CycleRecord->SetObjectField(bReject ? TEXT("rejection") : TEXT("reload"), ReloadResultJson(Result, bReturned));
		if (bReject)
		{
			Component->SetScriptManifestPath(Artifacts[ActiveArtifact].ManifestPath);
			ExpectedRejectionError = Component->GetRuntimeStats().LastErrorMessage;
			CycleRecord->SetStringField(TEXT("component_rejection_error"), ExpectedRejectionError);
			CycleRecord->SetNumberField(TEXT("rejection_dispatch_frame"), static_cast<double>(StepFrame));
			CycleRecord->SetNumberField(TEXT("events_before_rejection"), EventsBefore);
			if (bReturned || Result.bSucceeded || Result.bReloadApplied || !Result.bRollbackPreservedLiveRuntime
				|| !Result.ErrorMessage.Contains(TEXT("binding_reload_effect_unsupported"))
				|| !ExpectedRejectionError.Contains(TEXT("binding_reload_effect_unsupported")))
			{
				Error = TEXT("reload_rejected_candidate_did_not_fail_closed: ") + Result.ErrorCategory + TEXT(": ") + Result.ErrorMessage; return false;
			}
		}
		else
		{
			if (!bReturned || !Result.bSucceeded || !Result.bReloadApplied || !Result.bStateMigrationApplied
				|| Result.StateMigrationMigratedSlotCount < 1 || !Result.bHostEffectTransactionCommitted)
			{
				Error = TEXT("reload_candidate_did_not_commit: ") + Result.ErrorCategory + TEXT(": ") + Result.ErrorMessage; return false;
			}
			ActiveArtifact = Candidate;
			bLoose = true;
			ExpectedRejectionError.Reset();
		}
		return ValidateRuntime(*Component, Error) && CheckArtifacts(Error);
	}
	if (Step.Action == TEXT("teardown"))
	{
		TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();
		Report->SetObjectField(TEXT("resources_before_teardown"), Resources);
		if (!CaptureResources(*Component, *Widget, Resources, false, Error)) { return false; }
		EventsBeforeStop = Component->GetRuntimeStats().EventCallbackCount;
		Component->EndPlay(EEndPlayReason::RemovedFromWorld);
		Component->UnregisterComponent();
		bStopped = true;
		return RefreshStopped(Host, Component, Widget, Error);
	}
	return true;
}

bool FUiSaveReload::AfterStep(const FProbeStep& Step, AActor* Host, UAvidScriptComponent* Component,
	UUserWidget* Widget, FJsonObject& Action, FString& Error)
{
	if (!CanObserveStep()) { Error = TEXT("reload_observation_requires_later_tick"); return false; }
	if (bWithSaveLoad && !AfterSaveLoadStep(Step, *Host, *Component, *Widget, Action, Error)) { return false; }
	if (Step.Action.StartsWith(TEXT("reload_")) || Step.Action == TEXT("reject"))
	{
		TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();
		const bool bReject = Step.Action == TEXT("reject");
		CycleRecord->SetObjectField(bReject ? TEXT("resources_after_rejection") : TEXT("resources_ready"), Resources);
		if (!ValidateRuntime(*Component, Error) || !CaptureResources(*Component, *Widget, Resources, false, Error)) { return false; }
		CycleRecord->SetStringField(bReject ? TEXT("rejection_score") : TEXT("ready_score"), Action.GetStringField(TEXT("observed_score")));
		CycleRecord->SetNumberField(bReject ? TEXT("rejection_check_frame") : TEXT("ready_frame"), static_cast<double>(GFrameCounter));
		CycleRecord->SetBoolField(bReject ? TEXT("rejection_tick_verified") : TEXT("ready_after_tick"), true);
		if (bReject)
		{
			const int32 Events = Component->GetRuntimeStats().EventCallbackCount;
			CycleRecord->SetNumberField(TEXT("events_after_rejection"), Events);
			if (Events != CycleRecord->GetNumberField(TEXT("events_before_rejection"))) { Error = TEXT("rejection_dispatched_guest_event"); return false; }
		}
	}
	else if (Step.Action == TEXT("collect") && CycleRecord) { CycleRecord->SetStringField(TEXT("collect_score"), Action.GetStringField(TEXT("observed_score"))); }
	else if (Step.Action == TEXT("collect_after_reject"))
	{
		TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();
		CycleRecord->SetObjectField(TEXT("resources_after_collect"), Resources);
		if (!CaptureResources(*Component, *Widget, Resources, false, Error)) { return false; }
		CycleRecord->SetStringField(TEXT("after_rejection_collect_score"), Action.GetStringField(TEXT("observed_score")));
		CycleRecord->SetBoolField(TEXT("passed"), true);
		const auto& Stats = Component->GetRuntimeStats();
		Report->SetNumberField(TEXT("successful_reloads"), Stats.SuccessfulReloadCount - InitialSuccesses);
		Report->SetNumberField(TEXT("rejected_reloads"), Stats.RejectedReloadCount - InitialRejections);
		Report->SetNumberField(TEXT("completed_cycles"), CurrentCycle);
		if (Stats.SuccessfulReloadCount - InitialSuccesses != CurrentCycle || Stats.RejectedReloadCount - InitialRejections != CurrentCycle)
		{
			Error = TEXT("reload_counts_mismatch"); return false;
		}
	}
	else if (Step.Action == TEXT("late_collect"))
	{
		if (!RefreshStopped(Host, Component, Widget, Error)) { return false; }
		Report->SetBoolField(TEXT("late_event_ignored"), true);
		Report->SetNumberField(TEXT("late_event_events_before"), EventsBeforeStop);
		Report->SetNumberField(TEXT("late_event_events_after"), Component->GetRuntimeStats().EventCallbackCount);
	}
	return true;
}

bool FUiSaveReload::ObserveSaveReturn(UAvidScriptComponent& Component, FJsonObject& Action, FString& Error) const
{
	const FAvidScriptRuntimeSession* Session = Component.GetRuntimeSessionForEditorDebugging();
	if (!Session) { Error = TEXT("reload_save_return_session_missing"); return false; }
	const int32 OwnedEntries = Session->GetSnapshot().OwnedObjectEntryCount;
	Action.SetNumberField(TEXT("owned_entries_after_save_return"), OwnedEntries);
	if (OwnedEntries != 0) { Error = TEXT("reload_save_return_retained_owned_object"); return false; }
	return true;
}

bool FUiSaveReload::CheckSaveFile(FString& Error) const
{
	FString Hash;
	if (SaveHash.IsEmpty() ? FPaths::FileExists(SavePath)
		: (!SafeFile(SavePath, false) || !ReadHash(SavePath, Hash) || Hash != SaveHash))
	{
		Error = TEXT("reload_save_changed_outside_save_action"); return false;
	}
	return true;
}

bool FUiSaveReload::BeforeSaveLoadStep(const FProbeStep& Step, AActor& Host, FJsonObject& Action, FString& Error)
{
	if (!CheckSaveFile(Error)) { return false; }
	Action.SetStringField(TEXT("save_sha256_before"), SaveHash);
	if (Step.Action == TEXT("late_collect")) { return true; }
	UObject* Saved = ReadUiObject<UObject>(&Host, TEXT("SavedObject"));
	int32 Score = 0;
	if (SaveHash.IsEmpty() ? Saved != nullptr
		: (!CurrentSavedObject.IsValid() || Saved != CurrentSavedObject.Get() || !ReadReloadSavedScore(Saved, Score) || Score != SavedScore))
	{
		Error = TEXT("reload_current_save_object_changed_before_action"); return false;
	}
	if (Step.Action == TEXT("save")) { SavedBeforeSave = Saved; }
	if (Step.Action == TEXT("load"))
	{
		SavedBeforeLoad = Saved;
		Action.SetBoolField(TEXT("old_saved_object_was_valid"), SavedBeforeLoad.IsValid());
		if (!SavedBeforeLoad.IsValid()) { Error = TEXT("reload_load_has_no_previous_save_object"); return false; }
	}
	return true;
}

bool FUiSaveReload::AfterSaveLoadStep(const FProbeStep& Step, AActor& Host, UAvidScriptComponent& Component,
	UUserWidget& Widget, FJsonObject& Action, FString& Error)
{
	UObject* Saved = ReadUiObject<UObject>(&Host, TEXT("SavedObject"));
	if (Step.Action == TEXT("save"))
	{
		const bool bInitialSave = SaveHash.IsEmpty();
		int32 Score = 0;
		TArray<uint8> Bytes;
		const bool bReused = SavedBeforeSave.IsValid() && Saved == SavedBeforeSave.Get();
		Action.SetBoolField(TEXT("saved_object_reused"), bReused);
		if ((!bInitialSave && !bReused) || !ReadReloadSavedScore(Saved, Score) || Score != FCString::Atoi(*Step.Score)
			|| !SafeFile(SavePath, false) || !FFileHelper::LoadFileToArray(Bytes, *SavePath) || Bytes.IsEmpty())
		{
			Error = TEXT("reload_save_object_or_file_invalid"); return false;
		}
		SaveHash = FAvidScriptHash::Sha256Hex(Bytes);
		SavedScore = Score;
		CurrentSavedObject = Saved;
		Action.SetNumberField(TEXT("saved_score"), Score);
		Action.SetNumberField(TEXT("save_file_bytes"), Bytes.Num());
		TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();
		Action.SetObjectField(TEXT("resources_after_save"), Resources);
		if (!CaptureResources(Component, Widget, Resources, false, Error)) { return false; }
		if (bInitialSave)
		{
			Report->SetStringField(TEXT("initial_save_sha256"), SaveHash);
			Report->SetObjectField(TEXT("resources_after_initial_save"), Resources);
		}
		else
		{
			CycleRecord->SetStringField(TEXT("save_sha256"), SaveHash);
			CycleRecord->SetNumberField(TEXT("save_file_bytes"), Bytes.Num());
			CycleRecord->SetNumberField(TEXT("saved_score"), Score);
			CycleRecord->SetBoolField(TEXT("saved_object_reused"), bReused);
			CycleRecord->SetObjectField(TEXT("resources_after_save"), Resources);
		}
	}
	else
	{
		if (!CheckSaveFile(Error)) { return false; }
		if (Step.Action == TEXT("load"))
		{
			int32 Score = 0;
			const bool bReplaced = SavedBeforeLoad != TWeakObjectPtr<UObject>(Saved);
			Action.SetBoolField(TEXT("saved_object_replaced"), bReplaced);
			if (!bReplaced || !ReadReloadSavedScore(Saved, Score) || Score != SavedScore || Score != FCString::Atoi(*Step.Score))
			{
				Error = TEXT("reload_load_did_not_replace_with_saved_score"); return false;
			}
			CurrentSavedObject = Saved;
			Action.SetNumberField(TEXT("loaded_score"), Score);
			CycleRecord->SetNumberField(TEXT("loaded_score"), Score);
			CycleRecord->SetBoolField(TEXT("saved_object_replaced"), true);
		}
		else if (Step.Action != TEXT("teardown") && Step.Action != TEXT("late_collect") && !SaveHash.IsEmpty())
		{
			int32 Score = 0;
			const bool bPreserved = CurrentSavedObject.IsValid() && Saved == CurrentSavedObject.Get()
				&& ReadReloadSavedScore(Saved, Score) && Score == SavedScore;
			Action.SetBoolField(TEXT("saved_object_preserved"), bPreserved);
			if (!bPreserved) { Error = TEXT("reload_saved_object_not_preserved"); return false; }
		}
		if (Step.Action == TEXT("collect_garbage"))
		{
			const bool bOldCollected = !SavedBeforeLoad.IsValid();
			Action.SetBoolField(TEXT("old_saved_object_collected"), bOldCollected);
			Action.SetBoolField(TEXT("current_saved_object_alive"), CurrentSavedObject.IsValid());
			CycleRecord->SetBoolField(TEXT("old_saved_object_collected"), bOldCollected);
			CycleRecord->SetBoolField(TEXT("current_saved_object_alive"), CurrentSavedObject.IsValid());
			if (!bOldCollected) { Error = TEXT("reload_old_save_object_retained_after_gc"); return false; }
			TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();
			Action.SetObjectField(TEXT("resources_after_gc"), Resources);
			CycleRecord->SetObjectField(TEXT("resources_after_gc"), Resources);
			if (!CaptureResources(Component, Widget, Resources, false, Error)) { return false; }
			Report->SetNumberField(TEXT("gc_cycles"), ++GcCycles);
		}
	}
	Action.SetStringField(TEXT("save_sha256_after"), SaveHash);
	return true;
}

bool FUiSaveReload::RefreshStopped(AActor* Host, UAvidScriptComponent* Component, UUserWidget* Widget, FString& Error)
{
	if (!IsValid(Host) || !IsValid(Component) || !IsValid(Widget)) { Error = TEXT("reload_teardown_observers_lost"); return false; }
	const auto& Stats = Component->GetRuntimeStats();
	AActor* Owner = nullptr;
	FAvidScriptObjectHandleResult OwnerResult;
	const bool bOwnerResolves = Component->ResolveOwnerActor(Owner, OwnerResult);
	const FAvidScriptRuntimeSession* Session = Component->GetRuntimeSessionForEditorDebugging();
	TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
	Json->SetStringField(TEXT("kind"), TEXT("component_end_play"));
	Json->SetBoolField(TEXT("component_end_play"), Stats.bComponentEndPlayObserved);
	Json->SetBoolField(TEXT("guest_end_play"), Stats.bEndPlayCalled);
	Json->SetBoolField(TEXT("runtime_loaded"), Stats.bRuntimeLoaded);
	Json->SetBoolField(TEXT("owner_released"), Stats.bOwnerReleased);
	Json->SetBoolField(TEXT("owner_resolves"), bOwnerResolves);
	Json->SetBoolField(TEXT("session_present"), Session != nullptr);
	Json->SetBoolField(TEXT("widget_in_viewport"), Widget->IsInViewport());
	Json->SetBoolField(TEXT("saved_object_present"), ReadUiObject<UObject>(Host, TEXT("SavedObject")) != nullptr);
	Json->SetNumberField(TEXT("bound_buttons"), CountButtons(*Widget));
	Json->SetNumberField(TEXT("events_before"), EventsBeforeStop);
	Json->SetNumberField(TEXT("events_after"), Stats.EventCallbackCount);
	Json->SetNumberField(TEXT("dropped_events"), Stats.DroppedGameplayEventCount);
	Json->SetStringField(TEXT("error_message"), Stats.LastErrorMessage);
	Json->SetBoolField(TEXT("resource_owner_destroyed"), Session == nullptr);
	// Once Session is destroyed its container counts cannot be sampled. Do not invent zero measurements.
	Json->SetBoolField(TEXT("resource_counts_measured"), false);
	Report->SetObjectField(TEXT("teardown"), Json);
	if (!Stats.bComponentEndPlayObserved || !Stats.bEndPlayCalled || Stats.bRuntimeLoaded || !Stats.bOwnerReleased
		|| bOwnerResolves || Session || Widget->IsInViewport() || ReadUiObject<UObject>(Host, TEXT("SavedObject"))
		|| CountButtons(*Widget) != 0 || Stats.EventCallbackCount != EventsBeforeStop || Stats.DroppedGameplayEventCount != 0
		|| Stats.bCollisionDelegatesBound || Component->IsRegistered() || Stats.LastErrorMessage != ExpectedRejectionError)
	{
		Error = TEXT("reload_teardown_retained_live_state: ") + Stats.LastErrorMessage; return false;
	}
	return true;
}

bool FUiSaveReload::Finish(FString& Error)
{
	const bool bUnchanged = CheckArtifacts(Error);
	const bool bSavePreserved = !bWithSaveLoad || CheckSaveFile(Error);
	Report->SetBoolField(TEXT("artifacts_unchanged"), bUnchanged);
	Report->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - Started);
	bool bRestored = !bHasConfiguration;
	if (bHasConfiguration && ConfiguredComponent.IsValid())
	{
		ConfiguredComponent->SetScriptModuleId(OriginalModuleId);
		ConfiguredComponent->SetScriptManifestPath(OriginalManifestPath);
		bRestored = ConfiguredComponent->GetScriptModuleId() == OriginalModuleId
			&& ConfiguredComponent->GetScriptManifestPath() == OriginalManifestPath;
	}
	Report->SetBoolField(TEXT("configuration_restored"), bRestored);
	if (!bRestored) { Error = TEXT("reload_configuration_not_restored"); }
	return bRestored && bUnchanged && bSavePreserved;
}
}
