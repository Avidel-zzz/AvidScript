#pragma once

#include "CoreMinimal.h"
#include "AvidScriptRuntimeSession.h"
#include "Subsystems/WorldSubsystem.h"

#include "AvidScriptWorldSubsystem.generated.h"

class AActor;
class UAvidScriptComponent;

struct FAvidScriptWorldRuntimeStats
{
	bool bRuntimeLoaded = false;
	bool bBeginPlayCalled = false;
	bool bEndPlayCalled = false;
	int32 TickCallCount = 0;
	FString LastErrorMessage;
	FAvidScriptWasmRuntimeMetrics Metrics;
};

UCLASS(Transient)
class AVIDSCRIPTRUNTIME_API UAvidScriptWorldSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
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
	void RecordFailure(const FAvidScriptWasmSmokeResult& Result);
	void ReleaseRuntime(FAvidScriptWasmSmokeResult* OutUnloadResult = nullptr);
	void FlushDeferredRuntimeRelease();

	TUniquePtr<FAvidScriptRuntimeSession> RuntimeSession;
	FAvidScriptWorldRuntimeStats RuntimeStats;
	bool bRuntimeReleaseDeferred = false;
	bool bRuntimeReleaseInProgress = false;
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
};
