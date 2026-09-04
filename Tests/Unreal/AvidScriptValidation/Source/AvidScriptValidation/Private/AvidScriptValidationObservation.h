#pragma once

#include "AvidScriptRuntimeSession.h"
#include "AvidScriptVmBackend.h"
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class AActor;
class UAvidScriptComponent;
class UButton;
class UUserWidget;
class UWorld;

namespace AvidScript::Validation
{
inline constexpr TCHAR UiSaveMap[] = TEXT("/AvidScript/Demos/UiSave/L_UiSave");
inline constexpr TCHAR UiSaveModule[] = TEXT("avidscript.ui_save_demo");

struct FUiSaveObservation
{
	FUiSaveObservation();
	// False with no error means initial activation or a safe snapshot is still pending.
	bool Refresh(const FString& ExpectedPackage, bool bRequireStable, FString& Error);
	bool CheckResources(FString& Error) const;
	bool CheckEvents(int32 Expected, FString& Error) const;
	UButton* FindButton(FName Name) const;

	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AActor> Host;
	TWeakObjectPtr<UAvidScriptComponent> Component;
	TWeakObjectPtr<UUserWidget> Widget;
	FString Map;
	FString Score;
	FString Status;
	FAvidScriptRuntimeSessionSnapshot Snapshot;
	FAvidScriptVmBackendInfo BackendInfo;
	int32 Events = 0;
	int32 BoundButtons = 0;
	bool bCaptured = false;
	TSharedRef<FJsonObject> Runtime = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Startup = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Backend = MakeShared<FJsonObject>();
	TSharedRef<FJsonObject> Resources = MakeShared<FJsonObject>();

private:
	void CaptureValues();
};
}
