#include "As36LeadershipValidateCommandlet.h"

#include "As36LeadershipBridge.h"
#include "AvidScriptPerfFixture.h"
#include "AvidScriptPerfRunner.h"
#include "AvidScriptGameplayFrameBenchmark.h"
#include "AngelscriptManager.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    namespace As36FrozenNativeOracle
    {
#include "As36FrozenNativeOracle.inl"
    }

    struct FAs36ValidationWorld
    {
        UWorld* World = nullptr;
        bool Create()
        {
            if (!GEngine) { return false; }
            World = UWorld::CreateWorld(EWorldType::Game, false);
            if (!World) { return false; }
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            World->InitializeActorsForPlay(FURL());
            return true;
        }
        void Destroy()
        {
            if (World)
            {
                GEngine->DestroyWorldContext(World);
                World->DestroyWorld(false);
                World = nullptr;
            }
        }
        ~FAs36ValidationWorld() { Destroy(); }
    };

    TArray<TSharedPtr<FJsonValue>> ReadAs36Counts(const AAvidScriptPerfFixture& Fixture)
    {
        TArray<TSharedPtr<FJsonValue>> Values;
        for (int32 Index = 0; Index < 13; ++Index)
        {
            Values.Add(MakeShared<FJsonValueNumber>(static_cast<double>(Fixture.GetOperationCallCount(Index))));
        }
        return Values;
    }
}

UAs36LeadershipValidateCommandlet::UAs36LeadershipValidateCommandlet()
{
    IsClient = false;
    IsEditor = true;
    IsServer = false;
    LogToConsole = true;
    ShowErrorCount = true;
}

