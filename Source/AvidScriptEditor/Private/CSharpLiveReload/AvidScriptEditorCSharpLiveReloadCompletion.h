#pragma once

#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJob.h"
#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadBuildExecutor.h"

class FAvidScriptEditorCSharpLiveReloadCompletion
{
public:
	static void FromAsyncBuild(
		const FAvidScriptEditorCSharpAsyncBuildResult& AsyncResult,
		const FString& TargetActorPath,
		FAvidScriptEditorCSharpLiveReloadBuildResult& OutResult);

	static void FromBinding(
		bool bApplySucceeded,
		FAvidScriptEditorComponentBindingResult&& BindingResult,
		FAvidScriptEditorCSharpLiveReloadBuildResult& OutResult);
};
