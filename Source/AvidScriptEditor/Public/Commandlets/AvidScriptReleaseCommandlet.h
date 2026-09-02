#pragma once

#include "Commandlets/Commandlet.h"

#include "AvidScriptReleaseCommandlet.generated.h"

UCLASS()
class AVIDSCRIPTEDITOR_API UAvidScriptReleaseCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAvidScriptReleaseCommandlet();

	virtual int32 Main(const FString& Params) override;
};
