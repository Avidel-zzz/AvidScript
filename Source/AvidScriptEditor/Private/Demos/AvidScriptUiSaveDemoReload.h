#pragma once

#include "CoreMinimal.h"
#include "CoreGlobals.h"
#include "Demos/AvidScriptUiSaveDemoProbe.h"
#include "Dom/JsonObject.h"

class AActor;
class UAvidScriptComponent;
class UUserWidget;
class FAvidScriptRuntimeSession;
class FJsonObject;
class FJsonValue;

namespace AvidScript::UiSaveDemo
{
class FUiSaveReload
{
public:
	bool Initialize(FString& Error);
	void AppendSteps(TArray<FProbeStep>& Steps) const;
	bool ValidateRuntime(UAvidScriptComponent& Component, FString& Error) const;
	bool BeforeStep(const FProbeStep& Step, AActor* Host, UAvidScriptComponent* Component,
		UUserWidget* Widget, FJsonObject& Action, FString& Error);
	bool AfterStep(const FProbeStep& Step, AActor* Host, UAvidScriptComponent* Component,
		UUserWidget* Widget, FJsonObject& Action, FString& Error);
	bool RefreshStopped(AActor* Host, UAvidScriptComponent* Component, UUserWidget* Widget, FString& Error);
	bool Finish(FString& Error);
	bool IsStopped() const { return bStopped; }
	bool IsLoose() const { return bLoose; }
	bool CanObserveStep() const { return GFrameCounter > StepFrame + 1; }
	double GetTimeoutSeconds() const { return 30.0 + 6.0 * Cycles; }
	TSharedRef<FJsonObject> GetReport() const { return Report; }

private:
	struct FArtifact
	{
		FString ManifestPath;
		FString ManifestHash;
		FString WasmPath;
		FString WasmHash;
	};
	bool CheckArtifacts(FString& Error) const;
	bool CaptureResources(UAvidScriptComponent& Component, UUserWidget& Widget,
		TSharedRef<FJsonObject> Destination, bool bEstablishBaseline, FString& Error);
	FArtifact Artifacts[3];
	int32 Cycles = 20;
	int32 CurrentCycle = 0;
	int32 ActiveArtifact = 0;
	int32 InitialSuccesses = 0;
	int32 InitialRejections = 0;
	int32 EventsBeforeStop = 0;
	bool bLoose = false;
	bool bStopped = false;
	bool bHasConfiguration = false;
	FName OriginalModuleId;
	FString OriginalManifestPath;
	FString ExpectedRejectionError;
	TWeakObjectPtr<UAvidScriptComponent> ConfiguredComponent;
	const FAvidScriptRuntimeSession* OriginalSession = nullptr;
	uint64 StepFrame = 0;
	double Started = 0.0;
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> ResourceBaseline;
	TSharedPtr<FJsonObject> CycleRecord;
	TArray<TSharedPtr<FJsonValue>> Records;
};
}
