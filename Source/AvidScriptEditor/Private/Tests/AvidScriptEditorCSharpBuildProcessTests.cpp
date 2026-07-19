#if WITH_DEV_AUTOMATION_TESTS

#include "CSharpBuild/AvidScriptEditorCSharpBuildProcess.h"

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

namespace
{
FAvidScriptEditorCSharpBuildInvocation MakeAvidScriptBuildProcessTestInvocation(
	const FString& Command)
{
	FAvidScriptEditorCSharpBuildInvocation Invocation;
	Invocation.ExecutablePath = TEXT("powershell.exe");
	Invocation.Parameters = FString::Printf(
		TEXT("-NoProfile -ExecutionPolicy Bypass -Command \"& { %s }\""),
		*Command);
	Invocation.WorkingDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	return Invocation;
}

bool WaitForAvidScriptBuildProcessTerminal(
	IAvidScriptEditorCSharpBuildProcess& Process,
	FAvidScriptEditorCSharpBuildProcessSnapshot& OutSnapshot,
	const double TimeoutSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	while (FPlatformTime::Seconds() < Deadline)
	{
		Process.Poll(OutSnapshot);
		if (OutSnapshot.State != EAvidScriptEditorCSharpBuildProcessState::Running)
		{
			return true;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	Process.Poll(OutSnapshot);
	return OutSnapshot.State != EAvidScriptEditorCSharpBuildProcessState::Running;
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildProcessSuccessTest,
	"AvidScript.Editor.CSharpBuildProcess.OutputAndSuccess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildProcessSuccessTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpMonitoredBuildProcess Process;
	FString ErrorMessage;
	TestTrue(
		TEXT("Monitored process launches"),
		Process.Launch(
			MakeAvidScriptBuildProcessTestInvocation(
				TEXT("Write-Output 'alpha'; Write-Output 'beta'; exit 0")),
			ErrorMessage));
	TestTrue(TEXT("Monitored process reports running after launch"), Process.IsRunning());

	FAvidScriptEditorCSharpBuildProcessSnapshot Snapshot;
	if (!TestTrue(
		TEXT("Successful process reaches a terminal state"),
		WaitForAvidScriptBuildProcessTerminal(Process, Snapshot, 5.0)))
	{
		Process.Cancel();
		return false;
	}
	TestEqual(
		TEXT("Successful process completes"),
		Snapshot.State,
		EAvidScriptEditorCSharpBuildProcessState::Completed);
	TestEqual(TEXT("Successful process returns zero"), Snapshot.ProcessExitCode, 0);
	TestTrue(TEXT("Successful process captures alpha"), Snapshot.Stdout.Contains(TEXT("alpha")));
	TestTrue(TEXT("Successful process captures beta"), Snapshot.Stdout.Contains(TEXT("beta")));
	TestEqual(TEXT("Latest output is beta"), Snapshot.LatestOutputLine, FString(TEXT("beta")));
	TestFalse(TEXT("Successful process was not canceled"), Snapshot.bCancelRequested);
	TestFalse(TEXT("Successful process no longer runs"), Process.IsRunning());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildProcessFailureTest,
	"AvidScript.Editor.CSharpBuildProcess.NonZeroExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildProcessFailureTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpMonitoredBuildProcess Process;
	FString ErrorMessage;
	TestTrue(
		TEXT("Failing process launches"),
		Process.Launch(
			MakeAvidScriptBuildProcessTestInvocation(TEXT("Write-Output 'failed'; exit 7")),
			ErrorMessage));

	FAvidScriptEditorCSharpBuildProcessSnapshot Snapshot;
	if (!TestTrue(
		TEXT("Failing process reaches a terminal state"),
		WaitForAvidScriptBuildProcessTerminal(Process, Snapshot, 5.0)))
	{
		Process.Cancel();
		return false;
	}
	TestEqual(
		TEXT("Non-zero exit is a completed process"),
		Snapshot.State,
		EAvidScriptEditorCSharpBuildProcessState::Completed);
	TestEqual(TEXT("Non-zero exit code is preserved"), Snapshot.ProcessExitCode, 7);
	TestTrue(TEXT("Failure output is captured"), Snapshot.Stdout.Contains(TEXT("failed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptEditorCSharpBuildProcessCancelTest,
	"AvidScript.Editor.CSharpBuildProcess.CancelKillsTree",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptEditorCSharpBuildProcessCancelTest::RunTest(const FString& Parameters)
{
	FAvidScriptEditorCSharpMonitoredBuildProcess Process;
	FString ErrorMessage;
	TestTrue(
		TEXT("Long process launches"),
		Process.Launch(
			MakeAvidScriptBuildProcessTestInvocation(
				TEXT("Write-Output 'waiting'; Start-Sleep -Seconds 30")),
			ErrorMessage));
	Process.Cancel();

	FAvidScriptEditorCSharpBuildProcessSnapshot Snapshot;
	if (!TestTrue(
		TEXT("Canceled process reaches a terminal state"),
		WaitForAvidScriptBuildProcessTerminal(Process, Snapshot, 5.0)))
	{
		return false;
	}
	TestEqual(
		TEXT("Canceled process reports canceled"),
		Snapshot.State,
		EAvidScriptEditorCSharpBuildProcessState::Canceled);
	TestEqual(TEXT("Canceled process uses cancellation exit code"), Snapshot.ProcessExitCode, -1);
	TestTrue(TEXT("Cancel request is observable"), Snapshot.bCancelRequested);
	TestFalse(TEXT("Canceled process no longer runs"), Process.IsRunning());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
