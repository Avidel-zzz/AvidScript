#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptUtf8ValueHeap.h"

#include "Misc/AutomationTest.h"

namespace
{
bool InternUtf8(
	FAvidScriptUtf8ValueHeap& Heap,
	const TConstArrayView<uint8> Bytes,
	uint32& OutToken,
	bool& bOutCreated,
	FString& OutError)
{
	FAvidScriptUtf8ValueReservation Reservation;
	return Heap.Reserve(Reservation, OutError)
		&& Heap.InternReserved(
			Reservation,
			Bytes,
			OutToken,
			bOutCreated,
			OutError);
}
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptUtf8ValueHeapTest,
	"AvidScript.Bindings.Utf8ValueHeap.Core",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptUtf8ValueHeapTest::RunTest(const FString& Parameters)
{
	FAvidScriptUtf8ValueHeap Heap;
	FString Error;
	uint32 EmptyToken = 0;
	bool bCreated = false;
	TestTrue(
		TEXT("Empty UTF-8 value interns"),
		InternUtf8(Heap, TConstArrayView<uint8>(), EmptyToken, bCreated, Error));
	TestTrue(TEXT("Empty UTF-8 value creates a live token"), bCreated);
	TestTrue(TEXT("Heap tokens retain the high-bit tag"), FAvidScriptUtf8ValueHeap::IsHeapToken(EmptyToken));

	const uint8 UnicodeBytes[] = {
		0x41, 0xe6, 0xb8, 0xb8, 0xe6, 0x88, 0x8f
	};
	uint32 UnicodeToken = 0;
	TestTrue(
		TEXT("Unicode UTF-8 value interns"),
		InternUtf8(Heap, UnicodeBytes, UnicodeToken, bCreated, Error));
	TestTrue(TEXT("First Unicode intern creates a live token"), bCreated);
	uint32 DuplicateToken = 0;
	TestTrue(
		TEXT("Duplicate Unicode UTF-8 value interns"),
		InternUtf8(Heap, UnicodeBytes, DuplicateToken, bCreated, Error));
	TestFalse(TEXT("Duplicate Unicode intern reuses storage"), bCreated);
	TestEqual(TEXT("Duplicate Unicode intern reuses its token"), DuplicateToken, UnicodeToken);
	TestEqual(TEXT("Heap contains two unique values"), Heap.GetLiveValueCount(), 2);
	TestEqual(TEXT("Successful intern consumes all reservations"), Heap.GetReservedValueCount(), 0);

	TConstArrayView<uint8> Resolved;
	TestTrue(TEXT("Unicode token resolves"), Heap.Resolve(UnicodeToken, Resolved, Error));
	TestTrue(
		TEXT("Unicode token preserves exact bytes"),
		Resolved.Num() == UE_ARRAY_COUNT(UnicodeBytes)
			&& FMemory::Memcmp(
				Resolved.GetData(),
				UnicodeBytes,
				UE_ARRAY_COUNT(UnicodeBytes)) == 0);

	FAvidScriptUtf8ValueHeap OtherHeap;
	const uint8 OtherBytes[] = { 0x6f, 0x74, 0x68, 0x65, 0x72 };
	uint32 OtherToken = 0;
	TestTrue(
		TEXT("A second runtime heap can intern independently"),
		InternUtf8(OtherHeap, OtherBytes, OtherToken, bCreated, Error));
	TestNotEqual(
		TEXT("Runtime heaps receive distinct process capabilities"),
		OtherToken,
		UnicodeToken);
	TestFalse(
		TEXT("A token from the first runtime fails in the second runtime"),
		OtherHeap.Resolve(UnicodeToken, Resolved, Error));
	TestFalse(
		TEXT("A token from the second runtime fails in the first runtime"),
		Heap.Resolve(OtherToken, Resolved, Error));

	FAvidScriptUtf8ValueReservation ReleasedReservation;
	TestTrue(TEXT("Reservation can be acquired"), Heap.Reserve(ReleasedReservation, Error));
	const uint32 ReleasedToken = ReleasedReservation.Token;
	Heap.ReleaseReservation(ReleasedReservation);
	TestFalse(TEXT("Released reservation is inactive"), ReleasedReservation.bActive);
	TestFalse(TEXT("Released reservation token does not resolve"), Heap.Resolve(ReleasedToken, Resolved, Error));

	const uint8 InvalidUtf8[] = { 0xc0, 0xaf };
	FAvidScriptUtf8ValueReservation InvalidReservation;
	TestTrue(TEXT("Invalid UTF-8 test reserves a slot"), Heap.Reserve(InvalidReservation, Error));
	uint32 InvalidToken = 0;
	TestFalse(
		TEXT("Non-canonical UTF-8 fails closed"),
		Heap.InternReserved(
			InvalidReservation,
			InvalidUtf8,
			InvalidToken,
			bCreated,
			Error));
	TestTrue(TEXT("Invalid UTF-8 reports its category"), Error.Contains(TEXT("utf8_value_invalid")));
	Heap.ReleaseReservation(InvalidReservation);

	TArray<uint8> Oversized;
	Oversized.SetNumZeroed(FAvidScriptUtf8ValueHeap::MaxValueBytes + 1u);
	FAvidScriptUtf8ValueReservation OversizedReservation;
	TestTrue(TEXT("Oversized UTF-8 test reserves a slot"), Heap.Reserve(OversizedReservation, Error));
	TestFalse(
		TEXT("Oversized UTF-8 value fails closed"),
		Heap.InternReserved(
			OversizedReservation,
			Oversized,
			InvalidToken,
			bCreated,
			Error));
	TestTrue(TEXT("Oversized UTF-8 reports its category"), Error.Contains(TEXT("utf8_value_too_large")));
	Heap.ReleaseReservation(OversizedReservation);

	Heap.RemoveCreatedValue(UnicodeToken);
	TestFalse(TEXT("Removed live token becomes stale"), Heap.Resolve(UnicodeToken, Resolved, Error));
	uint32 ReusedToken = 0;
	TestTrue(
		TEXT("Released live slot can be reused"),
		InternUtf8(Heap, UnicodeBytes, ReusedToken, bCreated, Error));
	TestNotEqual(TEXT("Reused live slot receives a fresh capability"), ReusedToken, UnicodeToken);

	const uint32 BeforeResetToken = ReusedToken;
	Heap.Reset();
	TestEqual(TEXT("Reset clears live values"), Heap.GetLiveValueCount(), 0);
	TestFalse(TEXT("Reset invalidates old tokens"), Heap.Resolve(BeforeResetToken, Resolved, Error));
	uint32 AfterResetToken = 0;
	TestTrue(
		TEXT("Heap accepts values after reset"),
		InternUtf8(Heap, UnicodeBytes, AfterResetToken, bCreated, Error));
	TestNotEqual(TEXT("Reset cannot reuse an old capability"), AfterResetToken, BeforeResetToken);

	FAvidScriptUtf8ValueHeap ExhaustionHeap;
	TArray<FAvidScriptUtf8ValueReservation> Reservations;
	Reservations.SetNum(FAvidScriptUtf8ValueHeap::MaxSlots);
	for (FAvidScriptUtf8ValueReservation& Reservation : Reservations)
	{
		if (!ExhaustionHeap.Reserve(Reservation, Error))
		{
			AddError(TEXT("UTF-8 value heap exhausted before its declared slot limit: ") + Error);
			return false;
		}
	}
	FAvidScriptUtf8ValueReservation OverflowReservation;
	TestFalse(
		TEXT("UTF-8 value heap rejects a reservation beyond its slot limit"),
		ExhaustionHeap.Reserve(OverflowReservation, Error));
	TestTrue(TEXT("Slot exhaustion reports its category"), Error.Contains(TEXT("utf8_value_heap_exhausted")));
	for (FAvidScriptUtf8ValueReservation& Reservation : Reservations)
	{
		ExhaustionHeap.ReleaseReservation(Reservation);
	}
	TestEqual(TEXT("Released exhaustion fixture has no reservations"), ExhaustionHeap.GetReservedValueCount(), 0);
	return true;
}

#endif
