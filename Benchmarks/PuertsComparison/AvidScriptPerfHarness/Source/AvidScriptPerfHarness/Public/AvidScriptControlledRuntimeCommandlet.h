#pragma once

#include "Commandlets/Commandlet.h"

#include "AvidScriptControlledRuntimeCommandlet.generated.h"

UCLASS()
class AVIDSCRIPTPERFHARNESS_API UAvidScriptControlledRuntimeCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UAvidScriptControlledRuntimeCommandlet();

	virtual int32 Main(const FString& Params) override;

	static int32 RunFromCommandLine(const TCHAR* CommandLine);
};
