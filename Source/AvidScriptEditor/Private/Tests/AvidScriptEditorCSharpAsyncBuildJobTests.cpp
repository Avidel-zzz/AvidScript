#if WITH_DEV_AUTOMATION_TESTS

#include "CSharpLiveReload/AvidScriptEditorCSharpAsyncBuildJobInternal.h"

#include "Misc/AutomationTest.h"

namespace
{
class FFakeAvidScriptCSharpBuildProcess final
	: public IAvidScriptEditorCSharpBuildProcess
{
public:
	virtual bool Launch(
		const FAvidScriptEditorCSharpBuildInvocation& Invocation,
		FString& OutErrorMessage) override
	{
		++LaunchCount;
		if (!bLaunchSucceeds)
		{
			OutErrorMessage = TEXT("fake launch rejected");
			Snapshot.State =
				EAvidScriptEditorCSharpBuildProcessState::LaunchFailed;
			return false;
		}
		Snapshot.State =
			EAvidScriptEditorCSharpBuildProcessState::Running;
		return true;
	}

	virtual bool Poll(
		FAvidScriptEditorCSharpBuildProcessSnapshot& OutSnapshot) override
	{
		OutSnapshot = Snapshot;
		return true;
	}

	virtual void Cancel() override
	{
		++CancelCount;
		Snapshot.State =
			EAvidScriptEditorCSharpBuildProcessState::Canceled;
		Snapshot.ProcessExitCode = -1;
		Snapshot.bCancelRequested = true;
	}

	virtual bool IsRunning() const override
	{
		return Snapshot.State ==
			EAvidScriptEditorCSharpBuildProcessState::Running;
	}

	void Complete(
		const int32 ExitCode,
		TArray<FString> OutputLines = {})
	{
		Snapshot.State =
			EAvidScriptEditorCSharpBuildProcessState::Completed;
		Snapshot.ProcessExitCode = ExitCode;
		Snapshot.OutputLines = MoveTemp(OutputLines);
		if (!Snapshot.OutputLines.IsEmpty())
		{
			Snapshot.LatestOutputLine =
				Snapshot.OutputLines.Last();
			Snapshot.Stdout =
				FString::Join(Snapshot.OutputLines, LINE_TERMINATOR);
			Snapshot.Stdout += LINE_TERMINATOR;
		}
	}

	bool bLaunchSucceeds = true;
	int32 LaunchCount = 0;
	int32 CancelCount = 0;
	FAvidScriptEditorCSharpBuildProcessSnapshot Snapshot;
};

class FFakeAvidScriptCSharpAsyncBuildBackend final
	: public IAvidScriptEditorCSharpAsyncBuildBackend
{
public:
	virtual FAvidScriptEditorCSharpAsyncBuildBackendStep Prepare(
		const FString& ProfilePath) override
	{
		++PrepareCount;
		FAvidScriptEditorCSharpAsyncBuildBackendStep Step;
		Step.NextStage = bAutomatic
			? EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning
			: EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning;
		Step.Invocation.ExecutablePath = TEXT("fake.exe");
		Step.Invocation.Config.ReportPath =
			TEXT("fake.report.json");
		return Step;
	}

	virtual FAvidScriptEditorCSharpAsyncBuildBackendStep CompleteInvocation(
		const EAvidScriptEditorCSharpAsyncBuildStage Stage,
		const FAvidScriptEditorCSharpBuildInvocation& Invocation,
		const FAvidScriptEditorCSharpBuildProcessSnapshot& ProcessSnapshot) override
	{
		CompletedStages.Add(Stage);
		FAvidScriptEditorCSharpAsyncBuildBackendStep Step;
		if (Stage ==
			EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning)
		{
			if (bFailBootstrap)
			{
				Step.NextStage =
					EAvidScriptEditorCSharpAsyncBuildStage::Failed;
				Step.Result.ErrorCategory =
					TEXT("fixture_bootstrap_failed");
				Step.Result.ErrorMessage =
					TEXT("fixture rejected bootstrap");
				return Step;
			}
			Step.NextStage =
				EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning;
			Step.Invocation.ExecutablePath = TEXT("fake.exe");
			Step.Invocation.Config.ReportPath =
				TEXT("fake.final.report.json");
			return Step;
		}
		if (bFailFinal)
		{
			Step.NextStage =
				EAvidScriptEditorCSharpAsyncBuildStage::Failed;
			Step.Result.ErrorCategory =
				TEXT("fixture_final_failed");
			Step.Result.ErrorMessage =
				TEXT("fixture rejected final build");
			return Step;
		}

		Step.NextStage =
			EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind;
		Step.Result.bSucceeded = true;
		Step.Result.BuildResult.bSucceeded = true;
		Step.Result.BuildResult.ReportPath =
			TEXT("fake.final.report.json");
		return Step;
	}

	virtual void Cleanup() override
	{
		++CleanupCount;
	}

	bool bAutomatic = false;
	bool bFailBootstrap = false;
	bool bFailFinal = false;
	int32 PrepareCount = 0;
	int32 CleanupCount = 0;
	TArray<EAvidScriptEditorCSharpAsyncBuildStage> CompletedStages;
};

struct FAvidScriptCSharpAsyncBuildJobFixture
{
	FAvidScriptCSharpAsyncBuildJobFixture(
		const bool bAutomatic,
		const bool bFailBootstrap = false,
		const bool bLaunchSucceeds = true)
	{
		Backend = new FFakeAvidScriptCSharpAsyncBuildBackend();
		Backend->bAutomatic = bAutomatic;
		Backend->bFailBootstrap = bFailBootstrap;
		Job = MakeUnique<FAvidScriptEditorCSharpAsyncBuildJob>(
			TUniquePtr<IAvidScriptEditorCSharpAsyncBuildBackend>(
				Backend),
			[this, bLaunchSucceeds]()
			{
				FFakeAvidScriptCSharpBuildProcess* NewProcess =
					new FFakeAvidScriptCSharpBuildProcess();
				NewProcess->bLaunchSucceeds = bLaunchSucceeds;
				Processes.Add(NewProcess);
				return TUniquePtr<IAvidScriptEditorCSharpBuildProcess>(
					NewProcess);
			},
			[this]()
			{
				return NowSeconds;
			});
	}

