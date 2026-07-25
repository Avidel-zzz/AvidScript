#pragma once

#include "CoreMinimal.h"
#include "AvidScriptObjectRegistry.h"
#include "AvidScriptRuntimeSession.h"
#include "Components/ActorComponent.h"
#include "UObject/SoftObjectPath.h"

#include "AvidScriptComponent.generated.h"

struct FAvidScriptComponentRuntimeStats
{
	bool bOwnerRegistered = false;
	bool bOwnerReleased = false;
	bool bRuntimeLoaded = false;
	bool bBeginPlayCalled = false;
	bool bCollisionDelegatesBound = false;
	bool bComponentEndPlayObserved = false;
	bool bEndPlayCalled = false;
	int32 TickCallCount = 0;
	int32 SuccessfulReloadCount = 0;
	int32 RejectedReloadCount = 0;
	int32 TimerCallbackCount = 0;
	int32 LastTimerCallbackId = 0;
	int32 LastTimerHandle = 0;
	int32 EventCallbackCount = 0;
	int32 LastEventId = 0;
	float LastEventValue = 0.0f;
	int32 LastInputActionId = 0;
	int32 LastInputTriggerEvent = 0;
	FVector LastInputValue = FVector::ZeroVector;
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

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Events")
	bool DispatchScriptEvent(int32 EventId, float Value);

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Input")
	bool DispatchScriptInput(int32 ActionId, int32 TriggerEvent, FVector Value);

	bool ReloadConfiguredScript(FAvidScriptWasmReloadResult& OutResult);

	UFUNCTION(BlueprintCallable, Category = "AvidScript|Runtime")
	bool ReloadScript();

#if WITH_DEV_AUTOMATION_TESTS
	FAvidScriptRuntimeSession* GetRuntimeSessionForTesting() const { return RuntimeSession.Get(); }
#endif

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(
		float DeltaTime,
		ELevelTick TickType,
		FActorComponentTickFunction* ThisTickFunction) override;

private:
	bool RegisterOwner();
	void ReleaseOwner();
	void BindOwnerGameplayDelegates();
	void UnbindOwnerGameplayDelegates();
	bool DispatchGameplayEvent(const FAvidScriptGameplayEvent& Event);
	bool DispatchOwnerGameplayEvent(
		EAvidScriptGameplayEventType EventType,
		AActor* OtherActor,
		const FVector& VectorValue);
	void ReleaseGameplayObjectHandles();

	UFUNCTION()
	void HandleOwnerBeginOverlap(AActor* OverlappedActor, AActor* OtherActor);

	UFUNCTION()
	void HandleOwnerEndOverlap(AActor* OverlappedActor, AActor* OtherActor);

	UFUNCTION()
	void HandleOwnerHit(AActor* SelfActor, AActor* OtherActor, FVector NormalImpulse, const FHitResult& Hit);

	bool LoadConfiguredScriptModule(FAvidScriptWasmSmokeResult& OutResult);
	FString ResolveScriptManifestPath() const;
	void RecordRuntimeFailure(const FAvidScriptWasmSmokeResult& Result);
	void ReleaseRuntime(FAvidScriptWasmSmokeResult* OutUnloadResult = nullptr);
	void FlushDeferredRuntimeRelease();

	UPROPERTY(EditAnywhere, Category = "AvidScript", meta = (FilePathFilter = "avidscript.json"))
	FFilePath ScriptManifestFile;

	FAvidScriptObjectRegistry ObjectRegistry;
	FAvidScriptObjectHandle OwnerHandle;
	TSet<uint64> GameplayObjectHandleValues;
	TUniquePtr<FAvidScriptRuntimeSession> RuntimeSession;
	FAvidScriptComponentRuntimeStats RuntimeStats;
	bool bRuntimeReleaseDeferred = false;
	bool bOwnerReleaseDeferred = false;
	bool bRuntimeReleaseInProgress = false;
};
