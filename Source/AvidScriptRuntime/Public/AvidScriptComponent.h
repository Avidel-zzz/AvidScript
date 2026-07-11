#pragma once

#include "CoreMinimal.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptWasmRuntime.h"
#include "Components/ActorComponent.h"
#include "UObject/SoftObjectPath.h"

#include "AvidScriptComponent.generated.h"

struct FAvidScriptComponentRuntimeStats
{
	bool bOwnerRegistered = false;
	bool bOwnerReleased = false;
	bool bRuntimeLoaded = false;
	bool bBeginPlayCalled = false;
	bool bComponentEndPlayObserved = false;
	bool bEndPlayCalled = false;
	int32 TickCallCount = 0;
	int32 TimerCallbackCount = 0;
	int32 LastTimerCallbackId = 0;
	int32 LastTimerHandle = 0;
	FAvidScriptObjectHandle OwnerHandle;
	FString OwnerObjectPath;
	FString ScriptManifestPath;
	FString ModuleId;
	FString LastErrorMessage;
	FAvidScriptWasmRuntimeMetrics Metrics;
};

UCLASS(ClassGroup = (AvidScript), meta = (BlueprintSpawnableComponent))
class AVIDSCRIPTRUNTIME_API UAvidScriptComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAvidScriptComponent();

	const FAvidScriptComponentRuntimeStats& GetRuntimeStats() const { return RuntimeStats; }
	void SetScriptManifestPath(const FString& InScriptManifestPath);
	FString GetScriptManifestPath() const;
	bool ResolveOwnerActor(AActor*& OutOwner, FAvidScriptObjectHandleResult& OutResult) const;

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool RegisterOwner();
	void ReleaseOwner();
	bool LoadConfiguredScriptModule(FAvidScriptWasmSmokeResult& OutResult);
	FString ResolveScriptManifestPath() const;
	void RecordRuntimeFailure(const FAvidScriptWasmSmokeResult& Result);
	void ReleaseRuntime(FAvidScriptWasmSmokeResult* OutUnloadResult = nullptr);

	UPROPERTY(EditAnywhere, Category = "AvidScript", meta = (FilePathFilter = "avidscript.json"))
	FFilePath ScriptManifestFile;

	FAvidScriptObjectRegistry ObjectRegistry;
	FAvidScriptObjectHandle OwnerHandle;
	TUniquePtr<FAvidScriptWasmRuntimeInstance> Runtime;
	FAvidScriptComponentRuntimeStats RuntimeStats;
	bool bPlayActive = false;
};
