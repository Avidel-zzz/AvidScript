#include "As38LeadershipTimingCommandlet.h"

#include "As36LeadershipBridge.h"
#include "AvidScriptPerfFixture.h"
#include "AvidScriptPerfRunner.h"
#include "AvidScriptGameplayFrameBenchmark.h"
#include "AngelscriptManager.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace As38Timing
{
    namespace Oracle
    {
#include "As36FrozenNativeOracle.inl"
    }

    constexpr int32 MicroIds[] = {7, 8, 0, 1, 2, 3, 4, 9, 5, 6};
    constexpr int32 GameplayIds[] = {10, 11};
    constexpr const TCHAR* Names[] = {TEXT("pure_integer"), TEXT("scalar_noop"), TEXT("scalar_add_int32"),
        TEXT("property_get_set"), TEXT("vector_value"), TEXT("object_roundtrip"), TEXT("batch_scalar"),
        TEXT("callback_empty"), TEXT("callback_tick"), TEXT("vector_ref_out"), TEXT("gameplay_frame_small"), TEXT("gameplay_frame_dense")};
    constexpr const TCHAR* Lanes[] = {TEXT("native_cpp"), TEXT("angelscript_editor_vm")};

    struct FWorldOwner
    {
        UWorld* World = nullptr;
        ~FWorldOwner()
        {
            if (World) { GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        }
        bool Create()
        {
            if (!GEngine) { return false; }
            World = UWorld::CreateWorld(EWorldType::Game, false);
            if (!World) { return false; }
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            World->InitializeActorsForPlay(FURL());
            return true;
        }
    };

    bool Integer(const FJsonObject& Object, const TCHAR* Name, int32 Minimum, int32 Maximum, int32& Out)
    {
        double Value = 0;
        if (!Object.TryGetNumberField(Name, Value) || !FMath::IsFinite(Value) ||
            Value < Minimum || Value > Maximum || FMath::FloorToDouble(Value) != Value) { return false; }
        Out = static_cast<int32>(Value);
        return true;
    }

    uint32 SampleSeed(int32 WorkloadIndex, int32 SampleIndex)
    {
        // Exact existing MakePerfRunnerSampleSeed contract, including 23-bit mask.
        return Oracle::PerfRunnerMix(1397313u ^ (static_cast<uint32>(WorkloadIndex + 1) * 0x9e3779b9u)
            ^ static_cast<uint32>(SampleIndex + 1)) & 0x007fffffu;
    }

    int64 ExpectedCalls(int32 Workload, int32 Counter, int32 Iterations)
    {
        if (Workload == 1 || Workload == 2 || Workload == 4 || Workload == 5 || Workload == 6 || Workload == 9)
        { return Counter == Workload ? Iterations : 0; }
        if (Workload == 10)
        {
            return static_cast<int64>(Iterations) * (Counter == 2 ? 32 : Counter == 4 ? 8 : Counter == 5 ? 4 : Counter == 12 ? 2 : 0);
        }
        if (Workload == 11)
        {
            return static_cast<int64>(Iterations) * (Counter == 2 ? 4096 : Counter == 4 ? 2048 : Counter == 5 || Counter == 12 ? 512 : 0);
        }
        return 0;
    }

    TArray<TSharedPtr<FJsonValue>> Counts(const AAvidScriptPerfFixture& Fixture)
    {
        TArray<TSharedPtr<FJsonValue>> Result;
        for (int32 Index = 0; Index < 13; ++Index) { Result.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Fixture.GetOperationCallCount(Index)))); }
        return Result;
    }

    void Reset(AAvidScriptPerfFixture& Fixture, uint32 Seed)
    {
        Fixture.ScalarValue = 0;
        Fixture.ResetNativeCallbackState(static_cast<int32>(Seed));
        Fixture.ResetOperationCounts(INDEX_NONE);
    }
}

UAs38LeadershipTimingCommandlet::UAs38LeadershipTimingCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
    ShowErrorCount = true;
}

