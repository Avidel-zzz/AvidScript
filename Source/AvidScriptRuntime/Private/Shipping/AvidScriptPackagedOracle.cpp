#include "AvidScriptWorldSubsystem.h"

#include "AvidScriptComponent.h"

#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Components/SceneComponent.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptPackagedOracle, Log, All);

namespace
{
constexpr int32 PackagedFaultProbeEvent = 62004;
constexpr float FaultInjectionDelaySeconds = 0.12f;
constexpr float OracleCompletionDelaySeconds = 0.30f;
constexpr float OracleTimeoutSeconds = 5.0f;

bool IsPathUnderRoot(const FString& Path, const FString& Root)
{
	FString FullPath = FPaths::ConvertRelativePathToFull(Path);
	FString FullRoot = FPaths::ConvertRelativePathToFull(Root);
	FPaths::NormalizeFilename(FullPath);
	FPaths::NormalizeDirectoryName(FullRoot);
	return FullPath.StartsWith(FullRoot + TEXT("/"), ESearchCase::IgnoreCase);
}

bool SpawnOracleActor(
	UWorld& World,
	const TCHAR* ActorName,
	const FName ModuleId,
	TWeakObjectPtr<AActor>& OutActor,
	TWeakObjectPtr<UAvidScriptComponent>& OutComponent)
{
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = MakeUniqueObjectName(&World, AActor::StaticClass(), FName(ActorName));
	SpawnParameters.ObjectFlags |= RF_Transient;
	AActor* Actor = World.SpawnActor<AActor>(
		AActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParameters);
	if (Actor == nullptr)
	{
		return false;
	}
	USceneComponent* RootComponent = NewObject<USceneComponent>(
		Actor,
		USceneComponent::StaticClass(),
		NAME_None,
		RF_Transient);
	if (RootComponent == nullptr)
	{
		Actor->Destroy();
		return false;
	}
	Actor->SetRootComponent(RootComponent);
	Actor->AddInstanceComponent(RootComponent);
	RootComponent->RegisterComponent();

	UAvidScriptComponent* Component = NewObject<UAvidScriptComponent>(
		Actor,
		UAvidScriptComponent::StaticClass(),
		NAME_None,
		RF_Transient);
	if (Component == nullptr)
	{
		Actor->Destroy();
		return false;
	}

	Component->SetScriptModuleId(ModuleId);
	Actor->AddInstanceComponent(Component);
	Component->RegisterComponent();
	OutActor = Actor;
	OutComponent = Component;
	return true;
}

void SetOracleStats(
	const TSharedRef<FJsonObject>& Object,
	const FAvidScriptComponentRuntimeStats& Stats)
{
	Object->SetBoolField(TEXT("resolved_from_package"), Stats.bResolvedFromPackage);
	Object->SetStringField(TEXT("package_id"), Stats.PackageId);
	Object->SetStringField(TEXT("module_id"), Stats.ModuleId);
	Object->SetBoolField(TEXT("runtime_loaded"), Stats.bRuntimeLoaded);
	Object->SetBoolField(TEXT("begin_play"), Stats.bBeginPlayCalled);
	Object->SetBoolField(TEXT("end_play"), Stats.bEndPlayCalled);
	Object->SetNumberField(TEXT("ticks"), Stats.TickCallCount);
	Object->SetNumberField(TEXT("timer_callbacks"), Stats.TimerCallbackCount);
	Object->SetNumberField(TEXT("event_callbacks"), Stats.EventCallbackCount);
	Object->SetStringField(TEXT("last_error"), Stats.LastErrorMessage.Left(1024));
}
} // namespace

