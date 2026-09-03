#pragma once

#include "Commandlets/Commandlet.h"
#include "AvidScriptPublishProfileBindingsCommandlet.generated.h"

UCLASS()
class UAvidScriptPublishProfileBindingsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UAvidScriptPublishProfileBindingsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
