#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildBackend.h"

#include "AvidScriptEditorCSharpProfileService.h"
#include "CSharpBuild/AvidScriptEditorCSharpBuildPipeline.h"

#include "Misc/Paths.h"

namespace
{
FString NormalizeAvidScriptCSharpAsyncBackendPath(FString Path)
{
	if (!Path.IsEmpty())
	{
		Path = FPaths::ConvertRelativePathToFull(Path);
		FPaths::NormalizeFilename(Path);
	}
	return Path;
}

void SetAvidScriptCSharpAsyncBackendFailure(
	const FString& ProfilePath,
	const FAvidScriptEditorCSharpProfileLoadResult& ProfileResult,
	const FAvidScriptEditorCSharpBuildResult& BuildResult,
	FAvidScriptEditorCSharpAsyncBuildBackendStep& OutStep)
{
	OutStep.NextStage = EAvidScriptEditorCSharpAsyncBuildStage::Failed;
	OutStep.Result.bSucceeded = false;
	OutStep.Result.ProfilePath = ProfilePath;
	OutStep.Result.ProfileResult = ProfileResult;
	OutStep.Result.BuildResult = BuildResult;
	OutStep.Result.ErrorCategory = BuildResult.ErrorCategory.IsEmpty()
		? FString(TEXT("live_reload_build_failed"))
		: BuildResult.ErrorCategory;
	OutStep.Result.ErrorMessage = BuildResult.ErrorMessage.IsEmpty()
		? FString(TEXT("C# asynchronous build failed."))
		: BuildResult.ErrorMessage;
	OutStep.Result.NextAction = BuildResult.NextAction;
}

class FAvidScriptEditorCSharpAsyncBuildBackend final
	: public IAvidScriptEditorCSharpAsyncBuildBackend
{
public:
	virtual ~FAvidScriptEditorCSharpAsyncBuildBackend() override
	{
		Cleanup();
	}

	virtual FAvidScriptEditorCSharpAsyncBuildBackendStep Prepare(
		const FString& ProfilePath) override
	{
		Cleanup();
		NormalizedProfilePath =
			NormalizeAvidScriptCSharpAsyncBackendPath(ProfilePath);
		ProfileResult = FAvidScriptEditorCSharpProfileLoadResult();

		FAvidScriptEditorCSharpAsyncBuildBackendStep Step;
		if (!FAvidScriptEditorCSharpProfileService::LoadProfile(
				NormalizedProfilePath,
				ProfileResult)
			|| !ProfileResult.bSucceeded)
		{
			Step.NextStage =
				EAvidScriptEditorCSharpAsyncBuildStage::Failed;
			Step.Result.ProfilePath = NormalizedProfilePath;
			Step.Result.ProfileResult = ProfileResult;
			Step.Result.ErrorCategory =
				ProfileResult.ErrorCategory.IsEmpty()
					? FString(TEXT("live_reload_profile_failed"))
					: ProfileResult.ErrorCategory;
			Step.Result.ErrorMessage =
				ProfileResult.ErrorMessage.IsEmpty()
					? FString(TEXT("C# asynchronous build profile could not be loaded."))
					: ProfileResult.ErrorMessage;
			Step.Result.NextAction = ProfileResult.NextAction;
			return Step;
		}
		if (!ProfileResult.NormalizedProfilePath.IsEmpty())
		{
			NormalizedProfilePath =
				ProfileResult.NormalizedProfilePath;
		}

		FAvidScriptEditorCSharpBuildResult PrepareResult;
		if (!FAvidScriptEditorCSharpBuildPipeline::Prepare(
				FAvidScriptEditorCSharpProfileService::MakeBuildRequest(ProfileResult),
				Plan,
				PrepareResult))
		{
			SetAvidScriptCSharpAsyncBackendFailure(
				NormalizedProfilePath,
				ProfileResult,
				PrepareResult,
				Step);
			return Step;
		}
		bPlanPrepared = true;

		const EAvidScriptEditorCSharpAsyncBuildStage InvocationStage =
			Plan.bAutomaticBindingSlice
				? EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning
				: EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning;
		const FAvidScriptEditorCSharpBuildConfig& InvocationConfig =
			Plan.bAutomaticBindingSlice
				? Plan.BootstrapConfig
				: Plan.FinalConfig;
		FAvidScriptEditorCSharpBuildResult InvocationPrepareResult;
		if (!FAvidScriptEditorCSharpBuildInvoker::Prepare(
				InvocationConfig,
				Step.Invocation,
				InvocationPrepareResult))
		{
			HandleInvocationPreparationFailure(
				InvocationStage,
				InvocationPrepareResult,
				Step);
			return Step;
		}
		Step.NextStage = InvocationStage;
		return Step;
	}

	virtual FAvidScriptEditorCSharpAsyncBuildBackendStep CompleteInvocation(
		const EAvidScriptEditorCSharpAsyncBuildStage Stage,
		const FAvidScriptEditorCSharpBuildInvocation& Invocation,
		const FAvidScriptEditorCSharpBuildProcessSnapshot& ProcessSnapshot) override
	{
		FAvidScriptEditorCSharpAsyncBuildBackendStep Step;
		FAvidScriptEditorCSharpBuildResult InvocationResult;
		FAvidScriptEditorCSharpBuildInvoker::Finalize(
			Invocation,
			ProcessSnapshot.ProcessExitCode,
			ProcessSnapshot.Stdout,
			FString(),
			InvocationResult);

		if (Stage == EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning)
		{
			FAvidScriptEditorCSharpBuildResult BootstrapOutcome;
			if (!FAvidScriptEditorCSharpBuildPipeline::CompleteBootstrap(
					Plan,
					InvocationResult,
					BootstrapOutcome))
			{
				SetAvidScriptCSharpAsyncBackendFailure(
					NormalizedProfilePath,
					ProfileResult,
					BootstrapOutcome,
					Step);
				return Step;
			}

			FAvidScriptEditorCSharpBuildResult FinalPrepareResult;
			if (!FAvidScriptEditorCSharpBuildInvoker::Prepare(
					Plan.FinalConfig,
					Step.Invocation,
					FinalPrepareResult))
			{
				HandleInvocationPreparationFailure(
					EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning,
					FinalPrepareResult,
					Step);
				return Step;
			}
			Step.NextStage =
				EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning;
			return Step;
		}

		FAvidScriptEditorCSharpBuildResult FinalOutcome;
		if (!FAvidScriptEditorCSharpBuildPipeline::CompleteFinal(
				Plan,
				InvocationResult,
				FinalOutcome))
		{
			SetAvidScriptCSharpAsyncBackendFailure(
				NormalizedProfilePath,
				ProfileResult,
				FinalOutcome,
				Step);
			return Step;
		}

		Step.NextStage =
			EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind;
		Step.Result.bSucceeded = true;
		Step.Result.ProfilePath = NormalizedProfilePath;
		Step.Result.ProfileResult = ProfileResult;
		Step.Result.BuildResult = MoveTemp(FinalOutcome);
		return Step;
	}

	virtual void Cleanup() override
	{
		if (bPlanPrepared)
		{
			FAvidScriptEditorCSharpBuildPipeline::Cleanup(Plan);
		}
		Plan = FAvidScriptEditorCSharpBuildPlan();
		bPlanPrepared = false;
	}

private:
	void HandleInvocationPreparationFailure(
		const EAvidScriptEditorCSharpAsyncBuildStage Stage,
		const FAvidScriptEditorCSharpBuildResult& InvocationPrepareResult,
		FAvidScriptEditorCSharpAsyncBuildBackendStep& OutStep)
	{
		FAvidScriptEditorCSharpBuildResult PipelineOutcome;
		if (Stage == EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning)
		{
			FAvidScriptEditorCSharpBuildPipeline::CompleteBootstrap(
				Plan,
				InvocationPrepareResult,
				PipelineOutcome);
		}
		else
		{
			FAvidScriptEditorCSharpBuildPipeline::CompleteFinal(
				Plan,
				InvocationPrepareResult,
				PipelineOutcome);
		}
		SetAvidScriptCSharpAsyncBackendFailure(
			NormalizedProfilePath,
			ProfileResult,
			PipelineOutcome,
			OutStep);
	}

	FString NormalizedProfilePath;
	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	FAvidScriptEditorCSharpBuildPlan Plan;
	bool bPlanPrepared = false;
};
} // namespace

TUniquePtr<IAvidScriptEditorCSharpAsyncBuildBackend>
CreateAvidScriptEditorCSharpAsyncBuildBackend()
{
	return MakeUnique<FAvidScriptEditorCSharpAsyncBuildBackend>();
}
