#pragma once

#include "CSharpBuild/AvidScriptEditorCSharpBuildInvoker.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildProcess.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJob.h"

struct FAvidScriptEditorCSharpAsyncBuildBackendStep
{
	EAvidScriptEditorCSharpAsyncBuildStage NextStage =
		EAvidScriptEditorCSharpAsyncBuildStage::Failed;
	FAvidScriptEditorCSharpBuildInvocation Invocation;
	FAvidScriptEditorCSharpAsyncBuildResult Result;
};

class IAvidScriptEditorCSharpAsyncBuildBackend
{
public:
	virtual ~IAvidScriptEditorCSharpAsyncBuildBackend() = default;

	virtual FAvidScriptEditorCSharpAsyncBuildBackendStep Prepare(
		const FString& ProfilePath) = 0;

	virtual FAvidScriptEditorCSharpAsyncBuildBackendStep CompleteInvocation(
		EAvidScriptEditorCSharpAsyncBuildStage Stage,
		const FAvidScriptEditorCSharpBuildInvocation& Invocation,
		const FAvidScriptEditorCSharpBuildProcessSnapshot& ProcessSnapshot) = 0;

	virtual void Cleanup() = 0;
};

TUniquePtr<IAvidScriptEditorCSharpAsyncBuildBackend>
CreateAvidScriptEditorCSharpAsyncBuildBackend();
