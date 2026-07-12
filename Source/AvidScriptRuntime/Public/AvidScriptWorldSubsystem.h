#pragma once

#include "CoreMinimal.h"
#include "AvidScriptRuntimeSession.h"
#include "Subsystems/WorldSubsystem.h"

#include "AvidScriptWorldSubsystem.generated.h"

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
	void RecordFailure(const FAvidScriptWasmSmokeResult& Result);
	void ReleaseRuntime(FAvidScriptWasmSmokeResult* OutUnloadResult = nullptr);

	TUniquePtr<FAvidScriptRuntimeSession> RuntimeSession;
	FAvidScriptWorldRuntimeStats RuntimeStats;
};
