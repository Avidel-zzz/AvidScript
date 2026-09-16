#pragma once

#include "Commandlets/Commandlet.h"
#include "As38LeadershipTimingCommandlet.generated.h"

UCLASS()
class UAs38LeadershipTimingCommandlet final : public UCommandlet
{
    GENERATED_BODY()
public:
    UAs38LeadershipTimingCommandlet();
    virtual int32 Main(const FString& Params) override;
};