bool UAvidScriptWorldSubsystem::StartPackagedOracle(UWorld& InWorld)
{
	FString RequestedModuleId;
	if (!FParse::Value(
			FCommandLine::Get(),
			TEXT("AvidScriptPackagedOracle="),
			RequestedModuleId))
	{
		return false;
	}

	bPackagedOracleActive = true;
	bPackagedOracleCompleted = false;
	bPackagedOracleRuntimeReady = false;
	bPackagedOracleFaultInjected = false;
	bPackagedOracleFaultRejected = false;
	PackagedOracleElapsedSeconds = 0.0f;
	PackagedOracleHealthyTicksBeforeFault = 0;
	PackagedOracleModuleId = MoveTemp(RequestedModuleId);

	FParse::Value(
		FCommandLine::Get(),
		TEXT("AvidScriptPackagedOracleReport="),
		PackagedOracleReportPath);
	if (PackagedOracleReportPath.IsEmpty())
	{
		PackagedOracleReportPath = FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScript/PackagedOracle/report.json"));
	}
	PackagedOracleReportPath = FPaths::ConvertRelativePathToFull(PackagedOracleReportPath);
	FPaths::NormalizeFilename(PackagedOracleReportPath);
	if (!IsPathUnderRoot(PackagedOracleReportPath, FPaths::ProjectSavedDir()))
	{
		PackagedOracleReportPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(
			FPaths::ProjectSavedDir(),
			TEXT("AvidScript/PackagedOracle/rejected-path.json")));
		CompletePackagedOracle(false, TEXT("report_path_outside_saved"));
		return true;
	}

	const FName ModuleId(*PackagedOracleModuleId);
	if (!SpawnOracleActor(
			InWorld,
			TEXT("AvidScriptPackagedHealthy"),
			ModuleId,
			PackagedOracleHealthyActor,
			PackagedOracleHealthyComponent)
		|| !SpawnOracleActor(
			InWorld,
			TEXT("AvidScriptPackagedFault"),
			ModuleId,
			PackagedOracleFaultActor,
			PackagedOracleFaultComponent))
	{
		CompletePackagedOracle(false, TEXT("actor_spawn_failed"));
		return true;
	}

	UAvidScriptComponent* HealthyComponent = PackagedOracleHealthyComponent.Get();
	UAvidScriptComponent* FaultComponent = PackagedOracleFaultComponent.Get();
	if (HealthyComponent == nullptr || FaultComponent == nullptr)
	{
		CompletePackagedOracle(false, TEXT("runtime_start_failed"));
		return true;
	}
	return true;
}

