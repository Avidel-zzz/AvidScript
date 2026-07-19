#if WITH_DEV_AUTOMATION_TESTS

#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadService.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
class FFakeAvidScriptLiveReloadWatchHost final
	: public IAvidScriptEditorCSharpLiveReloadWatchHost
{
public:
	virtual bool Start(
		const FString& WorkspaceRoot,
		FOnChangeBatch InOnChangeBatch,
		FString& OutErrorCategory,
		FString& OutErrorMessage) override
	{
		++StartCount;
		WatchedRoot = WorkspaceRoot;
		if (!bStartSucceeds)
		{
			OutErrorCategory = TEXT("watch_fixture_failed");
			OutErrorMessage = TEXT("watch fixture rejected");
			return false;
		}
		Callback = MoveTemp(InOnChangeBatch);
		bWatching = true;
		return true;
	}

	virtual void Stop() override
	{
		if (bWatching)
		{
			++StopCount;
		}
		bWatching = false;
		Callback = FOnChangeBatch();
	}

	virtual bool IsWatching() const override
	{
		return bWatching;
	}

	void Emit(TArray<FString> FilePaths, const bool bRescanRequired = false)
	{
		if (!bWatching || !Callback)
		{
			return;
		}
		FAvidScriptEditorCSharpLiveReloadChangeBatch Batch;
		Batch.FilePaths = MoveTemp(FilePaths);
		Batch.bRescanRequired = bRescanRequired;
		Callback(MoveTemp(Batch));
	}

	bool bStartSucceeds = true;
	bool bWatching = false;
	int32 StartCount = 0;
	int32 StopCount = 0;
	FString WatchedRoot;
	FOnChangeBatch Callback;
};

struct FFakeAvidScriptCSharpAsyncJobState
{
	void CompleteSuccess(const FString& ReportPath = TEXT("fixture.report.json"))
	{
		bFinished = true;
		Progress.Stage = EAvidScriptEditorCSharpAsyncBuildStage::ReadyToBind;
		Result.bSucceeded = true;
		Result.ProfilePath = StartedProfilePath;
		Result.ProfileResult.bSucceeded = true;
		Result.BuildResult.bSucceeded = true;
		Result.BuildResult.ReportPath = ReportPath;
	}

	void CompleteFailure(const FString& CauseCategory)
	{
		bFinished = true;
		Progress.Stage = EAvidScriptEditorCSharpAsyncBuildStage::Failed;
		Result.bSucceeded = false;
		Result.ProfilePath = StartedProfilePath;
		Result.ErrorCategory = CauseCategory;
		Result.ErrorMessage = TEXT("fixture asynchronous build failed");
		Result.NextAction = TEXT("fix the fixture input");
		Result.ProfileResult.bSucceeded = true;
		Result.BuildResult.ErrorCategory = CauseCategory;
		Result.BuildResult.ErrorMessage = Result.ErrorMessage;
	}

	void CompleteCanceled()
	{
		bFinished = true;
		Progress.Stage = EAvidScriptEditorCSharpAsyncBuildStage::Canceled;
		Progress.bCancelRequested = true;
		Result.bSucceeded = false;
		Result.ProfilePath = StartedProfilePath;
		Result.ErrorCategory = TEXT("live_reload_build_canceled");
		Result.ErrorMessage = TEXT("fixture asynchronous build canceled");
	}

	bool bStartSucceeds = true;
	bool bFinished = false;
	bool bConsumed = false;
	int32 StartCount = 0;
	int32 TickCount = 0;
	int32 CancelCount = 0;
	FString StartedProfilePath;
	FAvidScriptEditorCSharpAsyncBuildProgress Progress;
	FAvidScriptEditorCSharpAsyncBuildResult Result;
};

class FFakeAvidScriptCSharpAsyncBuildJob final
	: public IAvidScriptEditorCSharpAsyncBuildJob
{
public:
	explicit FFakeAvidScriptCSharpAsyncBuildJob(
		TSharedRef<FFakeAvidScriptCSharpAsyncJobState> InState)
		: State(MoveTemp(InState))
	{
	}

	virtual bool Start(const FString& ProfilePath) override
	{
		++State->StartCount;
		State->StartedProfilePath = ProfilePath;
		State->Progress.Stage =
			EAvidScriptEditorCSharpAsyncBuildStage::FinalRunning;
		if (!State->bStartSucceeds)
		{
			State->CompleteFailure(TEXT("fixture_launch_failed"));
			return false;
		}
		return true;
	}

	virtual void Tick() override
	{
		++State->TickCount;
	}

	virtual void Cancel() override
	{
		++State->CancelCount;
		if (!State->bFinished)
		{
			State->CompleteCanceled();
		}
	}

	virtual bool IsFinished() const override
	{
		return State->bFinished;
	}

	virtual const FAvidScriptEditorCSharpAsyncBuildProgress&
		GetProgress() const override
	{
		return State->Progress;
	}

	virtual bool ConsumeResult(
		FAvidScriptEditorCSharpAsyncBuildResult& OutResult) override
	{
		if (!State->bFinished || State->bConsumed)
		{
			return false;
		}
		State->bConsumed = true;
		OutResult = State->Result;
		return true;
	}

private:
	TSharedRef<FFakeAvidScriptCSharpAsyncJobState> State;
};

