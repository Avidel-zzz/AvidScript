#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadCompletion.h"

namespace
{
void SetAvidScriptLiveReloadCompletionFailure(
	const EAvidScriptEditorCSharpLiveReloadBuildStatus Status,
	const FString& ErrorCategory,
	const FString& CauseErrorCategory,
	const FString& ErrorMessage,
	const FString& NextAction,
	FAvidScriptEditorCSharpLiveReloadBuildResult& OutResult)
{
	OutResult.bSucceeded = false;
	OutResult.Status = Status;
	OutResult.ErrorCategory = ErrorCategory;
	OutResult.CauseErrorCategory = CauseErrorCategory;
	OutResult.ErrorMessage = ErrorMessage;
	OutResult.NextAction = NextAction;
}
} // namespace

void FAvidScriptEditorCSharpLiveReloadCompletion::FromAsyncBuild(
	const FAvidScriptEditorCSharpAsyncBuildResult& AsyncResult,
	const FString& TargetActorPath,
	FAvidScriptEditorCSharpLiveReloadBuildResult& OutResult)
{
	OutResult = FAvidScriptEditorCSharpLiveReloadBuildResult();
	OutResult.ProfilePath = AsyncResult.ProfilePath;
	OutResult.TargetActorPath = TargetActorPath;
	OutResult.ProfileResult = AsyncResult.ProfileResult;
	OutResult.BuildResult = AsyncResult.BuildResult;
	if (AsyncResult.bSucceeded && AsyncResult.BuildResult.bSucceeded)
	{
		return;
	}

	const bool bProfileFailed = !AsyncResult.ProfileResult.bSucceeded
		&& !AsyncResult.ProfileResult.ErrorCategory.IsEmpty();
	const FString CauseCategory = !AsyncResult.ErrorCategory.IsEmpty()
		? AsyncResult.ErrorCategory
		: AsyncResult.BuildResult.ErrorCategory;
	SetAvidScriptLiveReloadCompletionFailure(
		bProfileFailed
			? EAvidScriptEditorCSharpLiveReloadBuildStatus::ProfileFailed
			: EAvidScriptEditorCSharpLiveReloadBuildStatus::BuildFailed,
		bProfileFailed
			? FString(TEXT("live_reload_profile_failed"))
			: FString(TEXT("live_reload_build_failed")),
		CauseCategory,
		AsyncResult.ErrorMessage.IsEmpty()
			? FString(TEXT("C# live reload asynchronous build failed."))
			: AsyncResult.ErrorMessage,
		AsyncResult.NextAction.IsEmpty()
			? FString(TEXT("fix the C# diagnostic and save the script again"))
			: AsyncResult.NextAction,
		OutResult);
}

void FAvidScriptEditorCSharpLiveReloadCompletion::FromBinding(
	const bool bApplySucceeded,
	FAvidScriptEditorComponentBindingResult&& BindingResult,
	FAvidScriptEditorCSharpLiveReloadBuildResult& OutResult)
{
	OutResult.BindingResult = MoveTemp(BindingResult);
	if (!bApplySucceeded || !OutResult.BindingResult.bSucceeded)
	{
		SetAvidScriptLiveReloadCompletionFailure(
			EAvidScriptEditorCSharpLiveReloadBuildStatus::BindingFailed,
			TEXT("live_reload_binding_failed"),
			OutResult.BindingResult.ErrorCategory,
			OutResult.BindingResult.ErrorMessage.IsEmpty()
				? FString(TEXT("C# live reload could not apply the report to the fixed Actor."))
				: OutResult.BindingResult.ErrorMessage,
			OutResult.BindingResult.NextAction.IsEmpty()
				? FString(TEXT("repair the candidate manifest or restart live reload"))
				: OutResult.BindingResult.NextAction,
			OutResult);
		return;
	}

	OutResult.bSucceeded = true;
	OutResult.Status = EAvidScriptEditorCSharpLiveReloadBuildStatus::Succeeded;
}
