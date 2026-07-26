#pragma once

#include "Commandlets/Commandlet.h"

#include "AvidScriptPerfRunCommandlet.generated.h"

UCLASS()
class AVIDSCRIPTPERFHARNESS_API UAvidScriptPerfRunCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UAvidScriptPerfRunCommandlet();

	virtual int32 Main(const FString& Params) override;

	static int32 RunFromCommandLine(const TCHAR* CommandLine);
};
