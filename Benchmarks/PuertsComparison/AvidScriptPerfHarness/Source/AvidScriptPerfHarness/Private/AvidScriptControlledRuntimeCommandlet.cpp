#include "AvidScriptControlledRuntimeCommandlet.h"

#include "AvidScriptControlledRuntimeRunner.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

UAvidScriptControlledRuntimeCommandlet::UAvidScriptControlledRuntimeCommandlet()
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UAvidScriptControlledRuntimeCommandlet::Main(const FString& Params)
{
	(void)Params;
	return RunFromCommandLine(FCommandLine::Get());
}

int32 UAvidScriptControlledRuntimeCommandlet::RunFromCommandLine(
	const TCHAR* CommandLine)
{
	FString RequestPath;
	FString ResultPath;
	if (!FParse::Value(
			CommandLine,
			TEXT("AvidScriptControlledRuntimeRequest="),
			RequestPath) ||
		!FParse::Value(
			CommandLine,
			TEXT("AvidScriptControlledRuntimeResult="),
			ResultPath))
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("ASP54C4001 controlled runtime request and result paths are required"));
		return 2;
	}

	FString Error;
	if (!FAvidScriptControlledRuntimeRunner::RunFromFiles(
			RequestPath,
			ResultPath,
			Error))
	{
		UE_LOG(
			LogTemp,
			Error,
			TEXT("ASP54C4002 controlled runtime shootout failed: %s"),
			*Error);
		return 3;
	}

	UE_LOG(
		LogTemp,
		Display,
		TEXT("ASP54C4000 controlled runtime result published: %s"),
		*ResultPath);
	return 0;
}
