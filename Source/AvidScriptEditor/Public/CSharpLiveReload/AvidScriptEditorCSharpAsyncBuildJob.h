#pragma once

#include "AvidScriptEditorCSharpProfileService.h"

#include "CoreMinimal.h"

enum class EAvidScriptEditorCSharpAsyncBuildStage : uint8
{
	Idle,
	Preparing,
	BootstrapRunning,
	PublishingBindingSlice,
	FinalRunning,
	ReadyToBind,
	Failed,
	Canceled
};

struct FAvidScriptEditorCSharpAsyncBuildProgress
{
	EAvidScriptEditorCSharpAsyncBuildStage Stage =
		EAvidScriptEditorCSharpAsyncBuildStage::Idle;
	double ElapsedSeconds = 0.0;
	FString LatestOutputLine;
	int32 OutputLineCount = 0;
	bool bCancelRequested = false;
};

struct FAvidScriptEditorCSharpAsyncBuildResult
{
	bool bSucceeded = false;
	FString ErrorCategory;
	FString ErrorMessage;
	FString NextAction;
	FString ProfilePath;
	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	FAvidScriptEditorCSharpBuildResult BuildResult;
};

class IAvidScriptEditorCSharpAsyncBuildJob
{
public:
	virtual ~IAvidScriptEditorCSharpAsyncBuildJob() = default;

	virtual bool Start(const FString& ProfilePath) = 0;

	virtual void Tick() = 0;

	virtual void Cancel() = 0;

	virtual bool IsFinished() const = 0;

	virtual const FAvidScriptEditorCSharpAsyncBuildProgress&
		GetProgress() const = 0;

	virtual bool ConsumeResult(
		FAvidScriptEditorCSharpAsyncBuildResult& OutResult) = 0;
};

class AVIDSCRIPTEDITOR_API FAvidScriptEditorCSharpAsyncBuildJobFactory
{
public:
	static TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob> Create();
};
