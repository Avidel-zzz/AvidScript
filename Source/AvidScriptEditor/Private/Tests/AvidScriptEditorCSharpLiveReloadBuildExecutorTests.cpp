#if WITH_DEV_AUTOMATION_TESTS

#include "CSharpLiveReload/AvidScriptEditorCSharpLiveReloadBuildExecutor.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Misc/AutomationTest.h"

namespace
{
bool CreateAvidScriptLiveReloadBuildExecutorWorld(UWorld*& OutWorld)
{
	OutWorld = nullptr;
	if (GEngine == nullptr)
	{
		return false;
	}

	OutWorld = UWorld::CreateWorld(EWorldType::Game, false, TEXT("AvidScriptLiveReloadBuildExecutorWorld"));
	if (OutWorld == nullptr)
	{
		return false;
	}

	FWorldContext& WorldContext = GEngine->CreateNewWorldContext(EWorldType::Game);
	WorldContext.SetCurrentWorld(OutWorld);
	return true;
}

void DestroyAvidScriptLiveReloadBuildExecutorWorld(UWorld*& World)
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
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadBuildExecutorShortCircuitTest,
	"AvidScript.Editor.CSharpLiveReload.BuildExecutor.TargetAndProfileShortCircuit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadBuildExecutorShortCircuitTest::RunTest(const FString& Parameters)
{
	int32 LoadCount = 0;
	int32 BuildCount = 0;
	int32 ApplyCount = 0;
	FAvidScriptEditorCSharpLiveReloadBuildExecutor Executor(
		[&LoadCount](const FString&, FAvidScriptEditorCSharpProfileLoadResult& OutProfile)
		{
			++LoadCount;
			OutProfile.ErrorCategory = TEXT("profile_fixture_failed");
			OutProfile.ErrorMessage = TEXT("profile fixture rejected");
			OutProfile.NextAction = TEXT("repair fixture");
			return false;
		},
		[&BuildCount](const FAvidScriptEditorCSharpBuildConfig&, FAvidScriptEditorCSharpBuildResult&)
		{
			++BuildCount;
			return false;
		},
		[&ApplyCount](const FString&, AActor*, FAvidScriptEditorComponentBindingResult&)
		{
			++ApplyCount;
			return false;
		});

	FAvidScriptEditorCSharpLiveReloadBuildResult Result;
	TestFalse(TEXT("Null target is rejected"), Executor.Execute(TEXT("Profile.json"), nullptr, Result));
	TestEqual(
		TEXT("Null target status"),
		Result.Status,
		EAvidScriptEditorCSharpLiveReloadBuildStatus::TargetUnavailable);
	TestEqual(TEXT("Null target calls no dependency"), LoadCount + BuildCount + ApplyCount, 0);

	UWorld* World = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadBuildExecutorWorld(World)))
	{
		return false;
	}
	AActor* TargetActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Target actor spawns"), TargetActor))
	{
		DestroyAvidScriptLiveReloadBuildExecutorWorld(World);
		return false;
	}

	TestFalse(TEXT("Profile failure is returned"), Executor.Execute(TEXT("Profile.json"), TargetActor, Result));
	TestEqual(
		TEXT("Profile failure status"),
		Result.Status,
		EAvidScriptEditorCSharpLiveReloadBuildStatus::ProfileFailed);
	TestEqual(TEXT("Profile stage category"), Result.ErrorCategory, FString(TEXT("live_reload_profile_failed")));
	TestEqual(TEXT("Profile cause category"), Result.CauseErrorCategory, FString(TEXT("profile_fixture_failed")));
	TestEqual(TEXT("Profile loader called once"), LoadCount, 1);
	TestEqual(TEXT("Build is short-circuited"), BuildCount, 0);
	TestEqual(TEXT("Binding is short-circuited"), ApplyCount, 0);
	DestroyAvidScriptLiveReloadBuildExecutorWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadBuildExecutorBuildFailureTest,
	"AvidScript.Editor.CSharpLiveReload.BuildExecutor.BuildFailureShortCircuit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadBuildExecutorBuildFailureTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadBuildExecutorWorld(World)))
	{
		return false;
	}
	AActor* TargetActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Target actor spawns"), TargetActor))
	{
		DestroyAvidScriptLiveReloadBuildExecutorWorld(World);
		return false;
	}

	int32 ApplyCount = 0;
	FAvidScriptEditorCSharpLiveReloadBuildExecutor Executor(
		[](const FString& ProfilePath, FAvidScriptEditorCSharpProfileLoadResult& OutProfile)
		{
			OutProfile.bSucceeded = true;
			OutProfile.NormalizedProfilePath = ProfilePath;
			OutProfile.BuildConfig.ReportPath = TEXT("C:/Reports/live.report.json");
			return true;
		},
		[](const FAvidScriptEditorCSharpBuildConfig&, FAvidScriptEditorCSharpBuildResult& OutBuild)
		{
			OutBuild.ErrorCategory = TEXT("semantic_failed");
			OutBuild.ErrorMessage = TEXT("CS0001 fixture");
			OutBuild.NextAction = TEXT("fix source");
			return false;
		},
		[&ApplyCount](const FString&, AActor*, FAvidScriptEditorComponentBindingResult&)
		{
			++ApplyCount;
			return true;
		});

	FAvidScriptEditorCSharpLiveReloadBuildResult Result;
	TestFalse(TEXT("Build failure is returned"), Executor.Execute(TEXT("Profile.json"), TargetActor, Result));
	TestEqual(TEXT("Build stage category"), Result.ErrorCategory, FString(TEXT("live_reload_build_failed")));
	TestEqual(TEXT("Build cause category"), Result.CauseErrorCategory, FString(TEXT("semantic_failed")));
	TestEqual(TEXT("Binding does not run after build failure"), ApplyCount, 0);
	DestroyAvidScriptLiveReloadBuildExecutorWorld(World);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpLiveReloadBuildExecutorFixedTargetTest,
	"AvidScript.Editor.CSharpLiveReload.BuildExecutor.FixedTargetSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpLiveReloadBuildExecutorFixedTargetTest::RunTest(const FString& Parameters)
{
	UWorld* World = nullptr;
	if (!TestTrue(TEXT("Fixture world creates"), CreateAvidScriptLiveReloadBuildExecutorWorld(World)))
	{
		return false;
	}
	AActor* TargetActor = World->SpawnActor<AActor>();
	AActor* OtherActor = World->SpawnActor<AActor>();
	if (!TestNotNull(TEXT("Target actor spawns"), TargetActor)
		|| !TestNotNull(TEXT("Other actor spawns"), OtherActor))
	{
		DestroyAvidScriptLiveReloadBuildExecutorWorld(World);
		return false;
	}

	const FString ReportPath = TEXT("C:/Reports/live.report.json");
	AActor* AppliedActor = nullptr;
	FAvidScriptEditorCSharpLiveReloadBuildExecutor Executor(
		[ReportPath](const FString& ProfilePath, FAvidScriptEditorCSharpProfileLoadResult& OutProfile)
		{
			OutProfile.bSucceeded = true;
			OutProfile.NormalizedProfilePath = ProfilePath;
			OutProfile.BuildConfig.ReportPath = ReportPath;
			return true;
		},
		[ReportPath](const FAvidScriptEditorCSharpBuildConfig&, FAvidScriptEditorCSharpBuildResult& OutBuild)
		{
			OutBuild.bSucceeded = true;
			OutBuild.ReportPath = ReportPath;
			return true;
		},
		[&AppliedActor, TargetActor, ReportPath](
			const FString& ActualReportPath,
			AActor* ActualActor,
			FAvidScriptEditorComponentBindingResult& OutBinding)
		{
			if (ActualActor != TargetActor || ActualReportPath != ReportPath)
			{
				return false;
			}
			AppliedActor = ActualActor;
			OutBinding.bSucceeded = true;
			return true;
		});

	FAvidScriptEditorCSharpLiveReloadBuildResult Result;
	TestTrue(TEXT("Fixed target pipeline succeeds"), Executor.Execute(TEXT("Profile.json"), TargetActor, Result));
	TestTrue(TEXT("Result succeeds"), Result.bSucceeded);
	TestEqual(
		TEXT("Success status"),
		Result.Status,
		EAvidScriptEditorCSharpLiveReloadBuildStatus::Succeeded);
	TestEqual(TEXT("Explicit target is applied"), AppliedActor, TargetActor);
	TestNotEqual(TEXT("Other actor is not applied"), AppliedActor, OtherActor);
	TestEqual(TEXT("Target path is recorded"), Result.TargetActorPath, TargetActor->GetPathName());
	DestroyAvidScriptLiveReloadBuildExecutorWorld(World);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
