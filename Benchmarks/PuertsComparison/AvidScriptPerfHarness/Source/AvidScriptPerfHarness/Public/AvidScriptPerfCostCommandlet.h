#pragma once

#include "Commandlets/Commandlet.h"

#include "AvidScriptPerfCostCommandlet.generated.h"

UCLASS()
class AVIDSCRIPTPERFHARNESS_API UAvidScriptPerfCostCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UAvidScriptPerfCostCommandlet();

	virtual int32 Main(const FString& Params) override;

	static int32 RunFromCommandLine(const TCHAR* CommandLine);
};
