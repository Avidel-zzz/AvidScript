#pragma once

#include "Commandlets/Commandlet.h"
#include "As36LeadershipValidateCommandlet.generated.h"

// Draft: compile into the complete AvidScriptPerfHarness with AngelscriptCode.
UCLASS()
class UAs36LeadershipValidateCommandlet final : public UCommandlet
{
    GENERATED_BODY()
public:
    UAs36LeadershipValidateCommandlet();
    virtual int32 Main(const FString& Params) override;
};
