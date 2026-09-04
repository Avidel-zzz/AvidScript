#pragma once

#include "AvidScriptValidationObservation.h"
#include "AvidScriptValidationPaths.h"
#include "AvidScriptValidationProbe.h"
#include "AvidScriptVmDiagnostics.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "UObject/ObjectKey.h"

class AActor;
class UAvidScriptComponent;
class UObject;
class UUserWidget;
class UWorld;

namespace AvidScript::Validation
{
class FUiSavePackagedWorldProbe final : public IUiSavePackagedProbe
{
public:
	~FUiSavePackagedWorldProbe() override;
	void Start() override;

private:
	enum class EStage : uint8
	{
		AwaitWorld,
		Actions,
		Dwell,
		Travelling,
		Complete
	};

	struct FStep
	{
		FString Action;
		FName Button;
		FString Score;
		FString Status;
		int32 Events = 0;
	};

	bool Initialize(FString& Error);
	bool Tick(float DeltaSeconds);
	bool FindReadyObservation(FUiSaveObservation& Candidate, FString& Error) const;
	bool AcceptWorld(FUiSaveObservation&& Candidate, FString& Error);
	bool TickAction(FString& Error);
	bool ValidateSaveUnchanged(FString& Error);
	bool CompleteSaveAction(FString& Error);
	bool CaptureResources(FString& Error);
	bool CaptureMemorySample(bool bFinalCycle, FString& Error);
	bool CountActiveSessions(int32& OutCount, FString& Error) const;
	void AdvanceEvidenceChain(
		uint64 PhysicalBytes,
		uint64 VirtualBytes,
		int32 UObjectCount,
		const FAvidScriptVmMemorySnapshot& Vm);
	void BeginTravel();
	void HandleWorldCleanup(UWorld* CleanedWorld, bool bSessionEnded, bool bCleanupResources);
	void Finish(bool bSucceeded, const FString& Failure);
	bool WriteReport(bool bSucceeded, const FString& Failure, FString& Error) const;
	TSharedRef<FJsonObject> BuildFailureCycle() const;
	uint64 EstimateObserverBytes() const;
	double GetTimeoutSeconds() const;

	static constexpr int32 MaximumCycles = 10000;
	static constexpr int32 MaximumSamples = 15;
	static constexpr double MinimumDwellSeconds = 3.0;

	FTSTicker::FDelegateHandle Ticker;
	FDelegateHandle CleanupHandle;
	FUiSavePaths Paths;
	FUiSaveObservation Observation;
	EStage Stage = EStage::AwaitWorld;
	FString ExpectedPackage;
	FString RunId;
	FString StartedUtc;
	FString EvidenceChain;
	FString SaveHash;
	FString Failure;
	double Started = 0.0;
	double Finished = 0.0;
	double StageStarted = 0.0;
	int64 SaveBytes = 0;
	int32 RequestedCycles = 10;
	int32 SoakSeconds = 0;
	int32 CompletedCycles = 0;
	int32 ActivatedWorlds = 0;
	int32 TravelCount = 0;
	int32 CleanupCount = 0;
	int32 ActionChecksPassed = 0;
	int32 CleanupChecksPassed = 0;
	int32 GcChecksPassed = 0;
	int32 ResourceChecksPassed = 0;
	int32 StepIndex = 0;
	int32 CurrentCycleNumber = 0;
	int32 CurrentCycleEvents = 0;
	uint64 DispatchFrame = 0;
	bool bInitialized = false;
	bool bDispatched = false;
	bool bFinalRecovery = false;
	bool bCleanupObserved = false;
	bool bFinished = false;

	TArray<FStep> Steps;
	TArray<TSharedPtr<FJsonValue>> CurrentActions;
	TArray<TSharedPtr<FJsonValue>> Samples;
	TWeakObjectPtr<UObject> ActiveSavedObject;
	TWeakObjectPtr<UWorld> RetiredWorld;
	TWeakObjectPtr<AActor> RetiredHost;
	TWeakObjectPtr<UAvidScriptComponent> RetiredComponent;
	TWeakObjectPtr<UUserWidget> RetiredWidget;
	TWeakObjectPtr<UObject> RetiredSavedObject;
	FObjectKey PreviousWorld;
	FObjectKey PreviousHost;
	FObjectKey PreviousComponent;

	bool bBaselineAvailable = false;
	uint64 BaselinePhysical = 0;
	uint64 BaselineVirtual = 0;
	uint64 FinalPhysical = 0;
	uint64 FinalVirtual = 0;
	uint64 PeakPhysical = 0;
	uint64 PeakVirtual = 0;
	uint64 BaselineBackendLive = 0;
	int32 BaselineArtifactEntries = 0;
	int32 BaselineAttestationEntries = 0;
	uint64 BaselineArtifactBytes = 0;
	uint64 BaselineAttestationBytes = 0;
};
}
