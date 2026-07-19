#pragma once

#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildBackend.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildProcess.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJob.h"

class FAvidScriptEditorCSharpAsyncBuildJob final
	: public IAvidScriptEditorCSharpAsyncBuildJob
{
public:
	using FCreateProcess =
		TFunction<TUniquePtr<IAvidScriptEditorCSharpBuildProcess>()>;
	using FNowSeconds = TFunction<double()>;

	FAvidScriptEditorCSharpAsyncBuildJob(
		TUniquePtr<IAvidScriptEditorCSharpAsyncBuildBackend> InBackend,
		FCreateProcess InCreateProcess,
		FNowSeconds InNowSeconds);

	virtual ~FAvidScriptEditorCSharpAsyncBuildJob() override;

	virtual bool Start(const FString& ProfilePath) override;

	virtual void Tick() override;

	virtual void Cancel() override;

	virtual bool IsFinished() const override;

	virtual const FAvidScriptEditorCSharpAsyncBuildProgress&
		GetProgress() const override;

	virtual bool ConsumeResult(
		FAvidScriptEditorCSharpAsyncBuildResult& OutResult) override;

private:
	bool ApplyBackendStep(
		FAvidScriptEditorCSharpAsyncBuildBackendStep&& Step);
	bool LaunchInvocation(
		EAvidScriptEditorCSharpAsyncBuildStage Stage,
		FAvidScriptEditorCSharpBuildInvocation&& Invocation);
	void SetTerminal(
		EAvidScriptEditorCSharpAsyncBuildStage Stage,
		FAvidScriptEditorCSharpAsyncBuildResult&& InResult);
	void UpdateElapsedTime();

	TUniquePtr<IAvidScriptEditorCSharpAsyncBuildBackend> Backend;
	FCreateProcess CreateProcess;
	FNowSeconds NowSeconds;
	TUniquePtr<IAvidScriptEditorCSharpBuildProcess> Process;
	FAvidScriptEditorCSharpBuildInvocation ActiveInvocation;
	FAvidScriptEditorCSharpBuildInvocation PendingBootstrapInvocation;
	FAvidScriptEditorCSharpBuildProcessSnapshot PendingBootstrapSnapshot;
	FAvidScriptEditorCSharpAsyncBuildProgress Progress;
	FAvidScriptEditorCSharpAsyncBuildResult Result;
	FString RequestedProfilePath;
	double StartSeconds = 0.0;
	int32 CurrentProcessOutputLineCount = 0;
	bool bResultConsumed = false;
	bool bHasPendingBootstrapCompletion = false;
};
