#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptTypedCancellationTestSupport.h"
#include "AvidScriptCancellationTaskObserver.h"
#include "Continuation/AvidScriptAsyncObjectLoader.h"
#include "Ownership/AvidScriptSessionObjectOwnership.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformMisc.h"
#include "Misc/ScopeExit.h"
#include "UObject/GarbageCollection.h"

namespace AvidScript::Tests::ObjectCancellation
{
struct FCancelState { int32 Count = 0; };
class FObservedHost final : public IAvidScriptContinuationHost
{
public:
    explicit FObservedHost(IAvidScriptContinuationHost& InHost) : Host(InHost) { }
    int64 ObjectToken = 0;
    int64 SourceToken = 0;
    bool IsInvocationContextLive(const UWorld* World) const override { return Host.IsInvocationContextLive(World); }
    bool AcceptUnboundContinuation(int64 Token) const override { return Host.AcceptUnboundContinuation(Token); }
    int64 ScheduleDelay(float Delay, int32 Callback) override { return Host.ScheduleDelay(Delay, Callback); }
    int64 ScheduleDelayWithCancelResume(float Delay, int32 Callback) override { return Host.ScheduleDelayWithCancelResume(Delay, Callback); }
    int64 ScheduleObjectLoad(FString Path, int32 Callback) override { return Host.ScheduleObjectLoad(MoveTemp(Path), Callback); }
    int64 ScheduleObjectLoadWithCancelResume(FString Path, int32 Callback) override
    { ObjectToken = Host.ScheduleObjectLoadWithCancelResume(MoveTemp(Path), Callback); return ObjectToken; }
    bool Cancel(int64 Token) override { return Host.Cancel(Token); }
    bool ReadCancellationCause(int64 Token, int64& Cause) const override { return Host.ReadCancellationCause(Token, Cause); }
    int64 CreateCancellationSource() override
    {
        const int64 Created = Host.CreateCancellationSource();
        if (SourceToken == 0) SourceToken = Created;
        return Created;
    }
    EAvidScriptCancellationSourceStatus GetCancellationSourceStatus(int64 Token) const override { return Host.GetCancellationSourceStatus(Token); }
    bool CancelCancellationSource(int64 Token) override { return Host.CancelCancellationSource(Token); }
    bool ReleaseCancellationSource(int64 Token) override { return Host.ReleaseCancellationSource(Token); }
    bool BindCancellationSource(int64 Source, int64 Token) override { return Host.BindCancellationSource(Source, Token); }
    bool StoreState(int64 Token, TConstArrayView<uint8> Bytes) override { return Host.StoreState(Token, Bytes); }
    bool ReadState(int64 Token, TArrayView<uint8> Bytes) override { return Host.ReadState(Token, Bytes); }
    bool ConsumeResult(int64 Token, int32 Slot, int32 Generation, const FString& Type, FAvidScriptBindingLatentCompletionPayload& Payload) override
    { return Host.ConsumeResult(Token, Slot, Generation, Type, Payload); }
    bool StoreManagedState(int64 Token, const FAvidScriptWasmRuntimeInstance& Runtime, TConstArrayView<uint8> Bytes,
        TUniquePtr<IAvidScriptContinuationStateLease>&& Lease) override
    { return Host.StoreManagedState(Token, Runtime, Bytes, MoveTemp(Lease)); }
    bool ReadManagedState(int64 Token, const FAvidScriptWasmRuntimeInstance& Runtime, TArrayView<uint8> Bytes) override
    { return Host.ReadManagedState(Token, Runtime, Bytes); }
private:
    IAvidScriptContinuationHost& Host;
};
class FLoadHandle final : public IAvidScriptAsyncObjectLoadHandle
{
public:
    explicit FLoadHandle(TSharedRef<FCancelState> InState) : State(InState) { }
    void Cancel() override { if (State->Count == 0) ++State->Count; }
private:
    TSharedRef<FCancelState> State;
};
class FLoader final : public IAvidScriptAsyncObjectLoader
{
public:
    struct FRequest
    {
        FCompletion Completion;
        TSharedRef<FCancelState> State = MakeShared<FCancelState>();
        TWeakPtr<IAvidScriptAsyncObjectLoadHandle> Handle;
    };
    TArray<TSharedRef<FRequest>> Requests;
    TSharedPtr<IAvidScriptAsyncObjectLoadHandle> RequestAsyncLoad(const FSoftObjectPath& Path, FCompletion&& Completion) override
    {
        auto Request = MakeShared<FRequest>();
        Request->Completion = MoveTemp(Completion);
        Requests.Add(Request);
        auto Handle = MakeShared<FLoadHandle>(Request->State);
        Request->Handle = Handle;
        return Handle;
    }
    void Complete(UObject* Object)
    {
        if (Requests.IsEmpty() || !Requests.Last()->Completion) return;
        FCompletion Completion = MoveTemp(Requests.Last()->Completion);
        Completion(Object);
    }
};

struct FFixture
{
    FAvidScriptWasmReloadManifest Manifest;
    TArray<uint8> Bytes;
    TMap<FString, int32> Offsets;
    TArray<TSharedPtr<FJsonValue>> Cases;
    bool Load(FAutomationTestBase& Test, const FString& Root)
    {
        FAvidScriptWasmReloadManifestLoadResult Loaded;
        if (!Test.TestTrue(TEXT("Formal manifest verifies executed object bytes"),
            FAvidScriptWasmReloadManifestLoader::LoadFromFile(Root / TEXT("cold.avidscript.json"), Manifest, Bytes, Loaded)))
        { Test.AddError(Loaded.ErrorMessage); return false; }
        TArray<uint8> IrBytes;
        FString Json;
        TSharedPtr<FJsonObject> Ir;
        if (!Test.TestTrue(TEXT("Exact compiler IR exists"), FFileHelper::LoadFileToArray(IrBytes, *(Root / TEXT("cold/flow.guestir.json"))))
            || !Test.TestEqual(TEXT("Layout is the manifest's verified IR"), FAvidScriptHash::Sha256Hex(IrBytes), Manifest.DebugProvenance.GuestIrSha256)) return false;
        FFileHelper::BufferToString(Json, IrBytes.GetData(), IrBytes.Num());
        if (!Test.TestTrue(TEXT("Compiler IR parses"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Ir)) || !Ir.IsValid()) return false;
        if (!Test.TestEqual(TEXT("Paired object execution schema"), Ir->GetIntegerField(TEXT("schema_version")), 39)
            || !Test.TestEqual(TEXT("Paired object execution version"), Ir->GetStringField(TEXT("ir_version")), FString(TEXT("1.38")))
            || !Test.TestEqual(TEXT("Actual module identity"), Ir->GetStringField(TEXT("module_id")), Manifest.ModuleId)) return false;
        for (const auto& Value : Ir->GetObjectField(TEXT("memory_layout"))->GetArrayField(TEXT("state_slots")))
        {
            const auto Slot = Value->AsObject();
            if (Slot->GetStringField(TEXT("type_id")) != TEXT("type:int32")) continue;
            const FString Id = Slot->GetStringField(TEXT("global_id"));
            const int32 Offset = Slot->GetIntegerField(TEXT("offset"));
            if (!Test.TestTrue(TEXT("Readback slot is unique and aligned"), !Offsets.Contains(Id)
                && Slot->GetIntegerField(TEXT("size")) == 4 && Offset >= 0 && Offset <= 65532 && Offset % 4 == 0)) return false;
            Offsets.Add(Id, Offset);
        }
        if (!Test.TestTrue(TEXT("Unchanged-source .NET case oracle exists"), FFileHelper::LoadFileToString(Json, *(Root / TEXT("reference-cases.json")))
            && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Cases) && Cases.Num() == 10)) return false;
        return true;
    }
    int32 Offset(const TCHAR* Field) const
    {
        // Static source preparation specializes storage by owner and source
        // symbol identity; these fields no longer use ordinary global IDs.
        const FString SourceIdentity = FString::Printf(
            TEXT("type:global::ObjectCancellationScript\nsymbol:field:global::ObjectCancellationScript.%s:int32"), Field);
        FTCHARToUTF8 Utf8(*SourceIdentity);
        TArray<uint8> Key;
        Key.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
        const FString Id = TEXT("global:symbol:field:$static:") + FAvidScriptHash::Sha256Hex(Key);
        const int32* Found = Offsets.Find(Id);
        return Found ? *Found : -1;
    }
    int32 Read(FAutomationTestBase& Test, const FAvidScriptWasmRuntimeInstance& Runtime, const TCHAR* Field) const
    {
        int32 Value = MIN_int32;
        FString Error;
        if (Offset(Field) < 0 || !Runtime.ReadStateBytes(Offset(Field), MakeArrayView(reinterpret_cast<uint8*>(&Value), sizeof(Value)), Error))
            Test.AddError(FString(TEXT("Fixture readback: ")) + Field + TEXT(" ") + Error);
        return Value;
    }
};