int32 UAs36LeadershipValidateCommandlet::Main(const FString& Params)
{
#if !WITH_EDITOR
    UE_LOG(LogTemp, Error, TEXT("AS36 validation commandlet supports Editor VM only; packaged native code needs a game host."));
    return 2;
#else
    FString ResultPath;
    if (!FParse::Value(*Params, TEXT("As36Result="), ResultPath) || ResultPath.IsEmpty() || FPaths::FileExists(ResultPath))
    {
        UE_LOG(LogTemp, Error, TEXT("AS36 requires a fresh -As36Result path."));
        return 2;
    }
    TSharedRef<FJsonObject> Report = MakeShared<FJsonObject>();
    Report->SetNumberField(TEXT("schema_version"), 1);
    Report->SetStringField(TEXT("mode"), TEXT("UE Editor AngelScript VM"));
    Report->SetStringField(TEXT("scope"), TEXT("Same-semantics correctness only; no timings or performance leadership claim"));
    TArray<TSharedPtr<FJsonValue>> Cases;
    TArray<TSharedPtr<FJsonValue>> Controls;
    FString Error;
    auto Require = [&Error](bool bCondition, const FString& Reason)
    {
        if (!bCondition && Error.IsEmpty()) { Error = Reason; }
        return bCondition;
    };
    auto Reject = [&Controls, &Require](bool bAccepted, const TCHAR* Name)
    {
        TSharedRef<FJsonObject> Control = MakeShared<FJsonObject>();
        Control->SetStringField(TEXT("name"), Name);
        Control->SetBoolField(TEXT("rejected"), !bAccepted);
        Controls.Add(MakeShared<FJsonValueObject>(Control));
        return Require(!bAccepted, FString::Printf(TEXT("Invalid operation accepted: %s"), Name));
    };
    auto Validate = [&]() -> bool
    {
        if (!Require(IsInGameThread() && FAngelscriptManager::Get().bDidInitialCompileSucceed,
            TEXT("Game Thread or initial AngelScript compilation unavailable"))) { return false; }
        const TSharedPtr<FAngelscriptClassDesc> Descriptor =
            FAngelscriptManager::Get().GetClass(TEXT("AAs36LeadershipDriver"));
        if (!Require(Descriptor.IsValid() && Descriptor->Class && Descriptor->Class->IsChildOf(AActor::StaticClass()),
            TEXT("Active script driver class is missing"))) { return false; }
        FAs36ValidationWorld Context;
        if (!Require(Context.Create(), TEXT("Cannot create validation World"))) { return false; }
        UWorld* World = Context.World;
        AActor* Actor = World->SpawnActor<AActor>(Descriptor->Class);
        AAvidScriptPerfFixture* Fixture = World->SpawnActor<AAvidScriptPerfFixture>();
        AAvidScriptPerfFixture* Oracle = World->SpawnActor<AAvidScriptPerfFixture>();
        if (!Require(Actor && Fixture && Oracle, TEXT("Cannot spawn actor and fixtures"))) { return false; }
        FAs36LeadershipBridge Bridge;
        if (!Bridge.Bind(*Actor, *Fixture, Error)) { return false; }
        Report->SetStringField(TEXT("entry_identity"), Bridge.GetEntryIdentity());
        Report->SetBoolField(TEXT("float64_ref_out_object_identity_passed"), true);
        const uint32 Seeds[] = {0u, 1u, 1397313u, 0x7fffffffu, 0x80000000u, 0xffffffffu};
        const int32 MicroIterations[] = {1, 2, 17, 257};
        const int32 GameplayIterations[] = {1, 2};
        for (int32 Workload = 0; Workload < 12; ++Workload)
        {
            TConstArrayView<int32> IterationCases = Workload >= 10
                ? MakeArrayView(GameplayIterations) : MakeArrayView(MicroIterations);
            for (uint32 Seed : Seeds)
            {
                for (int32 Iterations : IterationCases)
                {
                    Oracle->ScalarValue = 0;
                    Oracle->ResetNativeCallbackState(static_cast<int32>(Seed));
                    Oracle->ResetOperationCounts(INDEX_NONE);
                    const uint32 Expected = As36FrozenNativeOracle::RunNativeWorkload(*Oracle,
                        static_cast<EAvidScriptPerfWorkload>(Workload), Iterations, Seed);
                    uint32 Actual = 0;
                    if (!Bridge.PrepareSample(Workload, Iterations, static_cast<int32>(Seed), Error) ||
                        !Bridge.ExecutePrepared(Error) || !Bridge.ReadChecksum(Actual, Error)) { return false; }
                    bool bMatched = Actual == Expected && Fixture->ScalarValue == Oracle->ScalarValue;
                    for (int32 Counter = 0; Counter < 13; ++Counter)
                    {
                        bMatched &= Fixture->GetOperationCallCount(Counter) == Oracle->GetOperationCallCount(Counter);
                    }
                    TSharedRef<FJsonObject> Case = MakeShared<FJsonObject>();
                    Case->SetNumberField(TEXT("workload_id"), Workload);
                    Case->SetNumberField(TEXT("iterations"), Iterations);
                    Case->SetNumberField(TEXT("seed_u32"), static_cast<double>(Seed));
                    Case->SetNumberField(TEXT("expected_checksum_u32"), static_cast<double>(Expected));
                    Case->SetNumberField(TEXT("actual_checksum_u32"), static_cast<double>(Actual));
                    Case->SetNumberField(TEXT("expected_scalar"), Oracle->ScalarValue);
                    Case->SetNumberField(TEXT("actual_scalar"), Fixture->ScalarValue);
                    Case->SetArrayField(TEXT("expected_native_calls"), ReadAs36Counts(*Oracle));
                    Case->SetArrayField(TEXT("actual_native_calls"), ReadAs36Counts(*Fixture));
                    Case->SetBoolField(TEXT("matched"), bMatched);
                    Cases.Add(MakeShared<FJsonValueObject>(Case));
                    if (!Require(bMatched, FString::Printf(TEXT("Mismatch workload=%d iterations=%d seed=%u"),
                        Workload, Iterations, Seed))) { return false; }
                }
            }
        }
        if (!Require(Cases.Num() == 264, TEXT("Incomplete 264-case correctness matrix"))) { return false; }
        // Expected rejections return diagnostics; keep these separate from Error.
        FString Rejection;
        uint32 Ignored = 0;
        FAs36LeadershipBridge Unbound;
        if (!Reject(Unbound.PrepareSample(0, 1, 0, Rejection), TEXT("unbound sample")) ||
            !Reject(Unbound.ExecutePrepared(Rejection), TEXT("execute before prepare")) ||
            !Reject(Unbound.ReadChecksum(Ignored, Rejection), TEXT("read before execute")) ||
            !Reject(Bridge.PrepareSample(-1, 1, 0, Rejection), TEXT("negative workload")) ||
            !Reject(Bridge.PrepareSample(12, 1, 0, Rejection), TEXT("unknown workload")) ||
            !Reject(Bridge.PrepareSample(0, 0, 0, Rejection), TEXT("zero iterations")) ||
            !Reject(Bridge.PrepareSample(0, -1, 0, Rejection), TEXT("negative iterations")) ||
            !Reject(Bridge.PrepareSample(0, 10000001, 0, Rejection), TEXT("micro iteration limit")) ||
            !Reject(Bridge.PrepareSample(10, 1048577, 0, Rejection), TEXT("gameplay iteration limit"))) { return false; }
        if (!Bridge.PrepareSample(0, 1, 0, Error) || !Bridge.ExecutePrepared(Error)) { return false; }
        if (!Reject(Bridge.ExecutePrepared(Rejection), TEXT("duplicate execute"))) { return false; }
        if (!Bridge.ReadChecksum(Ignored, Error) ||
            !Reject(Bridge.ReadChecksum(Ignored, Rejection), TEXT("duplicate read"))) { return false; }
        FAs36LeadershipBridge WrongClass;
        if (!Reject(WrongClass.Bind(*Oracle, *Fixture, Rejection), TEXT("native actor substituted for script"))) { return false; }
        FAs36ValidationWorld OtherContext;
        if (!Require(OtherContext.Create(), TEXT("Cannot create second World"))) { return false; }
        AAvidScriptPerfFixture* OtherFixture = OtherContext.World->SpawnActor<AAvidScriptPerfFixture>();
        if (!Require(OtherFixture != nullptr, TEXT("Cannot spawn second World fixture"))) { return false; }
        FAs36LeadershipBridge CrossWorld;
        if (!Reject(CrossWorld.Bind(*Actor, *OtherFixture, Rejection), TEXT("cross World fixture"))) { return false; }
        const bool bAcceptedOffThread = Async(EAsyncExecution::ThreadPool, [&Bridge]()
        {
            FString WorkerError;
            return Bridge.PrepareSample(0, 1, 0, WorkerError);
        }).Get();
        if (!Reject(bAcceptedOffThread, TEXT("non Game Thread sample"))) { return false; }
        AActor* TeardownActor = OtherContext.World->SpawnActor<AActor>(Descriptor->Class);
        if (!Require(TeardownActor != nullptr, TEXT("Cannot spawn teardown control actor"))) { return false; }
        FAs36LeadershipBridge Teardown;
        if (!Teardown.Bind(*TeardownActor, *OtherFixture, Error)) { return false; }
        OtherContext.World->BeginTearingDown();
        if (!Reject(Teardown.PrepareSample(0, 1, 0, Rejection), TEXT("World tearing down"))) { return false; }
        OtherContext.Destroy();

        // Interleave setup for two actors so a shared script-global accumulator fails.
        AActor* OtherActor = World->SpawnActor<AActor>(Descriptor->Class);
        AAvidScriptPerfFixture* SecondFixture = World->SpawnActor<AAvidScriptPerfFixture>();
        if (!Require(OtherActor && SecondFixture, TEXT("Cannot create independent callback actor"))) { return false; }
        FAs36LeadershipBridge Second;
        if (!Second.Bind(*OtherActor, *SecondFixture, Error)) { return false; }
        for (int32 CallbackWorkload : {7, 8})
        {
            const int32 FirstSeed = 17;
            const int32 SecondSeed = static_cast<int32>(0x80000001u);
            if (!Bridge.PrepareSample(CallbackWorkload, 17, FirstSeed, Error) ||
                !Second.PrepareSample(CallbackWorkload, 17, SecondSeed, Error) ||
                !Bridge.ExecutePrepared(Error) || !Second.ExecutePrepared(Error)) { return false; }
            uint32 FirstResult = 0, SecondResult = 0;
            if (!Bridge.ReadChecksum(FirstResult, Error) || !Second.ReadChecksum(SecondResult, Error)) { return false; }
            Oracle->ResetNativeCallbackState(FirstSeed);
            const uint32 FirstExpected = As36FrozenNativeOracle::RunNativeWorkload(*Oracle, static_cast<EAvidScriptPerfWorkload>(CallbackWorkload), 17, FirstSeed);
            Oracle->ResetNativeCallbackState(SecondSeed);
            const uint32 SecondExpected = As36FrozenNativeOracle::RunNativeWorkload(*Oracle, static_cast<EAvidScriptPerfWorkload>(CallbackWorkload), 17, static_cast<uint32>(SecondSeed));
            if (!Require(FirstResult == FirstExpected && SecondResult == SecondExpected,
                TEXT("Callback state leaked between actors"))) { return false; }
        }
        Report->SetBoolField(TEXT("per_actor_callback_state_passed"), true);
        if (!Require(World->DestroyActor(SecondFixture), TEXT("Cannot destroy fixture control")) ||
            !Reject(Second.PrepareSample(0, 1, 0, Rejection), TEXT("destroyed fixture"))) { return false; }
        if (!Require(World->DestroyActor(Actor), TEXT("Cannot destroy actor control")) ||
            !Reject(Bridge.PrepareSample(0, 1, 0, Rejection), TEXT("destroyed actor"))) { return false; }
        return Require(Controls.Num() == 17, TEXT("Incomplete rejection controls"));
    };
    const bool bPassed = Validate();
    Report->SetBoolField(TEXT("passed"), bPassed);
    Report->SetNumberField(TEXT("expected_case_count"), 264);
    Report->SetNumberField(TEXT("completed_case_count"), Cases.Num());
    Report->SetNumberField(TEXT("expected_rejection_controls"), 17);
    Report->SetArrayField(TEXT("cases"), Cases);
    Report->SetArrayField(TEXT("rejection_controls"), Controls);
    Report->SetStringField(TEXT("error"), Error);
    FString Json;
    TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Json);
    if (!FJsonSerializer::Serialize(Report, Writer) ||
        !FFileHelper::SaveStringToFile(Json, *ResultPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogTemp, Error, TEXT("AS36 could not publish correctness report."));
        return 4;
    }
    if (!bPassed)
    {
        UE_LOG(LogTemp, Error, TEXT("AS36 same-semantics validation failed: %s"), *Error);
        return 3;
    }
    UE_LOG(LogTemp, Display, TEXT("AS36_VALIDATED cases=264 controls=17 mode=editor_vm report=%s"), *ResultPath);
    return 0;
#endif
}