	FFakeAvidScriptCSharpAsyncBuildBackend* Backend = nullptr;
	TArray<FFakeAvidScriptCSharpBuildProcess*> Processes;
	TUniquePtr<FAvidScriptEditorCSharpAsyncBuildJob> Job;
	double NowSeconds = 1.0;
};
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpAsyncBuildJobDirectTest,
	"AvidScript.Editor.CSharpAsyncBuildJob.DirectFinalProgress",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpAsyncBuildJobDirectTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptCSharpAsyncBuildJobFixture Fixture(false);
	TestTrue(TEXT("Direct job starts"), Fixture.Job->Start(TEXT("direct.json")));
	TestEqual(
		TEXT("Direct job starts final process"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning);
	TestEqual(TEXT("Direct job creates one process"), Fixture.Processes.Num(), 1);
	TestFalse(TEXT("Direct job is not finished while process runs"), Fixture.Job->IsFinished());

	Fixture.Processes[0]->Complete(
		0,
		{TEXT("frontend"), TEXT("wasm")});
	Fixture.NowSeconds = 2.25;
	Fixture.Job->Tick();
	TestTrue(TEXT("Direct job finishes"), Fixture.Job->IsFinished());
	TestEqual(
		TEXT("Direct job is ready to bind"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind);
	TestEqual(TEXT("Direct job accumulates output"), Fixture.Job->GetProgress().OutputLineCount, 2);
	TestEqual(TEXT("Direct job keeps latest output"), Fixture.Job->GetProgress().LatestOutputLine, FString(TEXT("wasm")));
	TestEqual(TEXT("Direct job records elapsed time"), Fixture.Job->GetProgress().ElapsedSeconds, 1.25);

	FAvidScriptEditorCSharpAsyncBuildResult Result;
	TestTrue(TEXT("Direct result is consumable"), Fixture.Job->ConsumeResult(Result));
	TestTrue(TEXT("Direct result succeeds"), Result.bSucceeded);
	TestFalse(TEXT("Direct result is consumed once"), Fixture.Job->ConsumeResult(Result));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpAsyncBuildJobAutomaticTest,
	"AvidScript.Editor.CSharpAsyncBuildJob.BootstrapThenFinal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpAsyncBuildJobAutomaticTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptCSharpAsyncBuildJobFixture Fixture(true);
	TestTrue(TEXT("Automatic job starts"), Fixture.Job->Start(TEXT("automatic.json")));
	TestEqual(
		TEXT("Automatic job starts bootstrap"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::BootstrapRunning);

	Fixture.Processes[0]->Complete(0, {TEXT("bootstrap")});
	Fixture.Job->Tick();
	TestEqual(
		TEXT("Automatic job exposes binding slice publication"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::PublishingBindingSlice);
	TestEqual(TEXT("Binding slice stage has not launched final process"), Fixture.Processes.Num(), 1);
	Fixture.Job->Tick();
	TestEqual(TEXT("Automatic job creates final process"), Fixture.Processes.Num(), 2);
	TestEqual(
		TEXT("Automatic job advances to final"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning);
	Fixture.Processes[1]->Complete(0, {TEXT("final")});
	Fixture.Job->Tick();
	TestEqual(
		TEXT("Automatic job is ready"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind);
	TestEqual(TEXT("Automatic job aggregates both process outputs"), Fixture.Job->GetProgress().OutputLineCount, 2);
	TestEqual(TEXT("Backend completes two invocations"), Fixture.Backend->CompletedStages.Num(), 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpAsyncBuildJobFinalFailureTest,
	"AvidScript.Editor.CSharpAsyncBuildJob.FinalFailurePreservesCause",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpAsyncBuildJobFinalFailureTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptCSharpAsyncBuildJobFixture Fixture(false);
	Fixture.Backend->bFailFinal = true;
	TestTrue(TEXT("Failing final job starts"), Fixture.Job->Start(TEXT("final-failed.json")));
	Fixture.Processes[0]->Complete(7, {TEXT("final broken")});
	Fixture.Job->Tick();
	TestEqual(
		TEXT("Final failure is terminal"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::Failed);

	FAvidScriptEditorCSharpAsyncBuildResult Result;
	TestTrue(TEXT("Final failure result is consumable"), Fixture.Job->ConsumeResult(Result));
	TestFalse(TEXT("Final failure does not succeed"), Result.bSucceeded);
	TestEqual(TEXT("Final failure category is preserved"), Result.ErrorCategory, FString(TEXT("fixture_final_failed")));
	TestEqual(TEXT("Final failure message is preserved"), Result.ErrorMessage, FString(TEXT("fixture rejected final build")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpAsyncBuildJobFailureTest,
	"AvidScript.Editor.CSharpAsyncBuildJob.BootstrapFailureStops",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpAsyncBuildJobFailureTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptCSharpAsyncBuildJobFixture Fixture(true, true);
	TestTrue(TEXT("Failing job starts"), Fixture.Job->Start(TEXT("failed.json")));
	Fixture.Processes[0]->Complete(2, {TEXT("broken")});
	Fixture.Job->Tick();
	TestEqual(
		TEXT("Bootstrap completion exposes binding slice stage before settlement"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::PublishingBindingSlice);
	Fixture.Job->Tick();
	TestEqual(
		TEXT("Bootstrap failure is terminal"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::Failed);
	TestEqual(TEXT("Bootstrap failure launches no final process"), Fixture.Processes.Num(), 1);

	FAvidScriptEditorCSharpAsyncBuildResult Result;
	TestTrue(TEXT("Failure result is consumable"), Fixture.Job->ConsumeResult(Result));
	TestFalse(TEXT("Failure result does not succeed"), Result.bSucceeded);
	TestEqual(TEXT("Failure category is preserved"), Result.ErrorCategory, FString(TEXT("fixture_bootstrap_failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpAsyncBuildJobBindingSliceCancelTest,
	"AvidScript.Editor.CSharpAsyncBuildJob.CancelDuringBindingSlice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpAsyncBuildJobBindingSliceCancelTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptCSharpAsyncBuildJobFixture Fixture(true);
	TestTrue(TEXT("Binding-slice cancel job starts"), Fixture.Job->Start(TEXT("slice-cancel.json")));
	Fixture.Processes[0]->Complete(0, {TEXT("bootstrap")});
	Fixture.Job->Tick();
	TestEqual(
		TEXT("Job reaches cancellable binding slice stage"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::PublishingBindingSlice);
	Fixture.Job->Cancel();
	TestEqual(
		TEXT("Binding slice cancel is terminal"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::Canceled);
	TestEqual(TEXT("Binding slice cancel launches no final process"), Fixture.Processes.Num(), 1);
	TestTrue(TEXT("Binding slice cancel records request"), Fixture.Job->GetProgress().bCancelRequested);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpAsyncBuildJobCancelTest,
	"AvidScript.Editor.CSharpAsyncBuildJob.Cancel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpAsyncBuildJobCancelTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptCSharpAsyncBuildJobFixture Fixture(false);
	TestTrue(TEXT("Cancelable job starts"), Fixture.Job->Start(TEXT("cancel.json")));
	FFakeAvidScriptCSharpBuildProcess* Process = Fixture.Processes[0];
	Fixture.Job->Cancel();
	TestEqual(TEXT("Cancel reaches active process once"), Process->CancelCount, 1);
	Fixture.Job->Tick();
	TestEqual(
		TEXT("Canceled job is terminal"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::Canceled);
	TestTrue(TEXT("Canceled progress records request"), Fixture.Job->GetProgress().bCancelRequested);

	FAvidScriptEditorCSharpAsyncBuildResult Result;
	TestTrue(TEXT("Canceled result is consumable"), Fixture.Job->ConsumeResult(Result));
	TestEqual(TEXT("Canceled category is stable"), Result.ErrorCategory, FString(TEXT("live_reload_build_canceled")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpAsyncBuildJobLaunchFailureTest,
	"AvidScript.Editor.CSharpAsyncBuildJob.LaunchFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpAsyncBuildJobLaunchFailureTest::RunTest(
	const FString& Parameters)
{
	FAvidScriptCSharpAsyncBuildJobFixture Fixture(false, false, false);
	TestFalse(TEXT("Launch failure rejects start"), Fixture.Job->Start(TEXT("launch.json")));
	TestEqual(
		TEXT("Launch failure is terminal"),
		Fixture.Job->GetProgress().Stage,
		EAvidScriptEditorCSharpAsyncBuildStage::Failed);

	FAvidScriptEditorCSharpAsyncBuildResult Result;
	TestTrue(TEXT("Launch failure result is consumable"), Fixture.Job->ConsumeResult(Result));
	TestEqual(TEXT("Launch failure category is stable"), Result.ErrorCategory, FString(TEXT("live_reload_build_launch_failed")));
	TestTrue(TEXT("Launch failure keeps profile identity"), Result.ProfilePath.EndsWith(TEXT("launch.json")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