void UAvidScriptWorldSubsystem::TickPackagedOracle(float DeltaTime)
{
	PackagedOracleElapsedSeconds += FMath::Max(DeltaTime, 0.0f);
	UAvidScriptComponent* HealthyComponent = PackagedOracleHealthyComponent.Get();
	UAvidScriptComponent* FaultComponent = PackagedOracleFaultComponent.Get();
	if (HealthyComponent == nullptr || FaultComponent == nullptr)
	{
		CompletePackagedOracle(false, TEXT("oracle_object_lost"));
		return;
	}
	if (PackagedOracleElapsedSeconds >= OracleTimeoutSeconds)
	{
		CompletePackagedOracle(false, TEXT("oracle_timeout"));
		return;
	}

	if (!bPackagedOracleRuntimeReady)
	{
		const FAvidScriptComponentRuntimeStats& HealthyStats = HealthyComponent->GetRuntimeStats();
		const FAvidScriptComponentRuntimeStats& FaultStats = FaultComponent->GetRuntimeStats();
		if (!HealthyStats.bRuntimeLoaded || !FaultStats.bRuntimeLoaded)
		{
			if (!HealthyStats.LastErrorMessage.IsEmpty() || !FaultStats.LastErrorMessage.IsEmpty())
			{
				CompletePackagedOracle(false, TEXT("runtime_start_failed"));
			}
			return;
		}
		if (!HealthyComponent->DispatchScriptEvent(62001, 25.0f))
		{
			CompletePackagedOracle(false, TEXT("initial_event_dispatch_failed"));
			return;
		}
		bPackagedOracleRuntimeReady = true;
		UE_LOG(
			LogAvidScriptPackagedOracle,
			Log,
			TEXT("AvidScript packaged oracle started | module=%s | report=%s"),
			*PackagedOracleModuleId,
			*PackagedOracleReportPath);
	}

	if (!bPackagedOracleFaultInjected
		&& PackagedOracleElapsedSeconds >= FaultInjectionDelaySeconds)
	{
		PackagedOracleHealthyTicksBeforeFault = HealthyComponent->GetRuntimeStats().TickCallCount;
		bPackagedOracleFaultRejected = !FaultComponent->DispatchScriptEvent(
			PackagedFaultProbeEvent,
			0.0f);
		bPackagedOracleFaultInjected = true;
	}

	if (!bPackagedOracleFaultInjected
		|| PackagedOracleElapsedSeconds < OracleCompletionDelaySeconds)
	{
		return;
	}

	const FAvidScriptComponentRuntimeStats& HealthyStats = HealthyComponent->GetRuntimeStats();
	const FAvidScriptComponentRuntimeStats& FaultStats = FaultComponent->GetRuntimeStats();
	const AActor* HealthyActor = PackagedOracleHealthyActor.Get();
	const bool bWorldContinued = HealthyStats.TickCallCount > PackagedOracleHealthyTicksBeforeFault;
	const bool bContinuationObserved = HealthyActor != nullptr
		&& HealthyActor->GetActorScale3D().Y >= 64.0;
	const bool bSucceeded = HealthyStats.bResolvedFromPackage
		&& HealthyStats.bRuntimeLoaded
		&& HealthyStats.bBeginPlayCalled
		&& HealthyStats.TickCallCount >= 3
		&& HealthyStats.TimerCallbackCount >= 1
		&& HealthyStats.EventCallbackCount >= 1
		&& bContinuationObserved
		&& bPackagedOracleFaultRejected
		&& !FaultStats.bRuntimeLoaded
		&& !FaultStats.LastErrorMessage.IsEmpty()
		&& bWorldContinued;
	CompletePackagedOracle(
		bSucceeded,
		bSucceeded ? FString() : TEXT("oracle_assertion_failed"));
}

