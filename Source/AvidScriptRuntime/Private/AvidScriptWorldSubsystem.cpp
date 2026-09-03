#include "AvidScriptWorldSubsystem.h"

#include "Startup/AvidScriptStartupCoordinator.h"

#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogAvidScriptWorldSubsystem, Log, All);

UAvidScriptWorldSubsystem::~UAvidScriptWorldSubsystem()
{
	delete StartupCoordinator;
	StartupCoordinator = nullptr;
}

bool UAvidScriptWorldSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UAvidScriptWorldSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	RuntimeStats = FAvidScriptWorldRuntimeStats();
	bStartupActivationPending = false;
	delete StartupCoordinator;
	StartupCoordinator = nullptr;
	if (StartPackagedOracle(InWorld))
	{
		return;
	}
	FString RequestedValue;
	if (!FParse::Value(FCommandLine::Get(), TEXT("AvidScriptScenario="), RequestedValue)
		&& !FParse::Value(FCommandLine::Get(), TEXT("AvidScriptScenarioProbeReport="), RequestedValue)
		&& !FParse::Value(FCommandLine::Get(), TEXT("AvidScriptScenarioProbeRunId="), RequestedValue))
	{
		return;
	}

	// World subsystems begin before GameMode starts actors; validate all runtimes after that boundary.
	bStartupActivationPending = true;
	if (InWorld.HasBegunPlay())
	{
		ActivateStartupScenario(InWorld);
	}
}

void UAvidScriptWorldSubsystem::ActivateStartupScenario(UWorld& InWorld)
{
	bStartupActivationPending = false;

	StartupCoordinator = new FAvidScriptStartupCoordinator();
	FAvidScriptStartupRuntimeResult StartupResult;
	if (!StartupCoordinator->ActivateFromProcess(InWorld, StartupResult))
	{
		RuntimeStats.bStartupScenarioRequested = StartupResult.bRequested;
		RuntimeStats.StartupScenarioId = StartupResult.ScenarioId;
		RuntimeStats.StartupDocumentPath = StartupResult.DocumentPath;
		RuntimeStats.LastErrorCategory = StartupResult.ErrorCategory;
		RuntimeStats.LastErrorMessage = StartupResult.ErrorMessage;
		UE_LOG(LogAvidScriptWorldSubsystem, Error, TEXT("%s"), *StartupResult.ErrorMessage);
		StartStartupScenarioProbe();
		delete StartupCoordinator;
		StartupCoordinator = nullptr;
		return;
	}

	RuntimeStats.bStartupScenarioRequested = StartupResult.bRequested;
	RuntimeStats.bStartupScenarioActive = StartupResult.bActive;
	RuntimeStats.StartupScenarioId = StartupResult.ScenarioId;
	RuntimeStats.StartupDocumentPath = StartupResult.DocumentPath;
	RuntimeStats.StartupBindingCount = StartupResult.BindingCount;
	RuntimeStats.StartupComponentCount = StartupResult.ComponentCount;
	RuntimeStats.StartupOwnedActorCount = StartupResult.OwnedActorCount;
	RuntimeStats.bRuntimeLoaded = StartupResult.ComponentCount > 0
		&& StartupResult.RuntimeLoadedCount == StartupResult.ComponentCount;
	RuntimeStats.bBeginPlayCalled = StartupResult.ComponentCount > 0
		&& StartupResult.BeginPlayCount == StartupResult.ComponentCount;

	if (!StartupResult.bRequested)
	{
		StartStartupScenarioProbe();
		delete StartupCoordinator;
		StartupCoordinator = nullptr;
		return;
	}

	UE_LOG(
		LogAvidScriptWorldSubsystem,
		Log,
		TEXT("AvidScript startup scenario active | world=%s | scenario=%s | bindings=%d | components=%d | owned_actors=%d"),
		*InWorld.GetName(),
		*StartupResult.ScenarioId,
		StartupResult.BindingCount,
		StartupResult.ComponentCount,
		StartupResult.OwnedActorCount);
	StartStartupScenarioProbe();
}

void UAvidScriptWorldSubsystem::OnWorldEndPlay(UWorld& InWorld)
{
	bStartupActivationPending = false;
	StopPackagedOracle();
	StopStartupScenarioProbe();
	if (StartupCoordinator != nullptr)
	{
		StartupCoordinator->Deactivate();
		delete StartupCoordinator;
		StartupCoordinator = nullptr;
	}
	RuntimeStats.bRuntimeLoaded = false;
	RuntimeStats.bStartupScenarioActive = false;
	RuntimeStats.bEndPlayCalled = true;
	Super::OnWorldEndPlay(InWorld);
}

void UAvidScriptWorldSubsystem::Tick(float DeltaTime)
{
	if (bStartupActivationPending)
	{
		if (UWorld* World = GetWorld(); World != nullptr && World->HasBegunPlay() && !World->bIsTearingDown)
		{
			ActivateStartupScenario(*World);
		}
	}
	if (bPackagedOracleActive)
	{
		TickPackagedOracle(DeltaTime);
	}
	if (bStartupScenarioProbeActive)
	{
		TickStartupScenarioProbe(DeltaTime);
	}
	Super::Tick(DeltaTime);
}

bool UAvidScriptWorldSubsystem::IsTickable() const
{
	return bStartupActivationPending || bPackagedOracleActive || bStartupScenarioProbeActive;
}

TStatId UAvidScriptWorldSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UAvidScriptWorldSubsystem, STATGROUP_Tickables);
}

void UAvidScriptWorldSubsystem::Deinitialize()
{
	bStartupActivationPending = false;
	if (UWorld* World = GetWorld(); World != nullptr && !RuntimeStats.bEndPlayCalled)
	{
		OnWorldEndPlay(*World);
	}
	else
	{
		StopPackagedOracle();
		StopStartupScenarioProbe();
		if (StartupCoordinator != nullptr)
		{
			StartupCoordinator->Deactivate();
			delete StartupCoordinator;
			StartupCoordinator = nullptr;
		}
	}
	Super::Deinitialize();
}
