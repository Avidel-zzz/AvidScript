#pragma once

#include "CoreMinimal.h"
#include "UObject/StructOnScope.h"

class AActor;
class AAvidScriptPerfFixture;
class UWorld;

// Caller owns World, actor and fixture lifetimes and prevents
// reload/GC between PrepareSample and ReadChecksum. No raw AS context bypass.
class FAs36LeadershipBridge final
{
public:
    bool Bind(AActor& InActor, AAvidScriptPerfFixture& InFixture, FString& OutError);
    bool PrepareSample(int32 Workload, int32 Iterations, int32 Seed, FString& OutError);
    bool ExecutePrepared(FString& OutError);
    bool ReadChecksum(uint32& OutChecksum, FString& OutError);
    FString GetEntryIdentity() const;

private:
    struct FEntry
    {
        UFunction* Function = nullptr;
        TUniquePtr<FStructOnScope> Frame;
        void* Memory() const { return Frame->GetStructMemory(); }
    };

    bool BindEntry(FEntry& Entry, FName Name, int32 ExpectedParameterCount, FString& OutError);
    bool CheckLifetime(FString& OutError) const;
    TWeakObjectPtr<AActor> Actor;
    TWeakObjectPtr<AAvidScriptPerfFixture> Fixture;
    TWeakObjectPtr<UClass> BoundClass;
    TWeakObjectPtr<UWorld> World;
    FEntry Initialize, ValidateTypes, Run, Reset, Empty, Tick, Read;
    int32* RunWorkload = nullptr;
    int32* RunIterations = nullptr;
    int32* RunSeed = nullptr;
    int32* RunResult = nullptr;
    int32* ResetSeed = nullptr;
    int32* EmptyValue = nullptr;
    float* TickDelta = nullptr;
    int32* ReadResult = nullptr;
    int32 SelectedWorkload = INDEX_NONE;
    int32 SelectedIterations = 0;
    AActor* PreparedActor = nullptr;
    bool bBound = false;
    bool bExecuted = false;
};
