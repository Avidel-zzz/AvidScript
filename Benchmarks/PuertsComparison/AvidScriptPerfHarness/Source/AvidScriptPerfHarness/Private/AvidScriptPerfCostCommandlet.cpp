#include "AvidScriptPerfCostCommandlet.h"

#include "AvidScriptPerfCostRunner.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

UAvidScriptPerfCostCommandlet::UAvidScriptPerfCostCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UAvidScriptPerfCostCommandlet::Main(const FString& Params)
{
	(void)Params;
	return RunFromCommandLine(FCommandLine::Get());
}

int32 UAvidScriptPerfCostCommandlet::RunFromCommandLine(
	const TCHAR* CommandLine)
{
	FString RequestPath;
	FString ResultPath;
	if (!FParse::Value(
			CommandLine,
			TEXT("AvidScriptPerfCostRequest="),
			RequestPath)
		|| !FParse::Value(
			CommandLine,
			TEXT("AvidScriptPerfCostResult="),
			ResultPath))
	{
		UE_LOG(LogTemp, Error, TEXT("ASP54C4601 cost request and result paths are required"));
		return 2;
	}

	FString Error;
	if (!FAvidScriptPerfCostRunner::RunFromFiles(RequestPath, ResultPath, Error))
	{
		UE_LOG(LogTemp, Error, TEXT("ASP54C4602 physical cost ladder failed: %s"), *Error);
		return 3;
	}

	UE_LOG(LogTemp, Display, TEXT("ASP54C4600 physical cost result published: %s"), *ResultPath);
	return 0;
}