int32 UAs38LeadershipTimingCommandlet::Main(const FString& Params)
{
    using namespace As38Timing;
#if !WITH_EDITOR
    UE_LOG(LogTemp, Error, TEXT("AS38 Editor timing cannot certify packaged generated native code."));
    return 2;
#else
    FString RequestPath, ResultPath, RequestText;
    if (!FParse::Value(*Params, TEXT("As38Request="), RequestPath) ||
        !FParse::Value(*Params, TEXT("As38Result="), ResultPath) || ResultPath.IsEmpty() || FPaths::FileExists(ResultPath) ||
        !FFileHelper::LoadFileToString(RequestText, *RequestPath) || RequestText.Len() > 65536)
    {
        UE_LOG(LogTemp, Error, TEXT("AS38 requires a bounded request and a fresh result path."));
        return 2;
    }
    TSharedPtr<FJsonObject> Request;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(RequestText), Request) || !Request.IsValid()) { return 2; }
    FString Mode, Suite, RequestId, ExecutionMode;
    int32 Version = 0, ProcessRuns = 0, ProcessRun = 0, Seed = 0, Warmups = 0, Timed = 0;
    if (!Request->TryGetStringField(TEXT("mode"), Mode) || (Mode != TEXT("calibration") && Mode != TEXT("measure")) ||
        !Request->TryGetStringField(TEXT("suite"), Suite) || (Suite != TEXT("micro") && Suite != TEXT("gameplay")) ||
        !Request->TryGetStringField(TEXT("request_id"), RequestId) || RequestId.Len() != 32 ||
        !Request->TryGetStringField(TEXT("execution_mode"), ExecutionMode) || ExecutionMode != TEXT("editor_vm") ||
        !Integer(*Request, TEXT("schema_version"), 1, 1, Version) ||
        !Integer(*Request, TEXT("seed"), 1397313, 1397313, Seed) ||
        !Integer(*Request, TEXT("process_runs"), 1, 5, ProcessRuns) || (ProcessRuns != 1 && ProcessRuns != 5) ||
        !Integer(*Request, TEXT("process_run"), -1, ProcessRuns - 1, ProcessRun) ||
        !Integer(*Request, TEXT("warmup_samples"), 5, 5, Warmups) ||
        !Integer(*Request, TEXT("timed_samples"), 30, 30, Timed) ||
        (Mode == TEXT("calibration") ? ProcessRun != -1 : ProcessRun < 0))
    {
        UE_LOG(LogTemp, Error, TEXT("AS38 request does not match the controlled timing contract."));
        return 2;
    }
    for (TCHAR Character : RequestId) { if (!FChar::IsHexDigit(Character)) { return 2; } }
    const bool bCalibration = Mode == TEXT("calibration");
    const bool bGameplay = Suite == TEXT("gameplay");
    const TConstArrayView<int32> Workloads = bGameplay ? MakeArrayView(GameplayIds) : MakeArrayView(MicroIds);
    const int32 MinimumIterations = bGameplay ? 1 : 1000;
    const int32 MaximumIterations = bGameplay ? 1048576 : 10000000;
    int32 FrozenIterations[12][2] = {};
    if (!bCalibration)
    {
        const TSharedPtr<FJsonObject>* IterationObject = nullptr;
        if (!Request->TryGetObjectField(TEXT("iteration_counts"), IterationObject) || (*IterationObject)->Values.Num() != Workloads.Num()) { return 2; }
        for (int32 Id : Workloads)
        {
            const TSharedPtr<FJsonObject>* LaneObject = nullptr;
            if (!(*IterationObject)->TryGetObjectField(Names[Id], LaneObject) || (*LaneObject)->Values.Num() != 2) { return 2; }
            for (int32 Lane = 0; Lane < 2; ++Lane)
            {
                if (!Integer(**LaneObject, Lanes[Lane], MinimumIterations, MaximumIterations, FrozenIterations[Id][Lane])) { return 2; }
            }
        }
    }
    TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetNumberField(TEXT("schema_version"), 1);
    Report->SetStringField(TEXT("request_id"), RequestId);
    Report->SetStringField(TEXT("mode"), Mode);
    Report->SetStringField(TEXT("suite"), Suite);
    Report->SetStringField(TEXT("execution_mode"), TEXT("editor_vm"));
    Report->SetStringField(TEXT("scope"), TEXT("Controlled Native/AngelScript timings; complete cross-framework leadership remains unverified"));
    Report->SetNumberField(TEXT("process_run"), ProcessRun);
    Report->SetNumberField(TEXT("process_runs"), ProcessRuns);
    Report->SetNumberField(TEXT("warmup_samples"), Warmups);
    Report->SetNumberField(TEXT("timed_samples"), Timed);
    Report->SetNumberField(TEXT("seconds_per_cycle"), FPlatformTime::GetSecondsPerCycle64());
    Report->SetNumberField(TEXT("minimum_sample_milliseconds"), 5.0);
    Report->SetNumberField(TEXT("calibration_confirmation_samples"), 3);
    Report->SetBoolField(TEXT("static_jit_transpiled_code_loaded"), FAngelscriptManager::bStaticJITTranspiledCodeLoaded);
    TArray<TSharedPtr<FJsonValue>> Samples;
    FString Error;
    auto Run = [&]() -> bool
    {
        if (!IsInGameThread() || !FAngelscriptManager::Get().bDidInitialCompileSucceed ||
            FAngelscriptManager::bStaticJITTranspiledCodeLoaded)
        { Error = TEXT("Expected compiled Editor VM with no static JIT transpiled code"); return false; }
        const TSharedPtr<FAngelscriptClassDesc> Descriptor = FAngelscriptManager::Get().GetClass(TEXT("AAs36LeadershipDriver"));
        if (!Descriptor.IsValid() || !Descriptor->Class) { Error = TEXT("Missing script class"); return false; }
        FWorldOwner Context;
        if (!Context.Create()) { Error = TEXT("Cannot create timing World"); return false; }
        AActor* Actor = Context.World->SpawnActor<AActor>(Descriptor->Class);
        AAvidScriptPerfFixture* ScriptFixture = Context.World->SpawnActor<AAvidScriptPerfFixture>();
        AAvidScriptPerfFixture* NativeFixture = Context.World->SpawnActor<AAvidScriptPerfFixture>();
        AAvidScriptPerfFixture* OracleFixture = Context.World->SpawnActor<AAvidScriptPerfFixture>();
        if (!Actor || !ScriptFixture || !NativeFixture || !OracleFixture) { Error = TEXT("Cannot spawn timing fixtures"); return false; }
        FAs36LeadershipBridge Bridge;
        if (!Bridge.Bind(*Actor, *ScriptFixture, Error)) { return false; }
        Report->SetStringField(TEXT("entry_identity"), Bridge.GetEntryIdentity());
        auto Sample = [&](int32 WorkloadIndex, int32 Lane, int32 Index, int32 Position, const TCHAR* Phase, int32 Iterations, double& Milliseconds) -> bool
        {
            const int32 Id = Workloads[WorkloadIndex];
            const uint32 SampleSeedValue = SampleSeed(WorkloadIndex, Index);
            Reset(*OracleFixture, SampleSeedValue);
            const uint32 Expected = Oracle::RunNativeWorkload(*OracleFixture, static_cast<EAvidScriptPerfWorkload>(Id), Iterations, SampleSeedValue);
            AAvidScriptPerfFixture& Fixture = Lane == 0 ? *NativeFixture : *ScriptFixture;
            if (Lane == 0) { Reset(Fixture, SampleSeedValue); }
            else if (!Bridge.PrepareSample(Id, Iterations, static_cast<int32>(SampleSeedValue), Error)) { return false; }
            uint32 Actual = 0;
            bool bExecuted = true;
            const FPlatformMemoryStats BeforeMemory = FPlatformMemory::GetStats();
            const uint64 Start = FPlatformTime::Cycles64();
            if (Lane == 0) { Actual = Oracle::RunNativeWorkload(Fixture, static_cast<EAvidScriptPerfWorkload>(Id), Iterations, SampleSeedValue); }
            else { bExecuted = Bridge.ExecutePrepared(Error); }
            const uint64 End = FPlatformTime::Cycles64();
            const FPlatformMemoryStats AfterMemory = FPlatformMemory::GetStats();
            if (!bExecuted || (Lane != 0 && !Bridge.ReadChecksum(Actual, Error))) { return false; }
            const uint64 Cycles = End - Start;
            bool bMatched = Actual == Expected && Fixture.ScalarValue == OracleFixture->ScalarValue && End > Start && Cycles < (1ull << 53);
            for (int32 Counter = 0; Counter < 13; ++Counter)
            {
                bMatched &= Fixture.GetOperationCallCount(Counter) == ExpectedCalls(Id, Counter, Iterations) &&
                    OracleFixture->GetOperationCallCount(Counter) == ExpectedCalls(Id, Counter, Iterations);
            }
            Milliseconds = static_cast<double>(Cycles) * FPlatformTime::GetSecondsPerCycle64() * 1000.0;
            TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
            Row->SetStringField(TEXT("phase"), Phase);
            Row->SetStringField(TEXT("lane"), Lanes[Lane]);
            Row->SetNumberField(TEXT("lane_position"), Position);
            Row->SetNumberField(TEXT("workload_id"), Id);
            Row->SetStringField(TEXT("workload"), Names[Id]);
            Row->SetNumberField(TEXT("workload_index"), WorkloadIndex);
            Row->SetNumberField(TEXT("sample_index"), Index);
            Row->SetNumberField(TEXT("seed_u32"), SampleSeedValue);
            Row->SetNumberField(TEXT("iterations"), Iterations);
            Row->SetNumberField(TEXT("elapsed_cycles"), static_cast<double>(Cycles));
            Row->SetNumberField(TEXT("milliseconds"), Milliseconds);
            Row->SetNumberField(TEXT("working_set_delta_bytes"), static_cast<double>(static_cast<int64>(AfterMemory.UsedPhysical) - static_cast<int64>(BeforeMemory.UsedPhysical)));
            Row->SetNumberField(TEXT("actual_checksum_u32"), Actual);
            Row->SetNumberField(TEXT("expected_checksum_u32"), Expected);
            Row->SetNumberField(TEXT("actual_scalar"), Fixture.ScalarValue);
            Row->SetNumberField(TEXT("expected_scalar"), OracleFixture->ScalarValue);
            Row->SetArrayField(TEXT("actual_native_calls"), Counts(Fixture));
            Row->SetArrayField(TEXT("expected_native_calls"), Counts(*OracleFixture));
            Row->SetBoolField(TEXT("matched"), bMatched);
            Samples.Add(MakeShared<FJsonValueObject>(Row));
            if (!bMatched) { Error = FString::Printf(TEXT("Sample mismatch: workload=%d lane=%d phase=%s index=%d"), Id, Lane, Phase, Index); }
            return bMatched;
        };
        for (int32 WorkloadIndex = 0; WorkloadIndex < Workloads.Num(); ++WorkloadIndex)
        {
            const int32 Id = Workloads[WorkloadIndex];
            if (bCalibration)
            {
                for (int32 Lane = 0; Lane < 2; ++Lane)
                {
                    int32 Iterations = MinimumIterations, Round = 0;
                    for (;;)
                    {
                        double Milliseconds = 0;
                        if (!Sample(WorkloadIndex, Lane, Round++, Lane, TEXT("calibration_probe"), Iterations, Milliseconds)) { return false; }
                        if (Milliseconds >= 5.0)
                        {
                            TArray<double> Confirmation;
                            for (int32 Index = 0; Index < 3; ++Index)
                            {
                                if (!Sample(WorkloadIndex, Lane, Round++, Lane, TEXT("calibration_confirm"), Iterations, Milliseconds)) { return false; }
                                Confirmation.Add(Milliseconds);
                            }
                            Confirmation.Sort();
                            if (Confirmation[1] >= 5.0) { FrozenIterations[Id][Lane] = Iterations; break; }
                        }
                        if (Iterations >= MaximumIterations) { Error = TEXT("Could not confirm 5ms steady state before the iteration limit"); return false; }
                        Iterations = Iterations > MaximumIterations / 2 ? MaximumIterations : Iterations * 2;
                    }
                }
            }
            else
            {
                for (int32 Phase = 0; Phase < 2; ++Phase)
                {
                    for (int32 Index = 0; Index < (Phase == 0 ? Warmups : Timed); ++Index)
                    {
                        for (int32 Position = 0; Position < 2; ++Position)
                        {
                            const int32 Lane = (ProcessRun + WorkloadIndex + Index + Position) % 2;
                            double Milliseconds = 0;
                            if (!Sample(WorkloadIndex, Lane, Index, Position, Phase == 0 ? TEXT("warmup") : TEXT("timed"), FrozenIterations[Id][Lane], Milliseconds)) { return false; }
                        }
                    }
                }
            }
        }
        return true;
    };
    const bool bPassed = Run();
    Report->SetBoolField(TEXT("passed"), bPassed);
    Report->SetStringField(TEXT("error"), Error);
    Report->SetArrayField(TEXT("samples"), Samples);
    TSharedRef<FJsonObject> IterationObject = MakeShared<FJsonObject>();
    for (int32 Id : Workloads)
    {
        TSharedRef<FJsonObject> LaneObject = MakeShared<FJsonObject>();
        for (int32 Lane = 0; Lane < 2; ++Lane) { LaneObject->SetNumberField(Lanes[Lane], FrozenIterations[Id][Lane]); }
        IterationObject->SetObjectField(Names[Id], LaneObject);
    }
    Report->SetObjectField(TEXT("iteration_counts"), IterationObject);
    FString Json;
    if (!FJsonSerializer::Serialize(Report, TJsonWriterFactory<>::Create(&Json)) ||
        !FFileHelper::SaveStringToFile(Json, *ResultPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { return 4; }
    if (!bPassed) { UE_LOG(LogTemp, Error, TEXT("AS38 timing failed: %s"), *Error); return 3; }
    UE_LOG(LogTemp, Display, TEXT("AS38_TIMING_VALIDATED mode=%s suite=%s samples=%d"), *Mode, *Suite, Samples.Num());
    return 0;
#endif
}
