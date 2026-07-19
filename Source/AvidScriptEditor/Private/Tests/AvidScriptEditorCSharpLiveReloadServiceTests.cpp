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
class FFakeAvidScriptLiveReloadWatchHost final : public IAvidScriptEditorCSharpLiveReloadWatchHost
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

bool CreateAvidScriptLiveReloadServiceWorld(UWorld*& OutWorld, AActor*& OutActor)
{
	OutWorld = nullptr;
	OutActor = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}
	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptLiveReloadServiceWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}
	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
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

FAvidScriptEditorCSharpLiveReloadServiceConfig MakeAvidScriptLiveReloadServiceConfig(
	const FString& CaseName)
{
	FAvidScriptEditorCSharpLiveReloadServiceConfig Config;
	Config.WorkspaceRoot = MakeAvidScriptLiveReloadServiceTestRoot(CaseName);
	Config.ProfilePath = FPaths::Combine(Config.WorkspaceRoot, TEXT("default.csharp-profile.json"));
	Config.DebounceSeconds = 0.35;
	return Config;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceDebounceTest,
	"AvidScript.Editor.CSharpLiveReload.Service.WatchDebounceLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceDebounceTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 1.0;
	int32 BuildCount = 0;
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&BuildCount, Target](
			const FString&,
			AActor* ActualTarget,
			FAvidScriptEditorCSharpLiveReloadBuildResult& OutBuild)
		{
			++BuildCount;
			OutBuild.bSucceeded = ActualTarget == Target;
			OutBuild.Status = EAvidScriptEditorCSharpLiveReloadBuildStatus::Succeeded;
			return OutBuild.bSucceeded;
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config =
		MakeAvidScriptLiveReloadServiceConfig(TEXT("Debounce"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	TestEqual(TEXT("Watch registers once"), FakeHost->StartCount, 1);
	TestEqual(TEXT("Watch root is normalized workspace"), FakeHost->WatchedRoot, StartResult.WorkspaceRoot);

	FakeHost->Emit({
		FPaths::Combine(Config.WorkspaceRoot, TEXT("GameplayScript.cs")),
		FPaths::Combine(Config.WorkspaceRoot, TEXT("notes.txt"))});
	TestEqual(TEXT("Callback does not build inline"), BuildCount, 0);
	Service.Tick();
	Now = 1.349;
	Service.Tick();
	TestEqual(TEXT("Debounce suppresses early build"), BuildCount, 0);
	Now = 1.35;
	Service.Tick();
	TestEqual(TEXT("Build runs once at deadline"), BuildCount, 1);
	TestEqual(
		TEXT("Service success status"),
		Service.GetLastResult().Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildSucceeded);

	Service.Stop();
	TestFalse(TEXT("Service stops"), Service.IsRunning());
	TestEqual(TEXT("Watch unregisters once"), FakeHost->StopCount, 1);
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceTrailingTest,
	"AvidScript.Editor.CSharpLiveReload.Service.BuildTimeChangeTrails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceTrailingTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 2.0;
	int32 BuildCount = 0;
	FAvidScriptEditorCSharpLiveReloadService* ServicePtr = nullptr;
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&BuildCount, &Now, FakeHost, &ServicePtr](
			const FString&,
			AActor*,
			FAvidScriptEditorCSharpLiveReloadBuildResult& OutBuild)
		{
			++BuildCount;
			if (BuildCount == 1)
			{
				Now = 2.4;
				FakeHost->Emit({FPaths::Combine(
					ServicePtr->GetLastResult().WorkspaceRoot,
					TEXT("Trailing.cs"))});
			}
			OutBuild.bSucceeded = true;
			OutBuild.Status = EAvidScriptEditorCSharpLiveReloadBuildStatus::Succeeded;
			return true;
		},
		[&Now]() { return Now; });
	ServicePtr = &Service;

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config =
		MakeAvidScriptLiveReloadServiceConfig(TEXT("Trailing"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("First.cs"))});
	Service.Tick();
	Now = 2.35;
	Service.Tick();
	TestEqual(TEXT("First build completes"), BuildCount, 1);
	TestEqual(TEXT("Build-time batch is coalesced"), Service.GetStats().CoalescedChangeBatchCount, 1);
	Now = 2.749;
	Service.Tick();
	TestEqual(TEXT("Trailing debounce suppresses early build"), BuildCount, 1);
	Now = 2.75;
	Service.Tick();
	TestEqual(TEXT("Exactly one trailing build runs"), BuildCount, 2);
	Service.Stop();
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceFailureTest,
	"AvidScript.Editor.CSharpLiveReload.Service.BuildFailureWaitsForChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceFailureTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 3.0;
	int32 BuildCount = 0;
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&BuildCount](const FString&, AActor*, FAvidScriptEditorCSharpLiveReloadBuildResult& OutBuild)
		{
			++BuildCount;
			OutBuild.ErrorCategory = TEXT("live_reload_build_failed");
			OutBuild.CauseErrorCategory = TEXT("semantic_failed");
			OutBuild.ErrorMessage = TEXT("fixture build failed");
			return false;
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config =
		MakeAvidScriptLiveReloadServiceConfig(TEXT("Failure"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Broken.cs"))});
	Service.Tick();
	Now = 3.35;
	Service.Tick();
	TestEqual(TEXT("Failed build ran once"), BuildCount, 1);
	TestTrue(TEXT("Service remains watching after failure"), Service.IsRunning());
	TestEqual(
		TEXT("Failure status"),
		Service.GetLastResult().Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::BuildFailed);
	TestEqual(TEXT("Failure cause is preserved"), Service.GetLastResult().CauseErrorCategory, FString(TEXT("semantic_failed")));
	Now = 10.0;
	Service.Tick();
	TestEqual(TEXT("Failure does not retry unchanged input"), BuildCount, 1);
	Service.Stop();
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceTargetTest,
	"AvidScript.Editor.CSharpLiveReload.Service.DestroyedTargetStops",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceTargetTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 4.0;
	int32 BuildCount = 0;
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&BuildCount](const FString&, AActor*, FAvidScriptEditorCSharpLiveReloadBuildResult&)
		{
			++BuildCount;
			return true;
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config =
		MakeAvidScriptLiveReloadServiceConfig(TEXT("DestroyedTarget"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	TestTrue(TEXT("Target actor destroys"), World->DestroyActor(Target));
	Target = nullptr;
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("AfterDestroy.cs"))});
	Service.Tick();
	TestFalse(TEXT("Service stops for destroyed target"), Service.IsRunning());
	TestEqual(TEXT("Destroyed target runs no build"), BuildCount, 0);
	TestEqual(
		TEXT("Destroyed target category"),
		Service.GetLastResult().ErrorCategory,
		FString(TEXT("live_reload_target_unavailable")));
	TestEqual(TEXT("Destroyed target unregisters watcher"), FakeHost->StopCount, 1);
	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceStopDuringBuildTest,
	"AvidScript.Editor.CSharpLiveReload.Service.StopDuringBuildRejectsCompletion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceStopDuringBuildTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 5.0;
	int32 BuildCount = 0;
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService* ServicePtr = nullptr;
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&BuildCount, &ServicePtr](
			const FString&,
			AActor*,
			FAvidScriptEditorCSharpLiveReloadBuildResult& OutBuild)
		{
			++BuildCount;
			OutBuild.bSucceeded = true;
			ServicePtr->Stop();
			return true;
		},
		[&Now]() { return Now; });
	ServicePtr = &Service;

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config =
		MakeAvidScriptLiveReloadServiceConfig(TEXT("StopDuringBuild"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Stop.cs"))});
	Service.Tick();
	Now = 5.35;
	TestFalse(TEXT("Stopped generation rejects build completion"), Service.Tick());
	TestEqual(TEXT("Build ran once"), BuildCount, 1);
	TestFalse(TEXT("Service remains stopped"), Service.IsRunning());
	TestFalse(TEXT("Stopped result is not overwritten as running"), Service.GetLastResult().bRunning);
	TestEqual(
		TEXT("Stopped status is preserved"),
		Service.GetLastResult().Status,
		EAvidScriptEditorCSharpLiveReloadServiceStatus::Stopped);

	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadServiceDestroyDuringBuildTest,
	"AvidScript.Editor.CSharpLiveReload.Service.DestroyedDuringBuildStops",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadServiceDestroyDuringBuildTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	AActor* Target = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadServiceWorld(World, Target)))
	{
		return false;
	}

	double Now = 6.0;
	int32 BuildCount = 0;
	FFakeAvidScriptLiveReloadWatchHost* FakeHost = new FFakeAvidScriptLiveReloadWatchHost();
	FAvidScriptEditorCSharpLiveReloadService Service(
		TUniquePtr<IAvidScriptEditorCSharpLiveReloadWatchHost>(FakeHost),
		[&BuildCount, &World, &Target](
			const FString&,
			AActor*,
			FAvidScriptEditorCSharpLiveReloadBuildResult& OutBuild)
		{
			++BuildCount;
			OutBuild.bSucceeded = true;
			World->DestroyActor(Target);
			Target = nullptr;
			return true;
		},
		[&Now]() { return Now; });

	const FAvidScriptEditorCSharpLiveReloadServiceConfig Config =
		MakeAvidScriptLiveReloadServiceConfig(TEXT("DestroyedDuringBuild"));
	FAvidScriptEditorCSharpLiveReloadServiceResult StartResult;
	TestTrue(TEXT("Service starts"), Service.Start(Config, Target, StartResult));
	FakeHost->Emit({FPaths::Combine(Config.WorkspaceRoot, TEXT("Destroy.cs"))});
	Service.Tick();
	Now = 6.35;
	TestFalse(TEXT("Destroy during build stops ticker"), Service.Tick());
	TestEqual(TEXT("Build ran once"), BuildCount, 1);
	TestFalse(TEXT("Service stops for destroyed target"), Service.IsRunning());
	TestEqual(
		TEXT("Destroyed during build category"),
		Service.GetLastResult().CauseErrorCategory,
		FString(TEXT("actor_destroyed_during_build")));
	TestEqual(TEXT("Destroyed target unregisters watcher"), FakeHost->StopCount, 1);

	DestroyAvidScriptLiveReloadServiceWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
