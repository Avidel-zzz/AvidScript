#pragma once

#include "CoreMinimal.h"
#include "Demos/AvidScriptUiSaveDemoProbe.h"
#include "Dom/JsonObject.h"
#include "UObject/ObjectKey.h"

class AActor;
class UAvidScriptComponent;
class UUserWidget;
class UWorld;

namespace AvidScript::UiSaveDemo
{
enum class EWorldProbeResult : uint8 { Pending, Succeeded, Failed };

class FUiSaveWorld
{
public:
	FUiSaveWorld(FString InPackage, FString InSavePath, TFunction<bool()> InValidateDirectory);
	~FUiSaveWorld();
	bool Initialize(FString& Error);
	EWorldProbeResult Tick(FString& Error);
	void AppendReport(FJsonObject& Report) const;
	double GetTimeoutSeconds() const { return SoakSeconds + FMath::Max(120.0, RequestedCycles * 20.0); }

private:
	enum class EStage : uint8 { AwaitWorld, Actions, Dwell, Travelling, Complete };
	bool FindReadyWorld(UWorld*& OutWorld, AActor*& OutHost, UAvidScriptComponent*& OutComponent,
		UUserWidget*& OutWidget, FString& Error);
	bool AcceptWorld(UWorld& NewWorld, AActor& NewHost, UAvidScriptComponent& NewComponent,
		UUserWidget& NewWidget, FString& Error);
	bool TickAction(FString& Error);
	bool CaptureResources(FString& Error);
	bool CheckSaveFile(FString& Error) const;
	bool ReadText(FString& Score, FString& Status) const;
	void BeginTravel();
	void HandleWorldCleanup(UWorld* CleanedWorld, bool bSessionEnded, bool bCleanupResources);
	void CaptureRuntime(FJsonObject& Runtime, FJsonObject& Startup) const;
	void AddMemorySample(FJsonObject& Gc);

	FString ExpectedPackage;
	FString SavePath;
	FString SaveHash;
	FString Failure;
	int64 SaveBytes = 0;
	TFunction<bool()> ValidateDirectory;
	FDelegateHandle CleanupHandle;
	EStage Stage = EStage::AwaitWorld;
	double Started = 0.0;
	double StageStarted = 0.0;
	double Finished = 0.0;
	int32 RequestedCycles = 10;
	int32 SoakSeconds = 0;
	int32 CompletedCycles = 0;
	int32 ActivatedWorlds = 0;
	int32 TravelCount = 0;
	int32 CleanupCount = 0;
	int32 StepIndex = 0;
	uint64 DispatchFrame = 0;
	bool bDispatched = false;
	bool bFinalRecovery = false;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AActor> Host;
	TWeakObjectPtr<UAvidScriptComponent> Component;
	TWeakObjectPtr<UUserWidget> Widget;
	TWeakObjectPtr<UObject> SavedObject;
	FObjectKey PreviousWorld;
	FObjectKey PreviousHost;
	FObjectKey PreviousComponent;
	TArray<FProbeStep> Steps;
	TArray<TSharedPtr<FJsonValue>> Cycles;
	TArray<TSharedPtr<FJsonValue>> Actions;
	TSharedPtr<FJsonObject> CurrentCycle;
	TSharedRef<FJsonObject> MemorySummary = MakeShared<FJsonObject>();
	uint64 PeakPhysical = 0;
	uint64 PeakVirtual = 0;
	uint64 RetainedCycleJsonBytes = 0;
	int64 RetainedActionCount = 0;
};
}
