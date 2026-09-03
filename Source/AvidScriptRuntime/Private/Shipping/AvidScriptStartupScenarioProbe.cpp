#include "AvidScriptWorldSubsystem.h"

#include "AvidScriptComponent.h"
#include "Startup/AvidScriptStartupCoordinator.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptStartupScenarioProbe, Log, All);

namespace
{
constexpr float ProbeTimeoutSeconds = 5.0f;
constexpr float FirstEventSeconds = 0.08f;
constexpr float EventIntervalSeconds = 0.08f;
constexpr float CompletionDelaySeconds = 0.25f;
constexpr int32 MaximumProbeEvents = 64;

bool IsAvidScriptScenarioProbePathUnderRoot(const FString& Path, const FString& Root)
{
	FString FullPath = FPaths::ConvertRelativePathToFull(Path);
	FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeFilename(FullPath);
	FPaths::NormalizeDirectoryName(FullRoot);
	return FullPath.StartsWith(FullRoot + TEXT("/"), ESearchCase::IgnoreCase);
}

TSharedRef<FJsonObject> MakeVectorObject(const FVector& Value)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("x"), Value.X);
	Object->SetNumberField(TEXT("y"), Value.Y);
	Object->SetNumberField(TEXT("z"), Value.Z);
	return Object;
}

TSharedRef<FJsonObject> MakeRotatorObject(const FRotator& Value)
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetNumberField(TEXT("pitch"), Value.Pitch);
	Object->SetNumberField(TEXT("yaw"), Value.Yaw);
	Object->SetNumberField(TEXT("roll"), Value.Roll);
	return Object;
}
} // namespace

bool UAvidScriptWorldSubsystem::StartStartupScenarioProbe()
{
	FString RequestedReportPath;
	const bool bHasReportPath = FParse::Value(
			FCommandLine::Get(),
			TEXT("AvidScriptScenarioProbeReport="),
			RequestedReportPath);
	FString RunId;
	const bool bHasRunId = FParse::Value(
		FCommandLine::Get(), TEXT("AvidScriptScenarioProbeRunId="), RunId);
	if (!bHasReportPath && !bHasRunId)
	{
		return false;
	}

	bStartupScenarioProbeActive = true;
	bStartupScenarioProbeCompleted = false;
	bStartupScenarioProbeRuntimeReady = false;
	StartupScenarioProbeElapsedSeconds = 0.0f;
	StartupScenarioProbeNextEventSeconds = FirstEventSeconds;
	StartupScenarioProbeEventIndex = 0;
	StartupScenarioProbeEvents.Reset();
	if (bHasRunId)
	{
		FGuid ParsedRunId;
		if (bHasReportPath || !FGuid::ParseExact(RunId, EGuidFormats::Digits, ParsedRunId))
		{
			StartupScenarioProbeReportPath = FPaths::Combine(
				FPaths::ProjectSavedDir(), TEXT("AvidScript/ScenarioProbe/rejected-identity.json"));
			CompleteStartupScenarioProbe(false, TEXT("probe_identity_invalid"));
			return true;
		}
		RequestedReportPath = FPaths::Combine(
			FPaths::ProjectSavedDir(), TEXT("AvidScript/ScenarioProbe"),
			ParsedRunId.ToString(EGuidFormats::Digits) + TEXT(".json"));
	}

	StartupScenarioProbeReportPath = FPaths::ConvertRelativePathToFull(RequestedReportPath);
	FPaths::NormalizeFilename(StartupScenarioProbeReportPath);
	if (RequestedReportPath.IsEmpty()
		|| !IsAvidScriptScenarioProbePathUnderRoot(
			StartupScenarioProbeReportPath,
			FPaths::ProjectSavedDir()))
	{
		StartupScenarioProbeReportPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScript/ScenarioProbe/rejected-path.json")));
		CompleteStartupScenarioProbe(false, TEXT("report_path_outside_saved"));
		return true;
	}

	FString EventList;
	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptScenarioProbeEvents="),
		EventList,
		false);
	TArray<FString> EventTokens;
	EventList.ParseIntoArray(EventTokens, TEXT(","), true);
	if (EventTokens.IsEmpty() || EventTokens.Num() > MaximumProbeEvents)
	{
		CompleteStartupScenarioProbe(false, TEXT("probe_events_invalid"));
		return true;
	}
	for (const FString& Token : EventTokens)
	{
		int32 EventId = 0;
		if (!LexTryParseString(EventId, *Token) || EventId <= 0)
		{
			CompleteStartupScenarioProbe(false, TEXT("probe_events_invalid"));
			return true;
		}
		StartupScenarioProbeEvents.Add(EventId);
	}

	if (StartupCoordinator == nullptr || !StartupCoordinator->IsActive())
	{
		CompleteStartupScenarioProbe(false, TEXT("startup_scenario_inactive"));
		return true;
	}
	return true;
}

