#if WITH_DEV_AUTOMATION_TESTS

#include "Continuation/AvidScriptSessionContinuations.h"
#include "Continuation/AvidScriptTaskValueCapture.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include <array>

namespace AvidScriptTaskValuePlanTestPrivate
{
class FValueLease final : public IAvidScriptTaskValueLease
{
public:
    explicit FValueLease(AvidScript::TaskResult::FCapturedValue&& InValue) : Value(MoveTemp(InValue)) {}
    std::span<const std::uint8_t> GetBytes() const { return Value.GetBytes(); }
    AvidScript::TaskResult::EValueError Read(const AvidScript::TaskResult::FValuePlan& Plan,
        AvidScript::Managed::FHeap& Heap, std::span<std::uint8_t> Bytes) const
    { return Value.ReadIntoFrame(Plan, Heap, Bytes, 0); }
private:
    AvidScript::TaskResult::FCapturedValue Value;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAvidScriptTaskValuePlanSessionTest,
    "AvidScript.Runtime.Continuation.TaskValuePlan.SourceCatalogAndSessionCapture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptTaskValuePlanSessionTest::RunTest(const FString& Parameters)
{
    using namespace AvidScript::TaskResult;
    using namespace AvidScript::Managed;
    const FString Directory = FPlatformMisc::GetEnvironmentVariable(TEXT("AVIDSCRIPT_TASK_VALUE_PLAN_DIR"));
    TArray<uint8> Packet;
    if (!TestFalse(TEXT("Compiler plan directory is explicitly configured"), Directory.IsEmpty())
        || !TestTrue(TEXT("Original compiler nested plan reads"), FFileHelper::LoadFileToArray(Packet,
            *FPaths::Combine(Directory, TEXT("nested.plan"))))) return false;
    FValuePlan Plan;
    if (!TestTrue(TEXT("Native parser reads compiler plan"), ReadValuePlan(
        {Packet.GetData(), static_cast<std::size_t>(Packet.Num())}, Plan) == EValueError::Ok)) return false;
    if (!TestTrue(TEXT("Compiler nested value declares references"), Plan.GetSize() == 40 && Plan.RequiresLease())) return false;
    FHeap Heap;
    const std::array<FHeapLayout, 1> Layouts{{{1, 16, {{8, 1}}}}};
    if (!TestTrue(TEXT("Payload heap configures"), Heap.Configure(Layouts) == EHeapError::Ok)) return false;
    FToken ProducerFrame = 0, ParentRoot = 0, ChildRoot = 0, Parent = 0, Child = 0;
    TestTrue(TEXT("Producer frame starts"), Heap.PushFrame(ProducerFrame) == EHeapError::Ok);
    TestTrue(TEXT("Producer owns parent root"), Heap.CreateRoot(ProducerFrame, 0, ParentRoot) == EHeapError::Ok);
    TestTrue(TEXT("Producer owns child root"), Heap.CreateRoot(ProducerFrame, 0, ChildRoot) == EHeapError::Ok);
    TestTrue(TEXT("Parent allocates"), Heap.Allocate(1, ParentRoot, Parent) == EHeapError::Ok);
    TestTrue(TEXT("Child allocates"), Heap.Allocate(1, ChildRoot, Child) == EHeapError::Ok);
    TestTrue(TEXT("Parent references child"), Heap.WriteReference(Parent, 1, 8, Child) == EHeapError::Ok);
    std::array<std::uint8_t, 40> Bytes{};
    Bytes[0] = 7; Bytes[11] = 0x80; Bytes[32] = 3;
    for (unsigned I = 0; I < 8; ++I) Bytes[16 + I] = Bytes[24 + I] = static_cast<uint8>(Parent >> (I * 8));
    const FManagedTypeResolver Resolver = [](const FValueLeaf& Leaf, std::uint32_t& Type)
    {
        Type = 1;
        return Leaf.TypeId == "r:\xe7\x8e\xa9\xe5\xae\xb6" && Leaf.TargetTypeId == "p:node";
    };
    FCapturedValue Captured;
    if (!TestTrue(TEXT("Validated nested value acquires roots"),
        CaptureValue(Plan, Heap, Bytes, 0, Resolver, Captured) == EValueError::Ok)) return false;
    TestEqual(TEXT("Aliased nested fields share one persistent root"), static_cast<int32>(Captured.GetRootCount()), 1);
    TStrongObjectPtr<UWorld> World(NewObject<UWorld>());
    const auto Owner = MakeShared<FAvidScriptSessionContinuations>();
    auto& Host = Owner->ResetActive(World.Get());
    // Native fixture identity only. Executable payload admission remains a later group.
    const FString TypeId = UTF8_TO_TCHAR(Plan.GetTypeId().c_str());
    const int64 Task = Host.CreateRootedTaskResult(TypeId);
    int64 FirstWaiter = 0, SecondWaiter = 0;
    TestTrue(TEXT("First reader queues"), Host.AwaitTaskResult(Task, 611, FirstWaiter) == EAvidScriptTaskWaitRegistration::Queued);
    TestTrue(TEXT("Second reader queues"), Host.AwaitTaskResult(Task, 612, SecondWaiter) == EAvidScriptTaskWaitRegistration::Queued);
    TSharedPtr<AvidScriptTaskValuePlanTestPrivate::FValueLease> ConcreteLease =
        MakeShared<AvidScriptTaskValuePlanTestPrivate::FValueLease>(MoveTemp(Captured));
    const TWeakPtr<AvidScriptTaskValuePlanTestPrivate::FValueLease> Reader = ConcreteLease;
    TSharedPtr<IAvidScriptTaskValueLease> Lease = ConcreteLease;
    TArray<int64> Woken;
    TestTrue(TEXT("Session owns validated captured result"), Host.SucceedRootedTaskResult(Task,
        {ConcreteLease->GetBytes().data(), static_cast<int32>(ConcreteLease->GetBytes().size())}, MoveTemp(Lease), Woken));
    ConcreteLease.Reset();
    TestTrue(TEXT("Producer releases task"), Host.ReleaseTaskResult(Task));
    TestTrue(TEXT("Producer exits"), Heap.PopFrame(ProducerFrame) == EHeapError::Ok);
    TestTrue(TEXT("Result graph survives GC"), Heap.Collect() == EHeapError::Ok && Heap.IsAlive(Parent) && Heap.IsAlive(Child));
    FAvidScriptTaskResultSnapshot Snapshot;
    TestTrue(TEXT("Captured typed result reads"), Host.ReadTaskResult(Task, Snapshot)
        && Snapshot.TypeId == TypeId && Snapshot.bValueRequiresLease && Snapshot.Value.Num() == 40);
    if (Snapshot.Value.Num() == 40)
    {
        TestEqual(TEXT("Float negative-zero bit pattern survives"), Snapshot.Value[11], uint8(0x80));
        TestTrue(TEXT("Alias bytes remain identical"), FMemory::Memcmp(Snapshot.Value.GetData() + 16, Snapshot.Value.GetData() + 24, 8) == 0);
    }
    TArray<FAvidScriptContinuationCompletion> Ready;
    Owner->DrainReady(Ready);
    TestTrue(TEXT("First reader completes"), Owner->FinalizeDispatched(FirstWaiter, true));
    TestTrue(TEXT("Second reader keeps graph alive"), Heap.Collect() == EHeapError::Ok && Heap.IsAlive(Parent));
    Owner->DrainReady(Ready);
    FToken ConsumerFrame = 0;
    TestTrue(TEXT("Consumer frame starts"), Heap.PushFrame(ConsumerFrame) == EHeapError::Ok);
    std::array<std::uint8_t, 40> ReadBytes{};
    auto PinnedReader = Reader.Pin();
    TestTrue(TEXT("Task still owns its concrete result reader"), PinnedReader.IsValid());
    if (PinnedReader)
        TestTrue(TEXT("Reader atomically acquires frame roots and bytes"), PinnedReader->Read(Plan, Heap, ReadBytes) == EValueError::Ok
            && ReadBytes == Bytes);
    PinnedReader.Reset();
    TestTrue(TEXT("Last reader completes"), Owner->FinalizeDispatched(SecondWaiter, true));
    TestFalse(TEXT("Final task reference releases concrete owner"), Reader.IsValid());
    TestTrue(TEXT("Consumer retains nested graph"), Heap.Collect() == EHeapError::Ok && Heap.IsAlive(Parent) && Heap.IsAlive(Child));
    TestTrue(TEXT("Consumer exits"), Heap.PopFrame(ConsumerFrame) == EHeapError::Ok);
    TestTrue(TEXT("Last root release collects result graph"), Heap.Collect() == EHeapError::Ok && Heap.GetStats().LiveObjects == 0);
    Owner->Teardown();
    TestEqual(TEXT("No value roots remain"), Heap.GetStats().LiveRoots, 0u);
    return true;
}

#endif