class FFakeAvidScriptCSharpAsyncJobQueue
{
public:
	TSharedRef<FFakeAvidScriptCSharpAsyncJobState> PlanJob()
	{
		const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> State =
			MakeShared<FFakeAvidScriptCSharpAsyncJobState>();
		PlannedStates.Add(State);
		return State;
	}

	TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob> Create()
	{
		++CreateCount;
		if (!PlannedStates.IsValidIndex(NextStateIndex))
		{
			PlanJob();
		}
		return TUniquePtr<IAvidScriptEditorCSharpAsyncBuildJob>(
			new FFakeAvidScriptCSharpAsyncBuildJob(
				PlannedStates[NextStateIndex++]));
	}

	TArray<TSharedRef<FFakeAvidScriptCSharpAsyncJobState>> PlannedStates;
	int32 NextStateIndex = 0;
	int32 CreateCount = 0;
};

FString MakeAvidScriptLiveReloadServiceTestRoot(const FString& CaseName)
{
	FString Root = FPaths::ConvertRelativePathToFull(FPaths::Combine(
		FPaths::ProjectSavedDir(),
		TEXT("AvidScript"),
		TEXT("Tests"),
		TEXT("P45"),
		TEXT("LiveReloadService"),
		CaseName));
	FPaths::NormalizeFilename(Root);
	IFileManager::Get().MakeDirectory(*Root, true);
	return Root;
}