void UAvidScriptWorldSubsystem::TickStartupScenarioProbe(float DeltaTime)
{
	StartupScenarioProbeElapsedSeconds += FMath::Max(DeltaTime, 0.0f);
	if (StartupScenarioProbeElapsedSeconds >= ProbeTimeoutSeconds)
	{
		CompleteStartupScenarioProbe(false, TEXT("probe_timeout"));
		return;
	}
	if (StartupCoordinator == nullptr || !StartupCoordinator->IsActive())
	{
		CompleteStartupScenarioProbe(false, TEXT("startup_scenario_lost"));
		return;
	}

	TArray<UAvidScriptComponent*> Components;
	StartupCoordinator->GetLiveComponents(Components);
	if (Components.IsEmpty())
	{
		CompleteStartupScenarioProbe(false, TEXT("startup_components_missing"));
		return;
	}
	if (!bStartupScenarioProbeRuntimeReady)
	{
		for (const UAvidScriptComponent* Component : Components)
		{
			const FAvidScriptComponentRuntimeStats& Stats = Component->GetRuntimeStats();
			if (!Stats.bRuntimeLoaded || !Stats.bBeginPlayCalled)
			{
				return;
			}
		}
		bStartupScenarioProbeRuntimeReady = true;
	}

	if (StartupScenarioProbeEventIndex < StartupScenarioProbeEvents.Num()
		&& StartupScenarioProbeElapsedSeconds >= StartupScenarioProbeNextEventSeconds)
	{
		UAvidScriptComponent* Component = Components[0];
		const int32 EventId = StartupScenarioProbeEvents[StartupScenarioProbeEventIndex];
		if (!Component->DispatchScriptEvent(
				EventId,
				static_cast<float>(StartupScenarioProbeEventIndex + 1)))
		{
			CompleteStartupScenarioProbe(false, TEXT("probe_event_dispatch_failed"));
			return;
		}
		++StartupScenarioProbeEventIndex;
		StartupScenarioProbeNextEventSeconds += EventIntervalSeconds;
		return;
	}

	const float ExpectedCompletionSeconds = FirstEventSeconds
		+ EventIntervalSeconds * static_cast<float>(StartupScenarioProbeEvents.Num())
		+ CompletionDelaySeconds;
	if (StartupScenarioProbeEventIndex == StartupScenarioProbeEvents.Num()
		&& StartupScenarioProbeElapsedSeconds >= ExpectedCompletionSeconds)
	{
		CompleteStartupScenarioProbe(true, FString());
	}
}

