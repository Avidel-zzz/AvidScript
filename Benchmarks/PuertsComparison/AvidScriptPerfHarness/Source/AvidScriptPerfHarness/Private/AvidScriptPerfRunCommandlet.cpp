#include "AvidScriptPerfRunCommandlet.h"

#include "AvidScriptPerfRunner.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

UAvidScriptPerfRunCommandlet::UAvidScriptPerfRunCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UAvidScriptPerfRunCommandlet::Main(const FString& Params)
{
	(void)Params;
	return RunFromCommandLine(FCommandLine::Get());
}

int32 UAvidScriptPerfRunCommandlet::RunFromCommandLine(const TCHAR* CommandLine)
{
	FString RequestPath;
	FString ResultPath;
	if (!FParse::Value(CommandLine, TEXT("AvidScriptPerfRequest="), RequestPath) ||
		!FParse::Value(CommandLine, TEXT("AvidScriptPerfResult="), ResultPath))
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("ASP53C2001 -AvidScriptPerfRequest and -AvidScriptPerfResult are required"));
		return 2;
	}

	FString Error;
	if (!FAvidScriptPerfRunner::RunWarmBenchmarkFromFiles(
		RequestPath,
		ResultPath,
		Error))
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("ASP53C2002 warm benchmark failed: %s"),
			*Error);
		return 3;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("ASP53C2000 warm benchmark result published: %s"),
		*ResultPath);
	return 0;
}
