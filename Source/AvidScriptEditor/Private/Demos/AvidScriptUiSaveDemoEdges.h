#pragma once

#include "Demos/AvidScriptUiSaveDemoFixtures.h"
#include "Demos/AvidScriptUiSaveDemoProbe.h"
#include "Dom/JsonObject.h"

class AActor;
class UAvidScriptComponent;
class UUserWidget;

namespace AvidScript::UiSaveDemo
{
class FUiSaveEdges
{
public:
	FUiSaveEdges(const FString& SavePath, TFunction<bool()> CheckSafe);
	static void AppendSteps(TArray<FProbeStep>& Steps);
	bool BeforeStep(const FProbeStep& Step, AActor* Host, UAvidScriptComponent* Component,
		UUserWidget* Widget, FJsonObject& Action, FString& Error);
	bool AfterStep(const FProbeStep& Step, AActor* Host, UAvidScriptComponent* Component,
		UUserWidget* Widget, FJsonObject& Action, FString& Error);
	bool RefreshStopped(AActor* Host, UAvidScriptComponent* Component, UUserWidget* Widget, FString& Error);
	bool IsStopped() const { return bStopped; }
	const FString& GetExpectedHash() const { return Fixtures.GetExpectedHash(); }
	TSharedRef<FJsonObject> GetReport();
	void Unlock() { Fixtures.Unlock(); }

private:
	FSaveFixtureController Fixtures;
	TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
	TWeakObjectPtr<UObject> SavedBeforeInvalidLoad;
	int32 InvalidLoadsPreserved = 0;
	int32 EventsBeforeTeardown = 0;
	int32 EventsBeforeLate = 0;
	bool bStopped = false;
};
}