void UAvidScriptWorldSubsystem::CompletePackagedOracle(
	const bool bSucceeded,
	const FString& FailureCategory)
{
	if (bPackagedOracleCompleted)
	{
		return;
	}
	bPackagedOracleCompleted = true;
	bPackagedOracleActive = false;

	UAvidScriptComponent* HealthyComponent = PackagedOracleHealthyComponent.Get();
	UAvidScriptComponent* FaultComponent = PackagedOracleFaultComponent.Get();
	AActor* HealthyActor = PackagedOracleHealthyActor.Get();
	AActor* FaultActor = PackagedOracleFaultActor.Get();
	FAvidScriptComponentRuntimeStats HealthyStats = HealthyComponent != nullptr
		? HealthyComponent->GetRuntimeStats()
		: FAvidScriptComponentRuntimeStats();
	FAvidScriptComponentRuntimeStats FaultStats = FaultComponent != nullptr
		? FaultComponent->GetRuntimeStats()
		: FAvidScriptComponentRuntimeStats();
	const FVector HealthyScale = HealthyActor != nullptr
		? HealthyActor->GetActorScale3D()
		: FVector::ZeroVector;

	if (HealthyActor != nullptr)
	{
		HealthyActor->Destroy();
		if (HealthyComponent != nullptr)
		{
			HealthyStats = HealthyComponent->GetRuntimeStats();
		}
	}
	if (FaultActor != nullptr)
	{
		FaultActor->Destroy();
		if (FaultComponent != nullptr)
		{
			FaultStats = FaultComponent->GetRuntimeStats();
		}
	}
	const bool bFinalSucceeded = bSucceeded && HealthyStats.bEndPlayCalled;
	const FString FinalFailureCategory = bFinalSucceeded
		? FString()
		: (FailureCategory.IsEmpty() ? TEXT("end_play_not_observed") : FailureCategory);

	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("schema_version"), 1);
	Root->SetStringField(
		TEXT("result"),
		bFinalSucceeded
			? TEXT("avidscript_packaged_oracle_passed")
			: TEXT("avidscript_packaged_oracle_failed"));
	Root->SetStringField(TEXT("status"), bFinalSucceeded ? TEXT("ok") : TEXT("error"));
	Root->SetStringField(TEXT("configuration"),
#if UE_BUILD_SHIPPING
		TEXT("Shipping")
#else
		TEXT("Development")
#endif
	);
	Root->SetStringField(TEXT("module_id"), PackagedOracleModuleId);
	Root->SetStringField(TEXT("failure_category"), FinalFailureCategory);
	Root->SetNumberField(TEXT("elapsed_ms"), PackagedOracleElapsedSeconds * 1000.0f);
	Root->SetBoolField(TEXT("fault_injected"), bPackagedOracleFaultInjected);
	Root->SetBoolField(TEXT("fault_rejected"), bPackagedOracleFaultRejected);
	Root->SetBoolField(
		TEXT("world_continued"),
		HealthyStats.TickCallCount > PackagedOracleHealthyTicksBeforeFault);
	Root->SetBoolField(TEXT("continuation_observed"), HealthyScale.Y >= 64.0);
	Root->SetNumberField(TEXT("healthy_scale_x"), HealthyScale.X);
	Root->SetNumberField(TEXT("healthy_scale_y"), HealthyScale.Y);
	Root->SetNumberField(TEXT("healthy_scale_z"), HealthyScale.Z);

	const TSharedRef<FJsonObject> HealthyObject = MakeShared<FJsonObject>();
	SetOracleStats(HealthyObject, HealthyStats);
	Root->SetObjectField(TEXT("healthy"), HealthyObject);
	const TSharedRef<FJsonObject> FaultObject = MakeShared<FJsonObject>();
	SetOracleStats(FaultObject, FaultStats);
	Root->SetObjectField(TEXT("fault"), FaultObject);

	FString Json;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json);
	FJsonSerializer::Serialize(Root, Writer);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(PackagedOracleReportPath), true);
	const bool bReportWritten = FFileHelper::SaveStringToFile(
		Json,
		*PackagedOracleReportPath,
		FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	UE_LOG(
		LogAvidScriptPackagedOracle,
		Display,
		TEXT("AVIDSCRIPT_PACKAGED_ORACLE %s"),
		*Json);
	if (!bReportWritten)
	{
		UE_LOG(
			LogAvidScriptPackagedOracle,
			Error,
			TEXT("AvidScript packaged oracle report could not be written: %s"),
			*PackagedOracleReportPath);
	}

	PackagedOracleHealthyActor.Reset();
	PackagedOracleFaultActor.Reset();
	PackagedOracleHealthyComponent.Reset();
	PackagedOracleFaultComponent.Reset();
	FPlatformMisc::RequestExitWithStatus(
		false,
		bFinalSucceeded && bReportWritten ? 0 : 1,
		TEXT("AvidScriptPackagedOracle"));
}

void UAvidScriptWorldSubsystem::StopPackagedOracle()
{
	bPackagedOracleActive = false;
	bPackagedOracleRuntimeReady = false;
	if (AActor* HealthyActor = PackagedOracleHealthyActor.Get())
	{
		HealthyActor->Destroy();
	}
	if (AActor* FaultActor = PackagedOracleFaultActor.Get())
	{
		FaultActor->Destroy();
	}
	PackagedOracleHealthyActor.Reset();
	PackagedOracleFaultActor.Reset();
	PackagedOracleHealthyComponent.Reset();
	PackagedOracleFaultComponent.Reset();
}
