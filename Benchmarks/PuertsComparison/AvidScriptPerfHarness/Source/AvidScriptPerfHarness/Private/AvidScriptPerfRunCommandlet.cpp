#include "AvidScriptPerfRunCommandlet.h"

#include "AvidScriptPerfArrayRunner.h"
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
	FString ArrayRequestPath;
	FString ArrayResultPath;
	if (FParse::Value(CommandLine, TEXT("AvidScriptPerfArrayRequest="), ArrayRequestPath))
	{
		if (!FParse::Value(CommandLine, TEXT("AvidScriptPerfArrayResult="), ArrayResultPath))
		{
			UE_LOG(LogTemp, Error, TEXT("ASP57A2001 -AvidScriptPerfArrayResult is required"));
			return 2;
		}
		FString ArrayError;
		if (!FAvidScriptPerfArrayRunner::RunFromFiles(
				ArrayRequestPath,
				ArrayResultPath,
				ArrayError))
		{
			UE_LOG(LogTemp, Error, TEXT("ASP57A2002 array benchmark failed: %s"), *ArrayError);
			return 3;
		}
		UE_LOG(LogTemp, Display, TEXT("ASP57A2000 array benchmark result published: %s"), *ArrayResultPath);
		return 0;
	}

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