bool CreateAvidScriptLiveReloadServiceWorld(
	UWorld*& OutWorld,
	AActor*& OutActor)
{
	OutWorld = nullptr;
	OutActor = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(
		EWorldType::Game,
		false,
		TEXT("AvidScriptLiveReloadServiceWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext =
		GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	OutActor = OutWorld->SpawnActor<AActor>();
	return OutActor != nullptr;
}

void DestroyAvidScriptLiveReloadServiceWorld(UWorld*& World)
{
	if (World == nullptr)
	{
		return;
	}
	if (GEngine != nullptr)
	{
		GEngine->DestroyWorldContext(World);
	}
	World->DestroyWorld(false);
	World = nullptr;
}

FAvidScriptEditorCSharpLiveReloadServiceConfig
MakeAvidScriptLiveReloadServiceConfig(const FString& CaseName)
{
	FAvidScriptEditorCSharpLiveReloadServiceConfig Config;
	Config.WorkspaceRoot =
		MakeAvidScriptLiveReloadServiceTestRoot(CaseName);
	Config.ProfilePath = FPaths::Combine(
		Config.WorkspaceRoot,
		TEXT("default.csharp-profile.json"));
	Config.DebounceSeconds = 0.35;
	return Config;
}

bool ApplyAvidScriptLiveReloadServiceFixtureReport(
	const FString& ReportPath,
	AActor* TargetActor,
	AActor*& OutAppliedTarget,
	int32& OutApplyCount,
	FAvidScriptEditorComponentBindingResult& OutResult)
{
	++OutApplyCount;
	OutAppliedTarget = TargetActor;
	OutResult.bSucceeded = !ReportPath.IsEmpty() && IsValid(TargetActor);
	return OutResult.bSucceeded;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceNonBlockingTest,
	"AvidScript.Editor.CSharpLiveReload.Service.NonBlockingDebounceLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceNonBlockingTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(
		TEXT("Fixture world creates"),
		CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 1.0;
	int32 ApplyCount = 0;
	AActor* AppliedTarget = nullptr;
	FFakeAvidScriptCSharpAsyncJobQueue Jobs;
	const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> Job = Jobs.PlanJob();
	FFakeAvidScriptLiveReloadWatchHost* FakeHost =
		new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&Jobs]() { return Jobs.Create(); },
		[&AppliedTarget, &ApplyCount](
			const FString& ReportPath,
			AActor* ActualTarget,
			FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return ApplyAvidScriptLiveReloadServiceFixtureReport(
				ReportPath,
				ActualTarget,
				AppliedTarget,
				ApplyCount,
				OutResult);
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config =
		MakeAvidScriptLiveReloadServiceConfig(TEXT("NonBlocking"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Gameplay.cs"))});
	Service.Tick();
	Now = 1.35;
	Service.Tick();
	TestEqual(TEXT("Deadline creates one job"), Jobs.CreateCount, 1);
	TestEqual(TEXT("Deadline starts one job"), Job->StartCount, 1);
	TestEqual(TEXT("Deadline does not bind inline"), ApplyCount, 0);
	TestEqual(
		TEXT("Service reports building"),
		Service.GetLastResult().Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::Building);

	Job->CompleteSuccess();
	Service.Tick();
	TestEqual(TEXT("Completion binds once"), ApplyCount, 1);
	TestEqual(TEXT("Completion uses fixed Actor"), AppliedTarget, Target);
	TestEqual(
		TEXT("Completion reports success"),
		Service.GetLastResult().Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildSucceeded);
	Service.Stop();
	TestEqual(TEXT("Watch stops once"), FakeHost->StopCount, 1);
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceTrailingTest,
	"AvidScript.Editor.CSharpLiveReload.Service.BuildTimeChangeTrails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceTrailingTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 2.0;
	int32 ApplyCount = 0;
	AActor* AppliedTarget = nullptr;
	FFakeAvidScriptCSharpAsyncJobQueue Jobs;
	const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> FirstJob = Jobs.PlanJob();
	const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> SecondJob = Jobs.PlanJob();
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&Jobs]() { return Jobs.Create(); },
		[&AppliedTarget, &ApplyCount](const FString& ReportPath, AActor* ActualTarget, FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return ApplyAvidScriptLiveReloadServiceFixtureReport(ReportPath, ActualTarget, AppliedTarget, ApplyCount, OutResult);
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config = MakeAvidScriptLiveReloadServiceConfig(TEXT("Trailing"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("First.cs"))});
	Service.Tick();
	Now = 2.35;
	Service.Tick();
	TestEqual(TEXT("First job starts"), Jobs.CreateCount, 1);
	Now = 2.4;
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Trailing.cs"))});
	Service.Tick();
	TestEqual(TEXT("Build-time batch coalesces"), Service.GetStats().CoalescedChangeBatchCount, 1);
	FirstJob->CompleteSuccess(TEXT("first.report.json"));
	Service.Tick();
	TestEqual(TEXT("First completion binds"), ApplyCount, 1);
	Now = 2.749;
	Service.Tick();
	TestEqual(TEXT("Trailing deadline suppresses early start"), Jobs.CreateCount, 1);
	Now = 2.75;
	Service.Tick();
	TestEqual(TEXT("Exactly one trailing job starts"), Jobs.CreateCount, 2);
	SecondJob->CompleteSuccess(TEXT("second.report.json"));
	Service.Tick();
	TestEqual(TEXT("Trailing completion binds once"), ApplyCount, 2);
	Service.Stop();
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceFailureTest,
	"AvidScript.Editor.CSharpLiveReload.Service.BuildFailureWaitsForChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceFailureTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 3.0;
	int32 ApplyCount = 0;
	AActor* AppliedTarget = nullptr;
	FFakeAvidScriptCSharpAsyncJobQueue Jobs;
	const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> Job = Jobs.PlanJob();
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&Jobs]() { return Jobs.Create(); },
		[&AppliedTarget, &ApplyCount](const FString& ReportPath, AActor* ActualTarget, FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return ApplyAvidScriptLiveReloadServiceFixtureReport(ReportPath, ActualTarget, AppliedTarget, ApplyCount, OutResult);
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config = MakeAvidScriptLiveReloadServiceConfig(TEXT("Failure"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Broken.cs"))});
	Service.Tick();
	Now = 3.35;
	Service.Tick();
	Job->CompleteFailure(TEXT("semantic_failed"));
	Service.Tick();
	TestTrue(TEXT("Service remains watching"), Service.IsRunning());
	TestEqual(TEXT("Failure status"), Service.GetLastResult().Status, EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed);
	TestEqual(TEXT("Failure cause is preserved"), Service.GetLastResult().CauseErrorCategory, FString(TEXT("semantic_failed")));
	TestEqual(TEXT("Build failure never binds"), ApplyCount, 0);
	Now = 10.0;
	Service.Tick();
	TestEqual(TEXT("Unchanged failure does not retry"), Jobs.CreateCount, 1);
	Service.Stop();
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceTargetTest,
	"AvidScript.Editor.CSharpLiveReload.Service.DestroyedTargetStops",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceTargetTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 4.0;
	int32 ApplyCount = 0;
	AActor* AppliedTarget = nullptr;
	FFakeAvidScriptCSharpAsyncJobQueue Jobs;
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&Jobs]() { return Jobs.Create(); },
		[&AppliedTarget, &ApplyCount](const FString& ReportPath, AActor* ActualTarget, FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return ApplyAvidScriptLiveReloadServiceFixtureReport(ReportPath, ActualTarget, AppliedTarget, ApplyCount, OutResult);
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config = MakeAvidScriptLiveReloadServiceConfig(TEXT("DestroyedTarget"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	TestTrue(TEXT("Target actor destroys"), World->DestroyActor(Target));
	Target = nullptr;
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("AfterDestroy.cs"))});
	Service.Tick();
	TestFalse(TEXT("Service stops for destroyed target"), Service.IsRunning());
	TestEqual(TEXT("Destroyed target creates no job"), Jobs.CreateCount, 0);
	TestEqual(TEXT("Destroyed target binds nothing"), ApplyCount, 0);
	TestEqual(TEXT("Destroyed target category"), Service.GetLastResult().ErrorCategory, FString(TEXT("live_reload_target_unavailable")));
	TestEqual(TEXT("Destroyed target unregisters watcher"), FakeHost->StopCount, 1);
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceStopTest,
	"AvidScript.Editor.CSharpLiveReload.Service.StopCancelsAndRejectsCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceStopTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 5.0;
	int32 ApplyCount = 0;
	AActor* AppliedTarget = nullptr;
	FFakeAvidScriptCSharpAsyncJobQueue Jobs;
	const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> Job = Jobs.PlanJob();
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&Jobs]() { return Jobs.Create(); },
		[&AppliedTarget, &ApplyCount](const FString& ReportPath, AActor* ActualTarget, FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return ApplyAvidScriptLiveReloadServiceFixtureReport(ReportPath, ActualTarget, AppliedTarget, ApplyCount, OutResult);
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config = MakeAvidScriptLiveReloadServiceConfig(TEXT("Stop"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Stop.cs"))});
	Service.Tick();
	Now = 5.35;
	Service.Tick();
	Service.Stop();
	TestEqual(TEXT("Stop cancels active job once"), Job->CancelCount, 1);
	Job->CompleteSuccess();
	TestFalse(TEXT("Stopped service rejects old completion"), Service.Tick());
	TestEqual(TEXT("Stopped completion never binds"), ApplyCount, 0);
	TestEqual(TEXT("Stopped status is preserved"), Service.GetLastResult().Status, EAvidScriptEditorCSharpLiveReloadServiceStatus::Stopped);
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceRenamedTargetTest,
	"AvidScript.Editor.CSharpLiveReload.Service.RenamedTargetStopsWithoutBinding",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceRenamedTargetTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 5.5;
	int32 ApplyCount = 0;
	AActor* AppliedTarget = nullptr;
	FFakeAvidScriptCSharpAsyncJobQueue Jobs;
	const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> Job = Jobs.PlanJob();
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&Jobs]() { return Jobs.Create(); },
		[&AppliedTarget, &ApplyCount](const FString& ReportPath, AActor* ActualTarget, FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return ApplyAvidScriptLiveReloadServiceFixtureReport(ReportPath, ActualTarget, AppliedTarget, ApplyCount, OutResult);
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config = MakeAvidScriptLiveReloadServiceConfig(TEXT("RenamedTarget"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Rename.cs"))});
	Service.Tick();
	Now = 5.85;
	Service.Tick();
	TestTrue(TEXT("Fixed target renames during build"), Target->Rename(TEXT("AvidScriptRenamedLiveReloadTarget"), nullptr, REN_DontCreateRedirectors));
	Job->CompleteSuccess();
	TestFalse(TEXT("Renamed target stops service on completion"), Service.Tick());
	TestEqual(TEXT("Renamed target never binds"), ApplyCount, 0);
	TestFalse(TEXT("Renamed target does not leave coordinator building"), Service.IsRunning());
	TestEqual(TEXT("Renamed target status"), Service.GetLastResult().Status, EAvidScriptEditorCSharpLiveReloadServiceStatus::TargetUnavailable);
	TestEqual(TEXT("Renamed target cause"), Service.GetLastResult().CauseErrorCategory, FString(TEXT("actor_identity_changed_during_build")));
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceDestroyDuringBuildTest,
	"AvidScript.Editor.CSharpLiveReload.Service.DestroyedDuringBuildCancels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceDestroyDuringBuildTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 6.0;
	int32 ApplyCount = 0;
	AActor* AppliedTarget = nullptr;
	FFakeAvidScriptCSharpAsyncJobQueue Jobs;
	const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> Job = Jobs.PlanJob();
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&Jobs]() { return Jobs.Create(); },
		[&AppliedTarget, &ApplyCount](const FString& ReportPath, AActor* ActualTarget, FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return ApplyAvidScriptLiveReloadServiceFixtureReport(ReportPath, ActualTarget, AppliedTarget, ApplyCount, OutResult);
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config = MakeAvidScriptLiveReloadServiceConfig(TEXT("DestroyedDuringBuild"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Destroy.cs"))});
	Service.Tick();
	Now = 6.35;
	Service.Tick();
	TestTrue(TEXT("Target destroys while job is active"), World->DestroyActor(Target));
	Target = nullptr;
	TestFalse(TEXT("Destroyed target stops service"), Service.Tick());
	TestEqual(TEXT("Destroyed target cancels job once"), Job->CancelCount, 1);
	TestEqual(TEXT("Destroyed target never binds"), ApplyCount, 0);
	TestEqual(TEXT("Destroyed during build cause"), Service.GetLastResult().CauseErrorCategory, FString(TEXT("actor_destroyed_during_build")));
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceBindingFailureTest,
	"AvidScript.Editor.CSharpLiveReload.Service.BindingFailureWaitsForChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceBindingFailureTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 7.0;
	int32 ApplyCount = 0;
	FFakeAvidScriptCSharpAsyncJobQueue Jobs;
	const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> Job = Jobs.PlanJob();
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&Jobs]() { return Jobs.Create(); },
		[&ApplyCount](const FString&, AActor*, FAvidScriptEditorComponentBindingResult& OutResult)
		{
			++ApplyCount;
			OutResult.ErrorCategory = TEXT("fixture_binding_failed");
			OutResult.ErrorMessage = TEXT("fixture rejected binding");
			return false;
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config = MakeAvidScriptLiveReloadServiceConfig(TEXT("BindingFailure"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Binding.cs"))});
	Service.Tick();
	Now = 7.35;
	Service.Tick();
	Job->CompleteSuccess();
	Service.Tick();
	TestEqual(TEXT("Binding attempts once"), ApplyCount, 1);
	TestTrue(TEXT("Binding failure keeps service running"), Service.IsRunning());
	TestEqual(TEXT("Binding failure service status"), Service.GetLastResult().Status, EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed);
	TestEqual(TEXT("Binding failure build status"), Service.GetLastResult().BuildResult.Status, EAvidScriptEditorCSharpLiveReloadBuildStatus::BindingFailed);
	TestEqual(TEXT("Binding failure cause"), Service.GetLastResult().CauseErrorCategory, FString(TEXT("fixture_binding_failed")));
	Now = 20.0;
	Service.Tick();
	TestEqual(TEXT("Binding failure does not retry unchanged input"), Jobs.CreateCount, 1);
	Service.Stop();
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceCanceledJobTest,
	"AvidScript.Editor.CSharpLiveReload.Service.CanceledJobDoesNotBind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceCanceledJobTest::RunTest(
	const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 8.0;
	int32 ApplyCount = 0;
	AActor* AppliedTarget = nullptr;
	FFakeAvidScriptCSharpAsyncJobQueue Jobs;
	const TSharedRef<FFakeAvidScriptCSharpAsyncJobState> Job = Jobs.PlanJob();
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&Jobs]() { return Jobs.Create(); },
		[&AppliedTarget, &ApplyCount](const FString& ReportPath, AActor* ActualTarget, FAvidScriptEditorComponentBindingResult& OutResult)
		{
			return ApplyAvidScriptLiveReloadServiceFixtureReport(ReportPath, ActualTarget, AppliedTarget, ApplyCount, OutResult);
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config = MakeAvidScriptLiveReloadServiceConfig(TEXT("Canceled"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Canceled.cs"))});
	Service.Tick();
	Now = 8.35;
	Service.Tick();
	Job->CompleteSuccess(TEXT("must-not-bind.report.json"));
	Job->Progress.Stage = EAvidScriptEditorCSharpAsyncBuildStage::Canceled;
	Job->Progress.bCancelRequested = true;
	Job->Result.ErrorCategory = TEXT("live_reload_build_canceled");
	Job->Result.ErrorMessage = TEXT("fixture canceled with stale success payload");
	Service.Tick();
	TestEqual(TEXT("Canceled job does not bind"), ApplyCount, 0);
	TestEqual(TEXT("Canceled job status"), Service.GetLastResult().Status, EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildCanceled);
	TestTrue(TEXT("Canceled job keeps watcher running"), Service.IsRunning());
	Service.Stop();
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
