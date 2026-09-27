#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptManagedHeapAbi.h"
#include "AvidScriptRuntimeBackendTestLanes.h"
#include "AvidScriptTaskResultAbi.h"
#include "AvidScriptWasmRuntime.h"
#include "Continuation/AvidScriptSessionContinuations.h"
#include "Continuation/AvidScriptExceptionCancellationIdentity.h"
#include "Memory/AvidScriptManagedHeap.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/AutomationTest.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/StrongObjectPtr.h"

#include <array>

namespace AvidScript::Tests::CancellationIdentity
{
constexpr const TCHAR* ModuleId = TEXT("task_cancellation_identity");
enum EAddress : int32
{
    SourceTask = 8, TargetTask = 16, FinalTask = 24, Proof = 32, Object = 40,
    SourceIdentity = 48, TargetIdentity = 56, RepeatedIdentity = 64, Frame = 72, Root = 80,
    Request = 256
};

// Hand-built native ABI fixtures; these are not C# compiler acceptance evidence.
void U32(TArray<uint8>& Out, uint32 Value)
{
    do { const uint8 Byte = Value & 0x7f; Value >>= 7; Out.Add(Byte | (Value ? 0x80 : 0)); } while (Value);
}
void I32(TArray<uint8>& Out, int32 Value)
{
    Out.Add(0x41);
    bool More;
    do
    {
        const uint8 Byte = Value & 0x7f;
        Value >>= 7;
        More = !((Value == 0 && !(Byte & 0x40)) || (Value == -1 && (Byte & 0x40)));
        Out.Add(Byte | (More ? 0x80 : 0));
    } while (More);
}
void Name(TArray<uint8>& Out, const ANSICHAR* Text)
{
    const int32 Size = FCStringAnsi::Strlen(Text);
    U32(Out, Size);
    Out.Append(reinterpret_cast<const uint8*>(Text), Size);
}
void Section(TArray<uint8>& Out, uint8 Id, const TArray<uint8>& Payload)
{
    Out.Add(Id); U32(Out, Payload.Num()); Out.Append(Payload);
}
void Custom(TArray<uint8>& Out, const ANSICHAR* Key, const FString& Text)
{
    TArray<uint8> Payload;
    Name(Payload, Key);
    const FTCHARToUTF8 Utf8(*Text);
    Payload.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
    Section(Out, 0, Payload);
}
void Load64(TArray<uint8>& Out, int32 Address) { I32(Out, Address); Out.Append({0x29, 3, 0}); }
void Store32(TArray<uint8>& Out, int32 Address, int32 Value)
{
    I32(Out, Address); I32(Out, Value); Out.Append({0x36, 2, 0});
}
void Copy64(TArray<uint8>& Out, int32 To, int32 From)
{
    I32(Out, To); Load64(Out, From); Out.Append({0x37, 3, 0});
}
void HeapCall(TArray<uint8>& Out, Managed::Abi::ECommand Command, int32 Bytes, int32 Output)
{
    Store32(Out, Request, Managed::Abi::Magic);
    Store32(Out, Request + 4, static_cast<int32>(Command));
    I32(Out, Request); I32(Out, Bytes); I32(Out, Output); I32(Out, 8);
    Out.Append({0x10, 0, 0x1a});
}
void ReadIdentity(TArray<uint8>& Out, int32 TaskAddress, int32 Output)
{
    I32(Out, Output); Load64(Out, TaskAddress); Out.Append({0x10, 2, 0x37, 3, 0});
}
void CancelAndPropagate(TArray<uint8>& Out)
{
    HeapCall(Out, Managed::Abi::ECommand::PushFrame, 8, Frame);
    Copy64(Out, Request + 8, Frame);
    HeapCall(Out, Managed::Abi::ECommand::CreateRoot, 24, Root);
    Store32(Out, Request + 8, 1);
    Copy64(Out, Request + 12, Root);
    HeapCall(Out, Managed::Abi::ECommand::Allocate, 20, Object);
    Load64(Out, SourceTask); I32(Out, 3); I32(Out, 1); Load64(Out, Object); Load64(Out, Proof);
    Out.Append({0x10, 1, 0x1a});
    Load64(Out, SourceTask); Load64(Out, TargetTask); Out.Append({0x10, 3, 0x1a});
    Load64(Out, TargetTask); Load64(Out, FinalTask); Out.Append({0x10, 3, 0x1a});
    ReadIdentity(Out, SourceTask, SourceIdentity);
    ReadIdentity(Out, FinalTask, TargetIdentity);
}
FString Catalog(int32 Schema)
{
    auto Document = MakeShared<FJsonObject>();
    Document->SetNumberField(TEXT("schema_version"), 1);
    Document->SetNumberField(TEXT("guest_ir_schema_version"), Schema);
    Document->SetStringField(TEXT("guest_ir_version"), FString::Printf(TEXT("1.%d"), Schema - 1));
    Document->SetStringField(TEXT("module_id"), ModuleId);
    Document->SetStringField(TEXT("source_sha256"), FString::ChrN(64, 'a'));
    TArray<TSharedPtr<FJsonValue>> Types;
    for (const TCHAR* Id : {TEXT("type:global::System.Exception"),
        TEXT("type:global::System.OperationCanceledException"), TEXT("type:global::System.Threading.Tasks.TaskCanceledException")})
    {
        auto Type = MakeShared<FJsonObject>();
        Type->SetNumberField(TEXT("token"), Types.Num() + 1);
        Type->SetStringField(TEXT("type_id"), Id);
        Types.Add(MakeShared<FJsonValueObject>(Type));
    }
    Document->SetArrayField(TEXT("types"), Types);
    auto Span = MakeShared<FJsonObject>();
    Span->SetNumberField(TEXT("token"), 1);
    Span->SetStringField(TEXT("source_id"), TEXT("Tests/CancellationIdentity.cs"));
    Span->SetNumberField(TEXT("source_length"), 32);
    Span->SetNumberField(TEXT("start"), 0); Span->SetNumberField(TEXT("length"), 12);
    Span->SetNumberField(TEXT("line"), 0); Span->SetNumberField(TEXT("column"), 0);
    Span->SetNumberField(TEXT("end_line"), 0); Span->SetNumberField(TEXT("end_column"), 12);
    Document->SetArrayField(TEXT("sources"), {MakeShared<FJsonValueObject>(Span)});
    FString Text;
    const auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    check(FJsonSerializer::Serialize(Document, Writer));
    return Text;
}
TArray<uint8> Module(bool bPending = true, int32 Schema = 32,
    const ANSICHAR* CancelImport = TaskResult::Abi::CancelLanguageErrorV2Import,
    const ANSICHAR* CancelModule = "avidscript")
{
    TArray<uint8> Bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
    Section(Bytes, 1, {7,
        0x60, 4, 0x7f, 0x7f, 0x7f, 0x7f, 1, 0x7f,
        0x60, 5, 0x7e, 0x7f, 0x7f, 0x7e, 0x7e, 1, 0x7f,
        0x60, 1, 0x7e, 1, 0x7e,
        0x60, 2, 0x7e, 0x7e, 1, 0x7f,
        0x60, 0, 0, 0x60, 1, 0x7d, 0, 0x60, 3, 0x7f, 0x7e, 0x7f, 0});
    TArray<uint8> Imports{4};
    uint8 Index = 0;
    for (const ANSICHAR* Import : {Managed::Abi::ImportName, CancelImport,
        TaskResult::Abi::CancellationTokenImport, TaskResult::Abi::PropagateFailureImport})
    {
        Name(Imports, Index == 1 ? CancelModule : "avidscript"); Name(Imports, Import); Imports.Append({0, Index++});
    }
    Section(Bytes, 2, Imports);
    Section(Bytes, 3, {4, 4, 5, 4, 6});
    Section(Bytes, 5, {1, 0, 1});
    TArray<uint8> Exports{5};
    Name(Exports, "memory"); Exports.Append({2, 0});
    Index = 4;
    for (const ANSICHAR* Export : {"avid_on_begin_play", "avid_on_tick", "avid_on_end_play", "avid_on_continuation"})
    {
        Name(Exports, Export); Exports.Append({0, Index++});
    }
    Section(Bytes, 7, Exports);
    TArray<uint8> Begin{0}, Read{0}, Resume{0};
    if (!bPending) CancelAndPropagate(Begin);
    Begin.Add(0x0b);
    ReadIdentity(Read, FinalTask, RepeatedIdentity); Read.Add(0x0b);
    Resume.Append({0x20, 0}); I32(Resume, 41); Resume.Append({0x46, 0x04, 0x40});
    CancelAndPropagate(Resume);
    Resume.Add(0x05); ReadIdentity(Resume, FinalTask, RepeatedIdentity); Resume.Append({0x0b, 0x0b});
    TArray<uint8> Code{4};
    for (const TArray<uint8>& Body : {Begin, Read, TArray<uint8>{0, 0x0b}, Resume})
    {
        U32(Code, Body.Num()); Code.Append(Body);
    }
    Section(Bytes, 10, Code);
    FString Provenance = FString::Printf(TEXT("module_id=%s\nsource_id=Tests/CancellationIdentity.cs\nsource_sha256=%s\n")
        TEXT("frontend_sha256=%s\nsemantic_sha256=%s\nguest_ir=%d/1.%d"),
        ModuleId, *FString::ChrN(64, 'a'), *FString::ChrN(64, 'b'), *FString::ChrN(64, 'c'), Schema, Schema - 1);
    if (Schema == 31 || Schema == 32) Provenance += TEXT("\nguest_ir_base=24/1.23");
    Custom(Bytes, "avidscript.provenance", Provenance);
    Custom(Bytes, "avidscript.language_errors", Catalog(Schema));
    return Bytes;
}
void Put64(TArray<uint8>& Bytes, int32 Offset, int64 Value)
{
    for (int32 Index = 0; Index != 8; ++Index) Bytes[Offset + Index] = static_cast<uint64>(Value) >> (8 * Index);
}
uint64 Get64(const TArray<uint8>& Bytes, int32 Offset)
{
    uint64 Value = 0;
    for (int32 Index = 0; Index != 8; ++Index) Value |= uint64(Bytes[Offset + Index]) << (8 * Index);
    return Value;
}
bool Configure(FAutomationTestBase& Test, FAvidScriptWasmRuntimeInstance& Runtime, TConstArrayView<uint8> Bytes,
    const FAvidScriptRuntimeBackendTestLane& Lane)
{
    FAvidScriptWasmSmokeResult Result;
    if (!Test.TestTrue(TEXT("Identity ABI fixture loads"), Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), ModuleId, Result)))
    {
        Test.AddError(Result.ErrorMessage); return false;
    }
    if (!TestAvidScriptRuntimeLaneIdentity(Test, Lane, Result)) return false;
    const std::array<Managed::FHeapLayout, 1> Layouts{{{1, 8, {}}}};
    return Test.TestTrue(TEXT("Identity error layout configures"), Runtime.GetManagedHeapForTesting()
        && Runtime.GetManagedHeapForTesting()->Configure(Layouts) == Managed::EHeapError::Ok);
}
void Bind(FAvidScriptWasmRuntimeInstance& Runtime, UWorld* World, FAvidScriptContinuationHostEndpoint& Endpoint)
{
    FAvidScriptWasmHostContext Context;
    Context.World = World; Context.Tasks = &Endpoint; Context.Continuations = &Endpoint;
    Runtime.SetHostContext(Context);
}
bool Complete(FAvidScriptWasmRuntimeInstance& Runtime, int64 Task, uint64 ObjectToken, int64 SourceProof,
    FAvidScriptHostCallResult& Result, int32 Type = 3, int32 Span = 1,
    EAvidScriptHostBindingId Binding = EAvidScriptHostBindingId::TaskCancelLanguageErrorV2)
{
    FAvidScriptHostCall Call;
    Call.BindingId = Binding; Call.Int64Args[0] = Task; Call.Int64Args[1] = static_cast<int64>(ObjectToken);
    Call.Int64Args[2] = SourceProof; Call.IntArgs[0] = Type; Call.IntArgs[1] = Span;
    return Runtime.DispatchHostCall(Call, Result);
}
bool Read(FAvidScriptWasmRuntimeInstance& Runtime, int64 Task, FAvidScriptHostCallResult& Result)
{
    FAvidScriptHostCall Call;
    Call.BindingId = EAvidScriptHostBindingId::TaskCancellationTokenV1; Call.Int64Args[0] = Task;
    return Runtime.DispatchHostCall(Call, Result);
}
uint64 Allocate(FAutomationTestBase& Test, FAvidScriptWasmRuntimeInstance& Runtime)
{
    auto* Heap = Runtime.GetManagedHeapForTesting();
    Managed::FToken FrameToken = 0, RootToken = 0, ObjectToken = 0;
    if (!Test.TestTrue(TEXT("Error frame opens"), Heap->PushFrame(FrameToken) == Managed::EHeapError::Ok)
        || !Test.TestTrue(TEXT("Error root creates"), Heap->CreateRoot(FrameToken, 0, RootToken) == Managed::EHeapError::Ok)
        || !Test.TestTrue(TEXT("Error object allocates"), Heap->Allocate(1, RootToken, ObjectToken) == Managed::EHeapError::Ok)) return 0;
    return ObjectToken;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTaskCancellationIdentityAbiTest,
    "AvidScript.Runtime.Continuation.TaskCancellationIdentityAbi",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskCancellationIdentityAbiTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::CancellationIdentity;
    using namespace AvidScript::Managed;
    const auto Lanes = GetAvidScriptRuntimeBackendTestLanes();
    if (!TestEqual(TEXT("Both VM backends are required"), Lanes.Num(), 2)) return false;
    for (const auto& Lane : Lanes)
    for (int32 Mode = 0; Mode != 3; ++Mode)
    {
        TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
        FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
        if (!Configure(*this, Runtime, Module(Mode != 0), Lane)) return false;
        auto* Heap = Runtime.GetManagedHeapForTesting();
        const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
        auto& Active = Owner->ResetActive(World.Get());
        Bind(Runtime, World.Get(), Active);
        const int64 Source = Active.CreateCancellationSource();
        const int64 Tasks[] = {Active.CreateTaskResult(TEXT("type:int32")), Active.CreateTaskResult(TEXT("type:int32")),
            Active.CreateTaskResult(TEXT("type:int32"))};
        int64 Waiter = 0;
        if (!TestTrue(TEXT("Target waiter registers before cancellation"),
            Active.AwaitTaskResult(Tasks[2], 42, Waiter) == EAvidScriptTaskWaitRegistration::Queued)) return false;
        TArray<uint8> Memory;
        Memory.SetNumZeroed(96);
        Put64(Memory, SourceTask, Tasks[0]); Put64(Memory, TargetTask, Tasks[1]); Put64(Memory, FinalTask, Tasks[2]);
        Put64(Memory, Proof, Mode == 0 ? Source : 0);
        FString Error;
        if (!TestTrue(TEXT("Task and evaluated proof are stored once"), Runtime.WriteStateBytes(0, Memory, Error))) return false;
        if (Mode == 0) TestTrue(TEXT("Immediate source is already cancelled"), Active.CancelCancellationSource(Source));
        FAvidScriptWasmSmokeResult Result;
        if (!TestTrue(TEXT("Identity fixture begins"), Runtime.BeginPlay(Result))) { AddError(Result.ErrorMessage); return false; }
        if (Mode != 0)
        {
            const int64 Producer = Active.ScheduleDelayWithCancelResume(60.0f, 41);
            if (!TestTrue(TEXT("Pending await binds its source"), Producer > 0 && Active.BindCancellationSource(Source, Producer))) return false;
            TestTrue(TEXT("Cancellation chooses source or manual cause"), Mode == 1 ? Active.CancelCancellationSource(Source) : Active.Cancel(Producer));
            TestTrue(TEXT("Source can release before dispatch"), Active.ReleaseCancellationSource(Source));
            const int64 Replacement = Active.CreateCancellationSource();
            TestTrue(TEXT("Reused source slot has distinct identity"), Replacement != Source);
            TArray<FAvidScriptContinuationCompletion> Ready;
            Owner->DrainReady(Ready);
            if (!TestEqual(TEXT("One direct cancellation dispatches first"), Ready.Num(), 1)) return false;
            if (!TestTrue(TEXT("Actual WASM receives saved cause"), Runtime.DispatchContinuation(Ready[0], Result)))
            { AddError(Result.ErrorMessage); return false; }
            TestTrue(TEXT("Direct producer finalizes"), Owner->FinalizeDispatched(Producer, true));
            TestTrue(TEXT("Identity did not cancel reused source"), Active.GetCancellationSourceStatus(Replacement) == EAvidScriptCancellationSourceStatus::Open);
            TestTrue(TEXT("Replacement source releases"), Active.ReleaseCancellationSource(Replacement));
        }
        else TestTrue(TEXT("Immediate source releases after proof admission"), Active.ReleaseCancellationSource(Source));
        if (!TestTrue(TEXT("ABI observations are readable"), Runtime.ReadStateBytes(0, Memory, Error))) return false;
        const uint64 Expected = Mode == 2 ? 0 : static_cast<uint64>(Source);
        TestEqual(TEXT("Source Task keeps complete identity"), Get64(Memory, SourceIdentity), Expected);
        TestEqual(TEXT("Two propagation hops retain the same identity"), Get64(Memory, TargetIdentity), Expected);
        TestTrue(TEXT("Source identity has high bits"), static_cast<uint64>(Source) > MAX_uint32);
        const uint64 ErrorObject = Get64(Memory, Object);
        const auto* ObjectIdentity = AvidScript::Continuation::FExceptionCancellationIdentity::Find(*Heap, ErrorObject);
        if (!TestTrue(TEXT("Actual WASM cancellation publishes immutable object identity"), ObjectIdentity
            && ObjectIdentity->ExceptionType == 3 && static_cast<uint64>(ObjectIdentity->Source) == Expected)) return false;
        TestEqual(TEXT("Propagation shares one object record"), Heap->GetStats().NativeDataObjects, 1u);
        FAvidScriptTaskResultSnapshot Snapshot;
        for (const int64 Task : Tasks)
        {
            TestTrue(TEXT("Task remains Cancelled with unchanged span and object"), Active.ReadTaskResult(Task, Snapshot)
                && Snapshot.State == EAvidScriptTaskResultState::Cancelled && Snapshot.LanguageError.IsSet()
                && Snapshot.LanguageError->TypeToken == 3 && Snapshot.LanguageError->SourceToken == 1
                && Snapshot.LanguageError->ObjectToken == ErrorObject && Snapshot.LanguageError->CancellationSourceToken.IsSet()
                && static_cast<uint64>(Snapshot.LanguageError->CancellationSourceToken.GetValue()) == Expected);
        }
        TestTrue(TEXT("Intermediate Task owners release"), Active.ReleaseTaskResult(Tasks[0]) && Active.ReleaseTaskResult(Tasks[1]));
        TestTrue(TEXT("One shared persistent root survives GC"), Heap->Collect() == EHeapError::Ok
            && Heap->IsAlive(ErrorObject) && Heap->GetStats().LiveRoots == 1 && Heap->GetStats().ActiveFrames == 0);
        for (int32 Repeat = 0; Repeat != 2; ++Repeat)
        {
            if (!TestTrue(TEXT("Later VM invocation reads identity"), Runtime.Tick(0.01f, Result))) { AddError(Result.ErrorMessage); return false; }
            TestTrue(TEXT("Repeated result is readable"), Runtime.ReadStateBytes(0, Memory, Error));
            TestEqual(TEXT("Repeated await sees identical token"), Get64(Memory, RepeatedIdentity), Expected);
            TestEqual(TEXT("Identity read adds no roots"), Heap->GetStats().LiveRoots, 1u);
            int64 ReadyToken = 123;
            TestTrue(TEXT("Completed Task awaits immediately"), Active.AwaitTaskResult(Tasks[2], 43, ReadyToken) == EAvidScriptTaskWaitRegistration::Ready);
            TestEqual(TEXT("Ready await schedules no new callback"), ReadyToken, 0LL);
        }
        // Native alias models an exception reference that escaped its catch. The
        // separate compiled catch suite tests actual C# aliases across await/GC.
        FPersistentRoots EscapedException;
        TestTrue(TEXT("Escaped exception acquires independent ownership"), Heap->RetainPersistent(
            {&ErrorObject, 1}, EscapedException) == EHeapError::Ok);
        TestTrue(TEXT("Last external owner releases"), Active.ReleaseTaskResult(Tasks[2]));
        TArray<FAvidScriptContinuationCompletion> Ready;
        Owner->DrainReady(Ready);
        if (!TestEqual(TEXT("Exactly one waiting Task callback remains"), Ready.Num(), 1)) return false;
        TestEqual(TEXT("Expected waiter resumes"), Ready[0].Token, Waiter);
        TestTrue(TEXT("Waiter reads identity using its own retained Task"), Runtime.DispatchContinuation(Ready[0], Result));
        TestTrue(TEXT("Waiter finalizes"), Owner->FinalizeDispatched(Waiter, true));
        TestTrue(TEXT("Escaped exception survives all Task and source owners"), Heap->Collect() == EHeapError::Ok
            && Heap->IsAlive(ErrorObject) && Owner->GetTaskResultsForTesting().GetCount() == 0
            && Owner->GetCancellationSourceCountForTesting() == 0);
        const auto* EscapedIdentity = AvidScript::Continuation::FExceptionCancellationIdentity::Find(*Heap, ErrorObject);
        TestTrue(TEXT("Object keeps complete identity after Task release and GC"), EscapedIdentity
            && static_cast<uint64>(EscapedIdentity->Source) == Expected && EscapedIdentity->ExceptionType == 3);
        EscapedException.Reset();
        TestTrue(TEXT("Final alias release collects error object"), Heap->Collect() == EHeapError::Ok && !Heap->IsAlive(ErrorObject));
        TestEqual(TEXT("No native object data remains"), Heap->GetStats().NativeDataBytes, uint64(0));
        TestEqual(TEXT("No root remains"), Heap->GetStats().LiveRoots, 0u);
        TestEqual(TEXT("No Task remains"), Owner->GetTaskResultsForTesting().GetCount(), 0);
        TestEqual(TEXT("No source remains"), Owner->GetCancellationSourceCountForTesting(), 0);
        TestEqual(TEXT("No continuation remains"), Owner->GetActiveCount(), 0);
        Owner->Teardown(); Runtime.Unload();
        AddInfo(AvidScriptRuntimeLaneLabel(Lane, *FString::Printf(TEXT("cancellation identity mode=%d"), Mode)));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTaskCancellationIdentityAdmissionTest,
    "AvidScript.Runtime.Continuation.TaskCancellationIdentityAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskCancellationIdentityAdmissionTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::CancellationIdentity;
    using namespace AvidScript::Managed;
    const auto Lanes = GetAvidScriptRuntimeBackendTestLanes();
    if (!TestEqual(TEXT("Both VM backends are required"), Lanes.Num(), 2)) return false;
    for (const auto& Lane : Lanes)
    {
        TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
        FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
        if (!Configure(*this, Runtime, Module(), Lane)) return false;
        auto* Heap = Runtime.GetManagedHeapForTesting();
        const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
        const auto ForeignOwner = MakeShared<FAvidScriptSessionContinuations>();
        auto& Active = Owner->ResetActive(World.Get());
        auto& Foreign = ForeignOwner->ResetActive(World.Get());
        Bind(Runtime, World.Get(), Active);
        const int64 Task = Active.CreateTaskResult(TEXT("type:int32"));
        const int64 Source = Active.CreateCancellationSource();
        const int64 OtherSource = Foreign.CreateCancellationSource();
        TestTrue(TEXT("Foreign source cancels"), Foreign.CancelCancellationSource(OtherSource));
        FAvidScriptHostCallResult Result;
        TestFalse(TEXT("Admission requires a VM invocation"), Complete(Runtime, Task, 1, Source, Result));
        TestEqual(TEXT("Invocation failure has context category"), Result.ErrorCategory, FString(TEXT("task_result_context")));
        const uint64 OldObject = Allocate(*this, Runtime);
        const uint64 Invocation = Runtime.BeginVmInvocation();
        const uint64 ErrorObject = Allocate(*this, Runtime);
        if (!ErrorObject) return false;
        const uint32 InitialRoots = Heap->GetStats().LiveRoots;
        for (const int64 InvalidProof : {int64(0), int64(1), Task, OtherSource, Source,
            static_cast<int64>(0x80000000ffffffffULL), static_cast<int64>(Source ^ (1ULL << 32))})
        {
            TestFalse(TEXT("No dispatch, open, foreign, forged or wrong-kind proof is rejected"),
                Complete(Runtime, Task, ErrorObject, InvalidProof, Result));
            TestEqual(TEXT("Bad proof has stable category"), Result.ErrorCategory, FString(TEXT("task_cancellation_source")));
            TestFalse(TEXT("Rejected proof does not report success"), Result.bSucceeded);
        }
        TestTrue(TEXT("Valid source cancels"), Active.CancelCancellationSource(Source));
        const int64 Stale = Active.CreateCancellationSource();
        TestTrue(TEXT("Stale source cancels and releases"), Active.CancelCancellationSource(Stale) && Active.ReleaseCancellationSource(Stale));
        TestFalse(TEXT("Released source cannot prove immediate cancellation"), Complete(Runtime, Task, ErrorObject, Stale, Result));
        const int64 ForeignTask = Foreign.CreateTaskResult(TEXT("type:int32"));
        const int64 WrongType = Active.CreateTaskResult(TEXT("type:float32"));
        for (const int64 InvalidTask : {int64(1), Source, ForeignTask, WrongType})
            TestFalse(TEXT("Task identity is still validated"), Complete(Runtime, InvalidTask, ErrorObject, Source, Result));
        TestFalse(TEXT("Non-cancellation type rejected"), Complete(Runtime, Task, ErrorObject, Source, Result, 1));
        TestFalse(TEXT("Unknown type rejected"), Complete(Runtime, Task, ErrorObject, Source, Result, 4));
        TestFalse(TEXT("Source span remains a separate catalog token"), Complete(Runtime, Task, ErrorObject, Source, Result, 3, 2));
        TestFalse(TEXT("Unrooted object rejected"), Complete(Runtime, Task, 0, Source, Result));
        TestFalse(TEXT("Root from preceding invocation rejected"), Complete(Runtime, Task, OldObject, Source, Result));
        TestEqual(TEXT("Rejected admissions acquire no persistent roots"), Heap->GetStats().LiveRoots, InitialRoots);
        FAvidScriptTaskResultSnapshot Snapshot;
        TestFalse(TEXT("All rejected writes leave Task pending"), Active.ReadTaskResult(Task, Snapshot));
        const int64 NativeRejectedTask = Active.CreateTaskResult(TEXT("type:int32"));
        class FNativeLease final : public IAvidScriptTaskLanguageErrorLease {};
        TArray<int64> NativeWaiters;
        TestFalse(TEXT("Native fault cannot carry cancellation identity"), Active.FaultTaskResultLanguageError(
            NativeRejectedTask, {3, 1, ErrorObject, Source}, MakeShared<FNativeLease>(), NativeWaiters));
        TestFalse(TEXT("Native cancellation rejects a positive source token"), Active.CancelTaskResultLanguageError(
            NativeRejectedTask, {3, 1, ErrorObject, int64(1)}, MakeShared<FNativeLease>(), NativeWaiters));
        TestFalse(TEXT("Rejected native records leave Task pending"), Active.ReadTaskResult(NativeRejectedTask, Snapshot));
        TestEqual(TEXT("Rejected native records wake no waiter"), NativeWaiters.Num(), 0);
        TestTrue(TEXT("Valid immediate proof is admitted"), Complete(Runtime, Task, ErrorObject, Source, Result));
        using AvidScript::Continuation::FExceptionCancellationIdentity;
        const auto* Identity = FExceptionCancellationIdentity::Find(*Heap, ErrorObject);
        if (!TestTrue(TEXT("Verified cause belongs to the exception"), Identity && Identity->Source == Source
            && Identity->ExceptionType == 3)) return false;
        const uint32 CompletedRoots = Heap->GetStats().LiveRoots;
        TestFalse(TEXT("Duplicate completion cannot replace cause"), Complete(Runtime, Task, ErrorObject, Source, Result));
        TestEqual(TEXT("Failed duplicate releases attempted root lease"), Heap->GetStats().LiveRoots, CompletedRoots);

        const uint64 RejectedObject = Allocate(*this, Runtime);
        // Allocate opens a new Guest frame. Capture the original exception into
        // that frame through the same Host operation used by compiled catch bindings.
        FAvidScriptHostCall Capture;
        Capture.BindingId = EAvidScriptHostBindingId::TaskTerminalErrorRootV1;
        Capture.Int64Args[0] = Task;
        if (!TestTrue(TEXT("Existing exception is captured into the new current frame"), Runtime.DispatchHostCall(Capture, Result)
            && static_cast<uint64>(Result.ReturnValueI64) == ErrorObject)) return false;
        const auto BeforeRejected = Heap->GetStats();
        TestFalse(TEXT("Completed Task cannot publish a different exception"), Complete(Runtime, Task, RejectedObject, Source, Result));
        TestTrue(TEXT("Failed completion rolls back unpublished object identity"), !FExceptionCancellationIdentity::Find(*Heap, RejectedObject)
            && Heap->GetStats().NativeDataBytes == BeforeRejected.NativeDataBytes
            && Heap->GetStats().LiveBytes == BeforeRejected.LiveBytes && Heap->GetStats().LiveRoots == BeforeRejected.LiveRoots);
        const int64 ReusedTask = Active.CreateTaskResult(TEXT("type:int32"));
        const int64 DifferentSource = Active.CreateCancellationSource();
        TestTrue(TEXT("Independent source cancels"), Active.CancelCancellationSource(DifferentSource));
        TestFalse(TEXT("Same exception cannot be relabeled with another source"), Complete(Runtime, ReusedTask, ErrorObject, DifferentSource, Result));
        TestEqual(TEXT("Conflicting source reports identity failure"), Result.ErrorCategory, FString(TEXT("task_cancellation_identity")));
        TestFalse(TEXT("Same exception cannot change concrete type"), Complete(Runtime, ReusedTask, ErrorObject, Source, Result, 2));
        TestFalse(TEXT("Conflicting object identity leaves target pending"), Active.ReadTaskResult(ReusedTask, Snapshot));
        TestTrue(TEXT("Same object and same identity may belong to another Task"), Complete(Runtime, ReusedTask, ErrorObject, Source, Result));
        TestEqual(TEXT("Repeated admission does not allocate another record"), Heap->GetStats().NativeDataObjects, 1u);
        TestTrue(TEXT("Independent owner and source release"), Active.ReleaseTaskResult(ReusedTask) && Active.ReleaseCancellationSource(DifferentSource));
        std::array<uint8, 8> ForgedBytes; ForgedBytes.fill(0xff);
        TestTrue(TEXT("Guest-visible bytes remain writable without changing identity"), Heap->WriteBytes(ErrorObject, 1, 0, ForgedBytes) == EHeapError::Ok
            && FExceptionCancellationIdentity::Find(*Heap, ErrorObject)->Source == Source);
        TestTrue(TEXT("Source release does not erase identity"), Active.ReleaseCancellationSource(Source));
        TestTrue(TEXT("Identity reads after source release"), Read(Runtime, Task, Result));
        TestEqual(TEXT("Read is full i64"), Result.ReturnValueI64, Source);
        TestEqual(TEXT("Read has no root side effect"), Heap->GetStats().LiveRoots, BeforeRejected.LiveRoots);
        for (const auto Binding : {EAvidScriptHostBindingId::TaskCancelLanguageErrorV1, EAvidScriptHostBindingId::TaskFaultLanguageErrorV1})
        {
            const int64 Legacy = Active.CreateTaskResult(TEXT("type:int32"));
            const int64 Copy = Active.CreateTaskResult(TEXT("type:int32"));
            TestTrue(TEXT("Legacy terminal writes remain available"), Complete(Runtime, Legacy, ErrorObject, 0, Result, 3, 1, Binding));
            TArray<int64> Waiters;
            TestTrue(TEXT("Legacy terminal error propagates"), Active.PropagateTaskFailure(Legacy, Copy, Waiters));
            TestFalse(TEXT("Unknown and faulted identities never become None"), Read(Runtime, Copy, Result));
            TestEqual(TEXT("Missing identity has stable category"), Result.ErrorCategory, FString(TEXT("task_cancellation_identity")));
            TestFalse(TEXT("Unknown identity read is not successful zero"), Result.bSucceeded);
            TestTrue(TEXT("Legacy metadata stays unset"), Active.ReadTaskResult(Copy, Snapshot)
                && Snapshot.LanguageError.IsSet() && !Snapshot.LanguageError->CancellationSourceToken.IsSet());
            TestTrue(TEXT("Legacy Task metadata does not overwrite existing object identity"),
                FExceptionCancellationIdentity::Find(*Heap, ErrorObject)->Source == Source);
            const int64 FreshLegacy = Active.CreateTaskResult(TEXT("type:int32"));
            TestTrue(TEXT("Fresh legacy object remains supported"), Complete(Runtime, FreshLegacy, RejectedObject, 0, Result, 3, 1, Binding));
            TestFalse(TEXT("Fresh legacy object does not invent cancellation identity"), FExceptionCancellationIdentity::Find(*Heap, RejectedObject) != nullptr);
            TestTrue(TEXT("Fresh legacy owner releases"), Active.ReleaseTaskResult(FreshLegacy));
            TestTrue(TEXT("Legacy owners release"), Active.ReleaseTaskResult(Legacy) && Active.ReleaseTaskResult(Copy));
        }
        const int64 Bare = Active.CreateTaskResult(TEXT("type:int32"));
        TArray<int64> Waiters;
        TestTrue(TEXT("Bare cancellation remains supported"), Active.CancelTaskResult(Bare, Waiters));
        TestFalse(TEXT("Bare cancellation cannot invent identity"), Read(Runtime, Bare, Result));
        TestFalse(TEXT("Foreign Task read rejected"), Read(Runtime, ForeignTask, Result));
        TestFalse(TEXT("Running Task read rejected"), Read(Runtime, WrongType, Result));
        const uint32 RootsBeforeThread = Heap->GetStats().LiveRoots;
        const bool bOffThreadRejected = Async(EAsyncExecution::ThreadPool, [&Runtime, Task, ErrorObject]()
        {
            FAvidScriptHostCallResult Local;
            return !Complete(Runtime, Task, ErrorObject, 0, Local) && Local.ErrorCategory == TEXT("task_result_context")
                && !Read(Runtime, Task, Local) && Local.ErrorCategory == TEXT("task_result_context");
        }).Get();
        TestTrue(TEXT("Background thread cannot access Task identity"), bOffThreadRejected);
        TestEqual(TEXT("Background rejection does not touch roots"), Heap->GetStats().LiveRoots, RootsBeforeThread);
        Runtime.EndVmInvocation(Invocation);
        Heap->UnwindToDepth(0);
        TestFalse(TEXT("Identity read outside invocation rejected"), Read(Runtime, Task, Result));
        Owner->Teardown(); ForeignOwner->Teardown();
        TestTrue(TEXT("Teardown drops all persistent roots"), Heap->Collect() == EHeapError::Ok
            && Heap->GetStats().LiveRoots == 0 && Heap->GetStats().ActiveFrames == 0 && !Heap->IsAlive(ErrorObject));
        TestEqual(TEXT("Rejected and admitted records leave no native bytes"), Heap->GetStats().NativeDataBytes, uint64(0));
        Runtime.Unload();
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTaskCancellationIdentityVersionTest,
    "AvidScript.Runtime.Continuation.TaskCancellationIdentityVersion",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskCancellationIdentityVersionTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::CancellationIdentity;
    const auto Lanes = GetAvidScriptRuntimeBackendTestLanes();
    if (!TestEqual(TEXT("Both VM backends are required"), Lanes.Num(), 2)) return false;
    for (const auto& Lane : Lanes)
    for (const int32 Schema : {24, 31})
    {
        FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
        if (!Configure(*this, Runtime, Module(true, Schema), Lane)) return false;
        FAvidScriptHostCallResult Result;
        TestFalse(TEXT("Old module cannot call v2 admission"), Complete(Runtime, 1, 1, 0, Result));
        TestEqual(TEXT("Admission fails at version gate"), Result.ErrorCategory, FString(TEXT("task_language_error_version")));
        TestFalse(TEXT("Old module cannot read new identity"), Read(Runtime, 1, Result));
        TestEqual(TEXT("Read fails at version gate"), Result.ErrorCategory, FString(TEXT("task_language_error_version")));
        FAvidScriptWasmSmokeResult Smoke;
        TestTrue(TEXT("Old module may begin without using new import"), Runtime.BeginPlay(Smoke));
        TestFalse(TEXT("Actual WASM identity call traps for old profile"), Runtime.Tick(0.01f, Smoke));
        TestTrue(TEXT("WASM trap retains version diagnostic"), Smoke.ErrorMessage.Contains(TEXT("task_language_error_version")));
        Runtime.Unload();
    }
    for (const auto& Lane : Lanes)
    for (int32 Invalid = 0; Invalid != 3; ++Invalid)
    {
        const TArray<uint8> Bytes = Module(true, 32,
            Invalid == 0 ? "avid_task_cancel_language_error_v3"
                : Invalid == 1 ? AvidScript::TaskResult::Abi::CancelLanguageErrorImport
                : AvidScript::TaskResult::Abi::CancelLanguageErrorV2Import,
            Invalid == 2 ? "env" : "avidscript");
        FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
        FAvidScriptWasmSmokeResult Result;
        TestFalse(TEXT("Future import, wrong signature and env alias are rejected"),
            Runtime.LoadModule(Bytes.GetData(), Bytes.Num(), ModuleId, Result));
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTaskCancellationIdentityLifecycleTest,
    "AvidScript.Runtime.Continuation.TaskCancellationIdentityLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskCancellationIdentityLifecycleTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::Tests::CancellationIdentity;
    using namespace AvidScript::Managed;
    const auto Lanes = GetAvidScriptRuntimeBackendTestLanes();
    if (!TestEqual(TEXT("Both VM backends are required"), Lanes.Num(), 2) || !GEngine) return false;
    for (const auto& Lane : Lanes)
    {
        struct FWorldScope
        {
            UWorld* World = nullptr;
            FWorldScope()
            {
                World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("CancellationIdentityWorld"));
                if (World)
                {
                    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
                    World->InitializeActorsForPlay(FURL());
                }
            }
            void Destroy()
            {
                if (!World) return;
                GEngine->DestroyWorldContext(World);
                World->DestroyWorld(false);
                World = nullptr;
            }
            ~FWorldScope() { Destroy(); }
        } Scope;
        if (!TestNotNull(TEXT("Lifecycle world exists"), Scope.World)) return false;
        FAvidScriptWasmRuntimeInstance Runtime(Lane.Selection);
        if (!Configure(*this, Runtime, Module(), Lane)) return false;
        auto* Heap = Runtime.GetManagedHeapForTesting();
        const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
        auto& Active = Owner->ResetActive(Scope.World);
        FAvidScriptContinuationHostEndpoint OldAlias(Owner, EAvidScriptContinuationLane::Active, Active.GetActivationSerial());
        FAvidScriptHostCallResult Result;
        auto CreateCancelled = [&](FAvidScriptContinuationHostEndpoint& Endpoint, int64& OutSource)
        {
            Bind(Runtime, Scope.World, Endpoint);
            const int64 Task = Endpoint.CreateTaskResult(TEXT("type:int32"));
            OutSource = Endpoint.CreateCancellationSource();
            TestTrue(TEXT("Lifecycle source cancels"), Endpoint.CancelCancellationSource(OutSource));
            const uint64 Invocation = Runtime.BeginVmInvocation();
            const uint64 ObjectToken = Allocate(*this, Runtime);
            TestTrue(TEXT("Lifecycle error admits"), Complete(Runtime, Task, ObjectToken, OutSource, Result));
            Runtime.EndVmInvocation(Invocation);
            TestTrue(TEXT("Recorded identity owns no source reference"), Endpoint.ReleaseCancellationSource(OutSource));
            return Task;
        };
        int64 ActiveSource = 0, FailedSource = 0, PublishedSource = 0;
        const int64 ActiveTask = CreateCancelled(Active, ActiveSource);
        auto& Failed = Owner->BeginPrepared(Scope.World);
        const int64 FailedTask = CreateCancelled(Failed, FailedSource);
        uint64 Invocation = Runtime.BeginVmInvocation();
        TestFalse(TEXT("Candidate cannot read active Task identity"), Read(Runtime, ActiveTask, Result));
        Runtime.EndVmInvocation(Invocation);
        Bind(Runtime, Scope.World, Active);
        Owner->DiscardPrepared();
        Invocation = Runtime.BeginVmInvocation();
        TestTrue(TEXT("Rollback preserves active identity"), Read(Runtime, ActiveTask, Result) && Result.ReturnValueI64 == ActiveSource);
        TestFalse(TEXT("Rolled back identity becomes inaccessible"), Read(Runtime, FailedTask, Result));
        Runtime.EndVmInvocation(Invocation);
        TestTrue(TEXT("Rollback releases only candidate root"), Heap->Collect() == EHeapError::Ok && Heap->GetStats().LiveRoots == 1);
        TestEqual(TEXT("Rollback reclaims candidate object data"), Heap->GetStats().NativeDataObjects, 1u);
        auto& Published = Owner->BeginPrepared(Scope.World);
        FAvidScriptContinuationHostEndpoint PreparedAlias(Owner, EAvidScriptContinuationLane::Prepared, Published.GetActivationSerial());
        const int64 PublishedTask = CreateCancelled(Published, PublishedSource);
        FString Error;
        if (!TestTrue(TEXT("Candidate validates for publication"), Owner->ValidatePreparedCommit(Error))) return false;
        Owner->CommitPrepared();
        Bind(Runtime, Scope.World, Published);
        Invocation = Runtime.BeginVmInvocation();
        TestTrue(TEXT("Promotion preserves candidate identity"), Read(Runtime, PublishedTask, Result) && Result.ReturnValueI64 == PublishedSource);
        TestFalse(TEXT("Retired active Task cannot be read"), Read(Runtime, ActiveTask, Result));
        FAvidScriptTaskResultSnapshot Snapshot;
        TestFalse(TEXT("Old active alias cannot read promoted Task"), OldAlias.ReadTaskResult(PublishedTask, Snapshot));
        TestFalse(TEXT("Old prepared alias cannot read promoted Task"), PreparedAlias.ReadTaskResult(PublishedTask, Snapshot));
        TestTrue(TEXT("Publication drops old root"), Heap->Collect() == EHeapError::Ok && Heap->GetStats().LiveRoots == 1);
        TestEqual(TEXT("Publication keeps only promoted object data"), Heap->GetStats().NativeDataObjects, 1u);
        Scope.Destroy();
        TestFalse(TEXT("Destroyed World rejects identity before object GC"), Read(Runtime, PublishedTask, Result));
        Runtime.EndVmInvocation(Invocation);
        Owner->Teardown(); Owner->Teardown();
        TestTrue(TEXT("Teardown releases all error leases"), Heap->Collect() == EHeapError::Ok && Heap->GetStats().LiveRoots == 0);
        TestEqual(TEXT("World teardown releases object metadata"), Heap->GetStats().NativeDataBytes, uint64(0));
        TestEqual(TEXT("Teardown releases every Task"), Owner->GetTaskResultsForTesting().GetCount(), 0);
        TestEqual(TEXT("Teardown releases every source"), Owner->GetCancellationSourceCountForTesting(), 0);
        Runtime.Unload();
    }
    return true;
}

#endif
