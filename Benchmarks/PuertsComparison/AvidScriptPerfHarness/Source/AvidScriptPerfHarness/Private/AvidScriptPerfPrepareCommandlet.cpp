#include "AvidScriptPerfPrepareCommandlet.h"

#include "AvidScriptEditorCSharpBuildService.h"
#include "AvidScriptEditorCSharpProfileService.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	FString MakeAvidScriptPerfBuildDiagnosticTail(
		const FAvidScriptEditorCSharpBuildResult& BuildResult)
	{
		constexpr int32 MaximumDiagnosticCharacters = 16000;
		FString Details = BuildResult.Stderr;
		if (!BuildResult.Stdout.IsEmpty())
		{
			if (!Details.IsEmpty())
			{
				Details += TEXT("\n");
			}
			Details += BuildResult.Stdout;
		}
		if (Details.Len() > MaximumDiagnosticCharacters)
		{
			Details.RightInline(MaximumDiagnosticCharacters);
		}
		return Details;
	}
}

UAvidScriptPerfPrepareCommandlet::UAvidScriptPerfPrepareCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UAvidScriptPerfPrepareCommandlet::Main(const FString& Params)
{
	FString ProfilePath;
	FParse::Value(*Params, TEXT("Profile="), ProfilePath);
	if (ProfilePath.IsEmpty())
	{
		const TSharedPtr<IPlugin> HarnessPlugin =
			IPluginManager::Get().FindPlugin(TEXT("AvidScriptPerfHarness"));
		if (!HarnessPlugin.IsValid())
		{
			UE_LOG(
				LogTemp,
				Error,
				TEXT("ASP53P1001 AvidScriptPerfHarness plugin is not mounted"));
			return 2;
		}
		ProfilePath = FPaths::Combine(
			HarnessPlugin->GetContentDir(),
			TEXT("CSharp/AvidScriptPerfWorkload.csharp-profile.json"));
	}
	else if (FPaths::IsRelative(ProfilePath))
	{
		ProfilePath = FPaths::ConvertRelativePathToFull(
			FPaths::ProjectDir(),
			ProfilePath);
	}

	FAvidScriptEditorCSharpProfileLoadResult ProfileResult;
	if (!FAvidScriptEditorCSharpProfileService::LoadProfile(
		ProfilePath,
		ProfileResult))
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("ASP53P1002 C# benchmark profile load failed | category=%s | message=%s"),
			*ProfileResult.ErrorCategory,
			*ProfileResult.ErrorMessage);
		return 3;
	}

	FAvidScriptEditorCSharpBuildResult BuildResult;
	if (!FAvidScriptEditorCSharpBuildService::BuildProfile(
		FAvidScriptEditorCSharpProfileService::MakeBuildRequest(ProfileResult),
		BuildResult))
	{
		const FString DiagnosticTail =
			MakeAvidScriptPerfBuildDiagnosticTail(BuildResult);
		UE_LOG(
			LogTemp,
			Error,
			TEXT("ASP53P1003 C# benchmark profile build failed | category=%s | message=%s | diagnostics=%s"),
			*BuildResult.ErrorCategory,
			*BuildResult.ErrorMessage,
			DiagnosticTail.IsEmpty() ? TEXT("<none>") : *DiagnosticTail);
		return 4;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("ASP53P1000 C# benchmark artifact ready | manifest=%s | report=%s"),
		*BuildResult.ManifestPath,
		*BuildResult.ReportPath);
	return 0;
}
