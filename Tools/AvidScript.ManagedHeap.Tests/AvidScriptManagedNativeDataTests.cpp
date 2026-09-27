#include "Memory/AvidScriptManagedHeap.h"

#include <array>
#include <stdexcept>
#include <utility>

namespace
{
using namespace AvidScript::Managed;
constexpr std::uint32_t Kind = 0x54455354;
const std::array<FHeapLayout, 1> Layouts{{{1, 16, {}}}};
struct FCounts { int Created = 0, Destroyed = 0; };
struct FRecord final : FNativeObjectData
{
	FRecord(FCounts& InCounts, std::int64_t InValue = -0x100000001LL,
		std::uint32_t InKind = Kind, std::uint32_t Size = sizeof(FRecord))
		: FNativeObjectData(InKind, Size), Counts(InCounts), Value(InValue) { ++Counts.Created; }
	~FRecord() override { ++Counts.Destroyed; }
	FCounts& Counts;
	const std::int64_t Value;
};
void Check(bool Condition, const char* Message) { if (!Condition) throw std::runtime_error(Message); }
void Ok(EHeapError Error) { Check(Error == EHeapError::Ok, "native object data operation failed"); }
FToken Root(FHeap& Heap) { FToken Value = 0; Ok(Heap.CreateRoot(0, 0, Value)); return Value; }
FToken Object(FHeap& Heap, FToken RootToken) { FToken Value = 0; Ok(Heap.Allocate(1, RootToken, Value)); return Value; }
const FRecord* Find(const FHeap& Heap, FToken Value)
{
	return static_cast<const FRecord*>(Heap.FindNativeData(Value, Kind));
}
void PublicationAndGuestWrites()
{
	FCounts Counts;
	FHeap Heap; Ok(Heap.Configure(Layouts));
	const auto R = Root(Heap), A = Object(Heap, R);
	FPendingNativeData Pending;
	Ok(Heap.PrepareNativeData(A, std::make_unique<FRecord>(Counts), Pending));
	Check(!Find(Heap, A), "unpublished data escaped");
	Check(Heap.GetStats().NativeDataObjects == 1 && Heap.GetStats().NativeDataBytes == sizeof(FRecord)
		&& Heap.GetStats().LiveBytes == 16 + sizeof(FRecord) && Heap.GetStats().LiveRoots == 1,
		"reservation escaped byte or root accounting");
	FPendingNativeData Moved(std::move(Pending)); Pending.Reset();
	Ok(Moved.Commit()); Moved.Reset();
	const auto* Data = Find(Heap, A);
	Check(Data && Data->Value == -0x100000001LL, "publication truncated native identity");
	std::array<std::uint8_t, 16> Bytes; Bytes.fill(0xff);
	Ok(Heap.WriteBytes(A, 1, 0, Bytes));
	Check(Find(Heap, A) == Data && Data->Value == -0x100000001LL, "Guest writes changed native data");
	Check(!Heap.FindNativeData(A, Kind + 1) && !Heap.FindNativeData(A, 0), "wrong native kind accepted");
	Ok(Heap.ReleaseRoot(R)); Ok(Heap.Collect());
	Check(!Find(Heap, A) && Heap.GetStats().LiveBytes == 0 && Heap.GetStats().NativeDataObjects == 0
		&& Counts.Created == Counts.Destroyed, "published data leaked after collection");
}
void RollbackAndAtomicOutput()
{
	FCounts Counts;
	FHeap Heap; Ok(Heap.Configure(Layouts));
	const auto A = Object(Heap, Root(Heap)), B = Object(Heap, Root(Heap));
	FPendingNativeData Pending;
	Ok(Heap.PrepareNativeData(A, std::make_unique<FRecord>(Counts), Pending));
	Check(Heap.PrepareNativeData(B, std::make_unique<FRecord>(Counts, 4, 0), Pending) == EHeapError::InvalidNativeData,
		"invalid replacement accepted");
	Check(Heap.GetStats().NativeDataObjects == 1, "failed replacement lost previous reservation");
	Ok(Pending.Commit()); Check(Find(Heap, A), "failed replacement changed guard owner");
	{
		FPendingNativeData Abandoned;
		Ok(Heap.PrepareNativeData(B, std::make_unique<FRecord>(Counts), Abandoned));
	}
	Check(!Find(Heap, B) && Heap.GetStats().NativeDataObjects == 1, "guard destruction did not roll back");
	const auto C = Object(Heap, Root(Heap));
	Ok(Heap.PrepareNativeData(B, std::make_unique<FRecord>(Counts), Pending));
	Ok(Heap.PrepareNativeData(C, std::make_unique<FRecord>(Counts, 9), Pending));
	Ok(Pending.Commit());
	Check(!Find(Heap, B) && Find(Heap, C) && Find(Heap, C)->Value == 9
		&& Heap.GetStats().NativeDataObjects == 2, "successful guard replacement leaked old reservation");
	Heap.Close(); Check(Counts.Created == Counts.Destroyed, "rollback leaked payload allocation");
}
void BudgetsIncludePendingData()
{
	FCounts Counts;
	FHeapLimits Limits; Limits.MaxLiveBytes = 32 + sizeof(FRecord);
	FHeap Heap(Limits); Ok(Heap.Configure(Layouts));
	const auto A = Object(Heap, Root(Heap)), B = Object(Heap, Root(Heap));
	FPendingNativeData Pending;
	Ok(Heap.PrepareNativeData(A, std::make_unique<FRecord>(Counts), Pending));
	Check(Heap.PrepareNativeData(B, std::make_unique<FRecord>(Counts), Pending) == EHeapError::ByteLimit,
		"pending native payload did not consume budget");
	Check(Heap.GetStats().LiveBytes == Limits.MaxLiveBytes, "failed prepare changed reserved budget");
	Pending.Reset();
	Check(Heap.GetStats().NativeDataBytes == 0 && Heap.GetStats().LiveBytes == 32, "rollback did not refund bytes");
	Ok(Heap.PrepareNativeData(B, std::make_unique<FRecord>(Counts), Pending)); Ok(Pending.Commit());
	const auto R = Root(Heap); FToken Rejected = 123;
	Check(Heap.Allocate(1, R, Rejected) == EHeapError::ByteLimit && Rejected == 0,
		"ordinary allocation bypassed native payload budget");
	Check(Heap.GetStats().PeakLiveBytes == Limits.MaxLiveBytes, "native payload missing from peak accounting");
	FHeapLimits PerObject; PerObject.MaxObjectBytes = 15 + sizeof(FRecord);
	FHeap Small(PerObject); Ok(Small.Configure(Layouts));
	const auto C = Object(Small, Root(Small));
	Check(Small.PrepareNativeData(C, std::make_unique<FRecord>(Counts), Pending) == EHeapError::ByteLimit,
		"per-object total ignored native payload");
	Check(Small.GetStats().LiveBytes == 16 && !Find(Small, C), "byte rejection mutated target");
}
void BudgetCollectionKeepsRootedTarget()
{
	FCounts Counts;
	FHeapLimits Limits; Limits.MaxLiveBytes = 16 + sizeof(FRecord);
	FHeap Heap(Limits); Ok(Heap.Configure(Layouts));
	const auto R = Root(Heap), Dead = Object(Heap, R), Live = Object(Heap, R);
	FPendingNativeData Pending;
	Ok(Heap.PrepareNativeData(Live, std::make_unique<FRecord>(Counts), Pending)); Ok(Pending.Commit());
	Check(!Heap.IsAlive(Dead) && Find(Heap, Live) && Heap.GetStats().LiveBytes == Limits.MaxLiveBytes,
		"native reservation failed to reclaim unrooted bytes under pressure");
}
void GuardDoesNotRootOrCorruptReusedSlot()
{
	FCounts Counts;
	FHeap Heap; Ok(Heap.Configure(Layouts));
	const auto R = Root(Heap), Old = Object(Heap, R);
	FPendingNativeData OldPending;
	Ok(Heap.PrepareNativeData(Old, std::make_unique<FRecord>(Counts), OldPending));
	Ok(Heap.SetRoot(R, 0)); Ok(Heap.Collect());
	Check(!Heap.IsAlive(Old) && Heap.GetStats().NativeDataBytes == 0, "guard secretly rooted object");
	Check(OldPending.Commit() == EHeapError::InvalidObject, "collected object publication succeeded");
	const auto Fresh = Object(Heap, R);
	FPendingNativeData FreshPending;
	Ok(Heap.PrepareNativeData(Fresh, std::make_unique<FRecord>(Counts, 7), FreshPending));
	OldPending.Reset();
	Ok(FreshPending.Commit());
	Check(Old != Fresh && Find(Heap, Fresh)->Value == 7 && Heap.GetStats().NativeDataObjects == 1,
		"stale guard corrupted reused object generation");
}
void IdentityAndDataValidation()
{
	FCounts Counts;
	FHeap Heap, Foreign; Ok(Heap.Configure(Layouts)); Ok(Foreign.Configure(Layouts));
	const auto R = Root(Heap), A = Object(Heap, R), Other = Object(Foreign, Root(Foreign));
	FPendingNativeData Pending;
	for (const auto Invalid : {FToken(0), R, Other, FToken(~0ULL)})
	{
		Check(Heap.PrepareNativeData(Invalid, std::make_unique<FRecord>(Counts), Pending) == EHeapError::InvalidObject,
			"foreign, null or wrong-kind object accepted");
		Check(!Find(Heap, Invalid), "invalid identity was readable");
	}
	Check(Heap.PrepareNativeData(A, nullptr, Pending) == EHeapError::InvalidNativeData, "null native data accepted");
	Check(Heap.PrepareNativeData(A, std::make_unique<FRecord>(Counts, 1, Kind, 1), Pending) == EHeapError::InvalidNativeData,
		"undersized native record accepted");
	Ok(Heap.PrepareNativeData(A, std::make_unique<FRecord>(Counts), Pending));
	FPendingNativeData Conflict;
	Check(Heap.PrepareNativeData(A, std::make_unique<FRecord>(Counts), Conflict) == EHeapError::NativeDataConflict,
		"pending reservation overwritten");
	Ok(Pending.Commit());
	Check(Heap.PrepareNativeData(A, std::make_unique<FRecord>(Counts), Conflict) == EHeapError::NativeDataConflict,
		"published record overwritten");
	Heap.Close();
	Check(!Find(Heap, A) && Heap.PrepareNativeData(A, std::make_unique<FRecord>(Counts), Pending) == EHeapError::Closed,
		"closed heap allowed native data access");
	Check(Heap.GetStats().LiveBytes == 0 && Heap.GetStats().NativeDataObjects == 0
		&& Counts.Created == Counts.Destroyed, "validation or close leaked native data");
}
void HeapAddressReuseAndLateGuard()
{
	FCounts Counts;
	FPendingNativeData Late;
	alignas(FHeap) std::array<std::byte, sizeof(FHeap)> Storage;
	FHeap* Old = std::construct_at(reinterpret_cast<FHeap*>(Storage.data())); Ok(Old->Configure(Layouts));
	const auto A = Object(*Old, Root(*Old));
	Ok(Old->PrepareNativeData(A, std::make_unique<FRecord>(Counts), Late));
	std::destroy_at(Old);
	Check(Late.Commit() == EHeapError::Closed && Counts.Created == Counts.Destroyed, "dead heap guard retained data");
	FHeap* Fresh = std::construct_at(reinterpret_cast<FHeap*>(Storage.data())); Ok(Fresh->Configure(Layouts));
	const auto B = Object(*Fresh, Root(*Fresh));
	FPendingNativeData Pending;
	Ok(Fresh->PrepareNativeData(B, std::make_unique<FRecord>(Counts, 11), Pending));
	Late.Reset(); Ok(Pending.Commit());
	Check(Find(*Fresh, B)->Value == 11, "late guard affected same-address replacement heap");
	Ok(Fresh->PrepareNativeData(Object(*Fresh, Root(*Fresh)), std::make_unique<FRecord>(Counts), Late));
	Fresh->Close(); Fresh->Close(); Late.Reset(); std::destroy_at(Fresh);
	Check(Counts.Created == Counts.Destroyed, "idempotent close or late reset leaked payload");
}
void GraphReachabilityOwnsNativeData()
{
	FCounts Counts;
	FHeap Heap;
	const std::array<FHeapLayout, 1> GraphLayout{{{1, 16, {{0, 1}}}}};
	Ok(Heap.Configure(GraphLayout));
	const auto RA = Root(Heap), RB = Root(Heap), A = Object(Heap, RA), B = Object(Heap, RB);
	Ok(Heap.WriteReference(A, 1, 0, B)); Ok(Heap.ReleaseRoot(RB));
	FPendingNativeData Pending;
	Ok(Heap.PrepareNativeData(B, std::make_unique<FRecord>(Counts), Pending)); Ok(Pending.Commit());
	Ok(Heap.Collect()); Check(Find(Heap, B), "transitively reachable payload was collected");
	Ok(Heap.WriteReference(A, 1, 0, 0)); Ok(Heap.Collect());
	Check(!Find(Heap, B) && Heap.GetStats().NativeDataBytes == 0, "unreachable child kept native data");
	Ok(Heap.ReleaseRoot(RA)); Ok(Heap.Collect());
	Check(Heap.GetStats().LiveBytes == 0 && Counts.Created == Counts.Destroyed, "native payload outlived graph");
}
}

int RunManagedNativeDataTests()
{
	PublicationAndGuestWrites(); RollbackAndAtomicOutput(); BudgetsIncludePendingData(); BudgetCollectionKeepsRootedTarget();
	GuardDoesNotRootOrCorruptReusedSlot(); IdentityAndDataValidation(); HeapAddressReuseAndLateGuard(); GraphReachabilityOwnsNativeData();
	return 8;
}
