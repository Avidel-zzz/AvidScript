#pragma once

#include "CoreMinimal.h"
#include "AvidScriptRuntimeSession.h"
#include "Subsystems/WorldSubsystem.h"

#include "AvidScriptWorldSubsystem.generated.h"

class AActor;
class FAvidScriptStartupCoordinator;
class UAvidScriptComponent;

struct FAvidScriptWorldRuntimeStats
{
	bool bRuntimeLoaded = false;
	bool bBeginPlayCalled = false;
	bool bEndPlayCalled = false;
	bool bStartupScenarioRequested = false;
	bool bStartupScenarioActive = false;
	int32 TickCallCount = 0;
	int32 StartupBindingCount = 0;
	int32 StartupComponentCount = 0;
	int32 StartupOwnedActorCount = 0;
	FString StartupScenarioId;
	FString StartupDocumentPath;
	FString LastErrorCategory;
	FString LastErrorMessage;
	FAvidScriptWasmRuntimeMetrics Metrics;
};

UCLASS(Transient)
class AVIDSCRIPTRUNTIME_API UAvidScriptWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual ~UAvidScriptWorldSubsystem() override;

	const FAvidScriptWorldRuntimeStats& GetRuntimeStats() const { return RuntimeStats; }

	virtual bool DoesSupportWorldType(EWorldType::Type WorldType) const override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void OnWorldEndPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override;
	virtual bool IsTickableInEditor() const override { return false; }
	virtual TStatId GetStatId() const override;
	virtual void Deinitialize() override;

private:
	bool StartPackagedOracle(UWorld& InWorld);
	void TickPackagedOracle(float DeltaTime);
	void StopPackagedOracle();
	void CompletePackagedOracle(bool bSucceeded, const FString& FailureCategory);
	bool StartStartupScenarioProbe();
	void TickStartupScenarioProbe(float DeltaTime);
	void StopStartupScenarioProbe();
	void CompleteStartupScenarioProbe(bool bSucceeded, const FString& FailureCategory);

	FAvidScriptStartupCoordinator* StartupCoordinator = nullptr;
	FAvidScriptWorldRuntimeStats RuntimeStats;
	bool bPackagedOracleActive = false;
	bool bPackagedOracleCompleted = false;
	bool bPackagedOracleRuntimeReady = false;
	bool bPackagedOracleFaultInjected = false;
	bool bPackagedOracleFaultRejected = false;
	float PackagedOracleElapsedSeconds = 0.0f;
	int32 PackagedOracleHealthyTicksBeforeFault = 0;
	FString PackagedOracleModuleId;
	FString PackagedOracleReportPath;
	TWeakObjectPtr<AActor> PackagedOracleHealthyActor;
	TWeakObjectPtr<AActor> PackagedOracleFaultActor;
	TWeakObjectPtr<UAvidScriptComponent> PackagedOracleHealthyComponent;
	TWeakObjectPtr<UAvidScriptComponent> PackagedOracleFaultComponent;
	bool bStartupScenarioProbeActive = false;
	bool bStartupScenarioProbeCompleted = false;
	bool bStartupScenarioProbeRuntimeReady = false;
	float StartupScenarioProbeElapsedSeconds = 0.0f;
	float StartupScenarioProbeNextEventSeconds = 0.0f;
	int32 StartupScenarioProbeEventIndex = 0;
	FString StartupScenarioProbeReportPath;
	TArray<int32> StartupScenarioProbeEvents;
};