void UAvidScriptWorldSubsystem::CompleteStartupScenarioProbe(
	const bool bSucceeded,
	const FString& FailureCategory)
{
	if (bStartupScenarioProbeCompleted)
	{
		return;
	}
	bStartupScenarioProbeCompleted = true;
	bStartupScenarioProbeActive = false;

	bool bFinalSucceeded = bSucceeded;
	TArray<UAvidScriptComponent*> Components;
	if (StartupCoordinator != nullptr)
	{
		StartupCoordinator->GetLiveComponents(Components);
	}
	TArray<TSharedPtr<FJsonValue>> ComponentValues;
	for (UAvidScriptComponent* Component : Components)
	{
		const FAvidScriptComponentRuntimeStats& Stats = Component->GetRuntimeStats();
		AActor* Owner = Component->GetOwner();
		bFinalSucceeded = bFinalSucceeded
			&& Owner != nullptr
			&& Stats.bRuntimeLoaded
			&& Stats.bBeginPlayCalled
			&& Stats.TickCallCount > 0
			&& Stats.DroppedGameplayEventCount == 0
			&& Stats.LastErrorMessage.IsEmpty();
		const TSharedRef<FJsonObject> ComponentObject = MakeShared<FJsonObject>();
		ComponentObject->SetStringField(
			TEXT("owner"),
			Owner != nullptr ? Owner->GetPathName() : TEXT("<none>"));
		ComponentObject->SetStringField(TEXT("module_id"), Stats.ModuleId);
		ComponentObject->SetBoolField(TEXT("runtime_loaded"), Stats.bRuntimeLoaded);
		ComponentObject->SetBoolField(TEXT("begin_play"), Stats.bBeginPlayCalled);
		ComponentObject->SetNumberField(TEXT("ticks"), Stats.TickCallCount);
		ComponentObject->SetNumberField(TEXT("events"), Stats.EventCallbackCount);
		ComponentObject->SetNumberField(
			TEXT("deferred_gameplay_events"),
			Stats.DeferredGameplayEventCount);
		ComponentObject->SetNumberField(
			TEXT("dropped_gameplay_events"),
			Stats.DroppedGameplayEventCount);
		ComponentObject->SetStringField(TEXT("last_error"), Stats.LastErrorMessage.Left(1024));
		if (Owner != nullptr)
		{
			ComponentObject->SetObjectField(TEXT("location"), MakeVectorObject(Owner->GetActorLocation()));
			ComponentObject->SetObjectField(TEXT("rotation"), MakeRotatorObject(Owner->GetActorRotation()));
			ComponentObject->SetObjectField(TEXT("scale"), MakeVectorObject(Owner->GetActorScale3D()));
			ComponentObject->SetBoolField(TEXT("hidden"), Owner->IsHidden());
			ComponentObject->SetBoolField(TEXT("collision_enabled"), Owner->GetActorEnableCollision());
		}
		ComponentValues.Add(MakeShared<FJsonValueObject>(ComponentObject));
	}
	if (Components.IsEmpty()
		|| StartupScenarioProbeEventIndex != StartupScenarioProbeEvents.Num()
		|| Components[0]->GetRuntimeStats().EventCallbackCount
			< StartupScenarioProbeEvents.Num())
	{
		bFinalSucceeded = false;
	}

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	FString RunId;
	FParse::Value(FCommandLine::Get(), TEXT("AvidScriptScenarioProbeRunId="), RunId);
	Root->SetStringField(TEXT("run_id"), RunId.ToLower());
	Root->SetStringField(
		TEXT("result"),
		bFinalSucceeded
			? TEXT("avidscript_startup_scenario_probe_passed")
			: TEXT("avidscript_startup_scenario_probe_failed"));
	Root->SetStringField(
		TEXT("failure_category"),
		bFinalSucceeded
			? FString()
			: (FailureCategory.IsEmpty() ? TEXT("probe_expectation_failed") : FailureCategory));
	Root->SetStringField(TEXT("scenario_id"), RuntimeStats.StartupScenarioId);
	Root->SetNumberField(TEXT("elapsed_ms"), StartupScenarioProbeElapsedSeconds * 1000.0f);
	Root->SetNumberField(TEXT("events_requested"), StartupScenarioProbeEvents.Num());
	Root->SetNumberField(TEXT("events_dispatched"), StartupScenarioProbeEventIndex);
	Root->SetArrayField(TEXT("components"), MoveTemp(ComponentValues));

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	const bool bSerialized = FJsonSerializer::Serialize(Root, Writer);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(StartupScenarioProbeReportPath), true);
	const bool bReportWritten = bSerialized
		&& FFileHelper::SaveStringToFile(
			Json,
			*StartupScenarioProbeReportPath,
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(
		LogAvidScriptStartupScenarioProbe,
		Display,
		TEXT("AVIDSCRIPT_STARTUP_SCENARIO_PROBE %s"),
		*Json);
	FPlatformMisc::RequestExitWithStatus(
		false,
		bFinalSucceeded && bReportWritten ? 0 : 1,
		TEXT("AvidScriptStartupScenarioProbe"));
}

void UAvidScriptWorldSubsystem::StopStartupScenarioProbe()
{
	bStartupScenarioProbeActive = false;
	bStartupScenarioProbeRuntimeReady = false;
	StartupScenarioProbeEvents.Reset();
}
