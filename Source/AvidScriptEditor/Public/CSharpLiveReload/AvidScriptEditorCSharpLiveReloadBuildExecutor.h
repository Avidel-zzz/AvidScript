#pragma once

#include "AvidScriptEditorComponentBindingService.h"
#include "AvidScriptEditorCSharpProfileService.h"

#include "CoreMinimal.h"

class AActor;

enum class EAvidScriptEditorCSharpLiveReloadBuildStatus : uint8
{
	Unknown,
	Succeeded,
	TargetUnavailable,
	ProfileFailed,
	BuildFailed,
	BindingFailed
};

struct FAvidScriptEditorCSharpLiveReloadBuildResult
{
	bool bSucceeded = false;
	EAvidScriptEditorCSharpLiveReloadBuildStatus Status =
		EAvidScriptEditorCSharpLiveReloadBuildStatus::Unknown;
	FString ErrorCategory;
	FString CauseErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString ProfilePath;
	FString TargetActorPath;
	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	FAvidScriptEditorCSharpBuildResult BuildResult;
	FAvidScriptEditorComponentBindingResult BindingResult;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpLiveReloadBuildExecutor
{
public:
	using FLoadProfile = TFunction<bool(
		const FString&,
		FAvidScriptEditorCSharpProfileLoadResult&)>;
	using FBuildProfile = TFunction<bool(
		const FAvidScriptEditorCSharpBuildConfig&,
		FAvidScriptEditorCSharpBuildResult&)>;
	using FApplyReport = TFunction<bool(
		const FString&,
		AActor*,
		FAvidScriptEditorComponentBindingResult&)>;

	FAvidScriptEditorCSharpLiveReloadBuildExecutor();

	FAvidScriptEditorCSharpLiveReloadBuildExecutor(
		FLoadProfile InLoadProfile,
		FBuildProfile InBuildProfile,
		FApplyReport InApplyReport);

	bool Execute(
		const FString& ProfilePath,
		AActor* TargetActor,
		FAvidScriptEditorCSharpLiveReloadBuildResult& OutResult) const;

private:
	FLoadProfile LoadProfile;
	FBuildProfile BuildProfile;
	FApplyReport ApplyReport;
};
