#pragma once

#include "AvidScriptValidationObservation.h"
#include "AvidScriptValidationPaths.h"
#include "AvidScriptValidationProbe.h"
#include "Containers/Ticker.h"

namespace AvidScript::Validation
{
class FUiSavePackagedProbe final : public IUiSavePackagedProbe
{
public:
	~FUiSavePackagedProbe() override;
	void Start() override;

private:
	struct FStep
	{
		FString Action;
		FName Button;
		FString Score;
		FString Status;
	};
	bool Initialize(FString& Error);
	bool Tick(float DeltaSeconds);
	bool CheckSaveState(bool bRequireFinal, FString& Error);
	void Finish(bool bSucceeded, const FString& Failure);
	bool WriteReport(bool bSucceeded, const FString& Failure, FString& Error) const;

	static constexpr double TimeoutSeconds = 90.0;
	FTSTicker::FDelegateHandle Ticker;
	FUiSavePaths Paths;
	FUiSaveObservation Observation;
	FString Mode;
	FString ExpectedPackage;
	FString RunId;
	FString StartedUtc;
	double Started = 0.0;
	TArray<FStep> Steps;
	TArray<TSharedPtr<FJsonValue>> Actions;
	int32 StepIndex = 0;
	uint64 DispatchFrame = 0;
	bool bInitialized = false;
	bool bDispatched = false;
	bool bSaveDispatched = false;
	bool bFinished = false;
};
}