static void CheckEmpty(FAutomationTestBase& Test, FAvidScriptSessionContinuations& Owner,
    const FAvidScriptSessionObjectOwnership& Ownership)
{
    Test.TestEqual(TEXT("No active object continuations"), Owner.GetActiveCount(), 0);
    Test.TestEqual(TEXT("No prepared object continuations"), Owner.GetPreparedCount(), 0);
    Test.TestEqual(TEXT("No saved object frames"), Owner.GetStateFrameByteCountForTesting(), 0);
    Test.TestEqual(TEXT("No source bindings"), Owner.GetCancellationBindingCountForTesting(), 0);
    Test.TestEqual(TEXT("No cancellation sources"), Owner.GetCancellationSourceCountForTesting(), 0);
    Test.TestEqual(TEXT("No pending Tasks"), Owner.GetTaskResultsForTesting().GetCount(), 0);
    Test.TestEqual(TEXT("No Task waiters"), Owner.GetTaskResultsForTesting().GetWaiterCount(), 0);
    Test.TestEqual(TEXT("No borrowed loaded objects"), Ownership.GetBorrowedHandleCount(), 0);
    Test.TestEqual(TEXT("No retained loaded objects"), Owner.GetRetainedLoadedObjectCountForTesting(), 0);
    Test.TestEqual(TEXT("No object result slots"), Owner.GetResultSlotCountForTesting(), 0);
    Test.TestEqual(TEXT("No ready active completions"), Owner.GetReadyCountForTesting(EAvidScriptContinuationLane::Active), 0);
    Test.TestEqual(TEXT("No ready prepared completions"), Owner.GetReadyCountForTesting(EAvidScriptContinuationLane::Prepared), 0);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptCompiledObjectCancellationTest,
    "AvidScript.Runtime.Continuation.CompiledObjectLoadCancellation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompiledObjectCancellationTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::ObjectCancellation;
    if (!GEngine) return false;
    FFixture Fixture;
    if (!Fixture.Load(*this, FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_OBJECT_CANCELLATION_DIR")))) return false;
    int32 Passed = 0;
    for (const auto Backend : {EAvidScriptVmBackendKind::Wasmtime, EAvidScriptVmBackendKind::Wamr})
    for (int32 Case = 0; Case < 14; ++Case)
    for (int32 Lane = 0; Lane < (Case < 10 ? 2 : 1); ++Lane)
    {
        const bool Retire = Case >= 10;
        const bool Prepared = Lane == 1 || Case == 13;
        const bool Cancel = Case >= 3 && Case < 10;
        const bool Success = Case == 0 || Case == 2;
        const int32 Mode = Case == 2 ? 1 : Case == 4 ? 2 : Case == 5 ? 3 : 0;
        const FString Label = FString::Printf(TEXT("backend=%d case=%d lane=%d"), static_cast<int32>(Backend), Case, Lane);
        UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
        if (!TestNotNull(TEXT("Object execution world created"), World)) return false;
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        ON_SCOPE_EXIT { World->bIsTearingDown = false; GEngine->DestroyWorldContext(World); World->DestroyWorld(false); };
        FAvidScriptObjectRegistry Registry;
        FAvidScriptSessionObjectOwnership Ownership;
        AActor* Actor = World->SpawnActor<AActor>();
        FAvidScriptObjectHandleResult Registered;
        const auto ActorHandle = Registry.RegisterObject(Actor, Registered, false);
        auto Loader = MakeShared<FLoader>();
        auto Owner = MakeShared<FAvidScriptSessionContinuations>(Loader);
        auto& Endpoint = Prepared ? Owner->BeginPrepared(World, &Registry, &Ownership, ActorHandle)
            : Owner->ResetActive(World, &Registry, &Ownership, ActorHandle);
        FObservedHost ObservedHost(Endpoint);
        AvidScript::Tests::CompiledCancellation::FTaskObserver Observer(Endpoint);
        FAvidScriptWasmRuntimeInstance Runtime(AvidScript::Tests::TypedCancellation::Selection(Backend));
        FAvidScriptWasmSmokeResult Result;
        if (!TestTrue(*Label, Runtime.LoadModule(Fixture.Bytes.GetData(), Fixture.Bytes.Num(), Fixture.Manifest.ModuleId, Result))
            || !TestTrue(TEXT("Prepare the actual status-aware continuation export"), Runtime.ValidateRequiredExports(
                {TEXT("avid_on_begin_play"), TEXT("avid_on_tick"), TEXT("avid_on_end_play"), TEXT("avid_on_continuation_v2")}, Result)))
        { AddError(Result.ErrorMessage); return false; }
        FAvidScriptWasmHostContext Context;
        Context.Tasks = &Observer; Context.Continuations = &ObservedHost;
        Context.World = World; Context.ObjectRegistry = &Registry; Context.ObjectOwnership = &Ownership;
        Context.OwnerHandle = ActorHandle;
        Runtime.SetHostContext(Context);
        FString Error;
        if (!TestTrue(TEXT("Select unchanged source mode"), Fixture.Offset(TEXT("Mode")) >= 0 && Runtime.WriteStateBytes(
            Fixture.Offset(TEXT("Mode")), MakeArrayView(reinterpret_cast<const uint8*>(&Mode), sizeof(Mode)), Error))
            || !TestTrue(*Label, Runtime.BeginPlay(Result))) { AddError(Error + Result.ErrorMessage); return false; }
        Observer.CaptureEntryStates();
        auto Read = [&](const TCHAR* Field) { return Fixture.Read(*this, Runtime, Field); };
        if (Case != 5)
        {
            TestEqual(TEXT("Source is suspended before cleanup"), Read(TEXT("Trace")), 0);
            TestEqual(TEXT("One object producer is registered"), Loader->Requests.Num(), 1);
            TestTrue(TEXT("Suspension stores a nonempty state frame"), Owner->GetStateFrameByteCountForTesting() > 0);
        }
        auto* Heap = Runtime.GetManagedHeapForTesting();
        TestEqual(TEXT("Suspended managed state can collect"), Heap->Collect(), AvidScript::Managed::EHeapError::Ok);
        CollectGarbage(RF_NoFlags);
        TWeakObjectPtr<UObject> LoadedWeak;
        if (Case == 0 || Case == 2 || Case == 6)
        {
            UObject* Loaded = NewObject<USceneComponent>(GetTransientPackage());
            LoadedWeak = Loaded;
            Loader->Complete(Loaded);
        }
        else if (Case == 1 || Case == 7) Loader->Complete(nullptr);
        if (Cancel && Case != 5 && Case != 9)
            TestTrue(TEXT("Source cancellation request succeeds"), Runtime.Tick(-1.0f, Result));
        int64 Replacement = 0;
        if (Case == 8)
        {
            TestTrue(TEXT("Released source leaves original token values intact"), Runtime.Tick(-2.0f, Result));
            Replacement = Endpoint.CreateCancellationSource();
        }
        TArray<FAvidScriptContinuationCompletion> Ready;
        if (Case == 9)
        {
            TestNotEqual(TEXT("Actual object import returned an opaque token"), ObservedHost.ObjectToken, 0LL);
            TestTrue(TEXT("Direct cancellation targets the actual object producer"), Endpoint.Cancel(ObservedHost.ObjectToken));
        }
        if (Retire)
        {
            if (Case == 10) World->bIsTearingDown = true;
            else if (Case == 11) { FAvidScriptObjectHandleResult Released; Registry.ReleaseHandle(ActorHandle, Released, false); }
            else if (Case == 12) Owner->Teardown();
            else Owner->DiscardPrepared();
            Owner->DrainReady(Ready);
            TestEqual(TEXT("Retirement cannot invoke Guest"), Ready.Num(), 0);
            Loader->Complete(NewObject<USceneComponent>(GetTransientPackage()));
            Loader->Complete(nullptr);
            Owner->DrainReady(Ready);
            TestEqual(TEXT("Late producer cannot revive retired execution"), Ready.Num(), 0);
            TestEqual(TEXT("Retirement does not run script finally"), Read(TEXT("Trace")), 0);
            TestEqual(TEXT("Retirement does not publish a result"), Read(TEXT("Result")), 0);
        }
        else
        {
            if (Case != 5) TestEqual(TEXT("Queue mutations do not run script cleanup"), Read(TEXT("Trace")), 0);
            CollectGarbage(RF_NoFlags);
            if (Case == 6) TestFalse(TEXT("Ready cancellation releases object before callback"), LoadedWeak.IsValid());
            else if (Success) TestTrue(TEXT("Queued successful object survives GC"), LoadedWeak.IsValid());
            if (Prepared)
            {
                Owner->DrainReady(Ready);
                TestEqual(TEXT("Prepared callback stays unpublished"), Ready.Num(), 0);
                TestTrue(TEXT("Complete prepared state validates"), Owner->ValidatePreparedCommit(Error));
                Owner->CommitPrepared();
            }
            int32 Resumes = 0;
            int32 Cancelled = 0;
            int32 CancelledObjects = 0;
            int32 CancelledTasks = 0;
            for (int32 Step = 0; Step < 12; ++Step)
            {
                World->Tick(LEVELTICK_All, 0.02f); ++GFrameCounter;
                Owner->DrainReady(Ready);
                for (const auto& Completion : Ready)
                {
                    if (Completion.Status == EAvidScriptContinuationStatus::Cancelled)
                    {
                        ++Cancelled;
                        int64 Cause = -1;
                        if (Completion.Token == ObservedHost.ObjectToken)
                        {
                            ++CancelledObjects;
                            TestTrue(TEXT("Object cancel cause is visible only in actual dispatch"), Endpoint.ReadCancellationCause(Completion.Token, Cause));
                            TestEqual(TEXT("Winning opaque source identity survives release"), Cause, Case == 9 ? 0LL : ObservedHost.SourceToken);
                            if (Replacement) TestNotEqual(TEXT("Reused source has a distinct identity"), Cause, Replacement);
                        }
                        else
                        {
                            ++CancelledTasks;
                            TestEqual(TEXT("Only a rethrown child produces a cancelled Task callback"), Case, 4);
                            TestFalse(TEXT("Task identity comes from its terminal snapshot rather than producer cause"), Endpoint.ReadCancellationCause(Completion.Token, Cause));
                        }
                        TestEqual(TEXT("Cancel provides no object slot"), Completion.ObjectSlot, 0);
                        TestEqual(TEXT("Cancel provides no object generation"), Completion.ObjectGeneration, 0);
                    }
                    const bool Dispatched = Runtime.DispatchContinuation(Completion, Result);
                    TestTrue(*Label, Dispatched);
                    if (!Dispatched) AddError(Result.ErrorCategory + TEXT(": ") + Result.ErrorMessage);
                    if (Completion.Token == ObservedHost.ObjectToken)
                    {
                        TestEqual(TEXT("Object handler performs only its own cleanup before parent resumes"), Read(TEXT("Trace")),
                            Cancel ? 3 : Case == 2 ? 2 : 23);
                        TestEqual(TEXT("Parent result still waits for its Task callback"), Read(TEXT("Result")), 0);
                    }
                    TestTrue(TEXT("Actual dispatched continuation finalizes once"), Owner->FinalizeDispatched(Completion.Token, Dispatched));
                    ++Resumes;
                    TestEqual(TEXT("Callback releases transient heap call frames"), Heap->GetStats().ActiveFrames, uint32(0));
                    TestEqual(TEXT("Intermediate managed state can collect"), Heap->Collect(), AvidScript::Managed::EHeapError::Ok);
                }
                if (Owner->GetActiveCount() == 0) break;
            }
            const auto Expected = Fixture.Cases[Case]->AsObject();
            for (const auto& Pair : TArray<TPair<const TCHAR*, const TCHAR*>>{
                {TEXT("Result"), TEXT("result")}, {TEXT("Trace"), TEXT("trace")}, {TEXT("InnerCatch"), TEXT("innerCatch")},
                {TEXT("OuterCatch"), TEXT("outerCatch")}, {TEXT("IdentityMatch"), TEXT("identityMatch")}, {TEXT("WrongCatch"), TEXT("wrongCatch")}})
                TestEqual(*Label, Read(Pair.Key), Expected->GetIntegerField(Pair.Value));
            TestEqual(TEXT("Only success consumes a borrowed handle"), Read(TEXT("LoadedSlot")) > 0 && Read(TEXT("LoadedGeneration")) > 0, Expected->GetBoolField(TEXT("hasHandle")));
            TestEqual(TEXT("Rethrown source Task is Cancelled"), Observer.CountAwaited(EAvidScriptTaskResultState::Cancelled), Case == 4 ? 1 : 0);
            TestEqual(TEXT("Object failure does not fault a Task"), Observer.CountAwaited(EAvidScriptTaskResultState::Faulted), 0);
            TestTrue(TEXT("Terminal Task identity survives GC"), Observer.bStableTerminalReads);
            TestEqual(TEXT("Only suspended object cancellation has a cancelled producer callback"), CancelledObjects, Cancel && Case != 5 ? 1 : 0);
            TestEqual(TEXT("Rethrow also queues its terminal Task cancellation"), CancelledTasks, Case == 4 ? 1 : 0);
            if (Replacement) TestTrue(TEXT("Reused source remains separately releasable"), Endpoint.ReleaseCancellationSource(Replacement));
            TestTrue(TEXT("Formal EndPlay executes"), Runtime.EndPlay(Result));
            AddInfo(FString::Printf(TEXT("object-cancellation %s result=%d trace=%d resumes=%d cancelled=%d"), *Label,
                Read(TEXT("Result")), Read(TEXT("Trace")), Resumes, Cancelled));
        }
        Owner->Teardown();
        Owner->Teardown();
        TestEqual(TEXT("Only successful results remain borrowed until Session ownership cleanup"),
            Ownership.GetBorrowedHandleCount(), Success ? 1 : 0);
        // The production Session retires continuations and separately cleans
        // its UObject ownership; a continuation owner does not own both.
        Ownership.Cleanup(Registry);
        Ownership.Cleanup(Registry);
        CheckEmpty(*this, *Owner, Ownership);
        TestEqual(TEXT("Released producer is not retained"), Loader->Requests.IsEmpty() ? false : Loader->Requests.Last()->Handle.IsValid(), false);
        TestEqual(TEXT("Only unfinished producer receives cancel"), Loader->Requests.IsEmpty() ? 0 : Loader->Requests.Last()->State->Count,
            Retire || (Cancel && Case != 5 && Case != 6 && Case != 7) ? 1 : 0);
        TestEqual(TEXT("Terminal heap collects"), Heap->Collect(), AvidScript::Managed::EHeapError::Ok);
        TestEqual(TEXT("Only domain static roots survive owner retirement"), Heap->GetStats().LiveRoots, Heap->GetStats().StaticRoots);
        TestEqual(TEXT("No active managed call frames"), Heap->GetStats().ActiveFrames, uint32(0));
        Runtime.Unload(); Runtime.Unload();
        TestNull(TEXT("Unload releases complete static domain and heap"), Runtime.GetManagedHeapForTesting());
        if (Retire) AddInfo(FString::Printf(TEXT("object-cancellation-retire %s"), *Label));
        if (HasAnyErrors()) return false;
        ++Passed;
    }
    TestEqual(TEXT("Both backends run complete execution and retirement matrix"), Passed, 48);
    AddInfo(TEXT("CompiledObjectLoadCancellation: 48/48 passed"));
    return true;
}

#endif
