#pragma once

#include "Commandlets/Commandlet.h"

#include "AvidScriptPerfPrepareCommandlet.generated.h"

UCLASS()
class AVIDSCRIPTPERFHARNESS_API UAvidScriptPerfPrepareCommandlet final : public UCommandlet
{
	GENERATED_BODY()

public:
	UAvidScriptPerfPrepareCommandlet();

	virtual int32 Main(const FString& Params) override;
};
