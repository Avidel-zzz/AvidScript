#if WITH_DEV_AUTOMATION_TESTS

#include "AvidScriptCompositeValueHeap.h"
#include "AvidScriptBindingsTestTypes.h"

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/UnrealType.h"

namespace
{
struct FAvidScriptCompositeBenchmarkCase
{
	const TCHAR* Label;
	const TCHAR* PropertyName;
	const TCHAR* TypeId;
	EAvidScriptCompositeValueKind Kind;
};

bool RunAvidScriptCompositeBenchmarkIteration(
	FAvidScriptCompositeValueHeap& Heap,
	FProperty& Property,
	const FString& TypeId,
	const EAvidScriptCompositeValueKind Kind,
	const void* SourceValue,
	void* DestinationValue,
	FString& OutError)
{
	FAvidScriptCompositeValueReservation Reservation;
	if (!Heap.Reserve(Reservation, OutError))
	{
		return false;
	}

	uint32 Token = 0;
	if (!Heap.PublishReserved(
			Reservation,
			TypeId,
			Kind,
			Property,
			SourceValue,
			TConstArrayView<uint32>(),
			Token,
			OutError))
	{
		Heap.ReleaseReservation(Reservation);
		return false;
	}

	if (!Heap.CopyToProperty(
			Token,
			TypeId,
			Property,
			DestinationValue,
			OutError))
	{
		Heap.ReleaseValue(Token, OutError);
		return false;
	}

	return Heap.ReleaseValue(Token, OutError);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCompositeValueHeapIdentityTest,
	"AvidScript.Bindings.CompositeValueHeap.Identity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompositeValueHeapIdentityTest::RunTest(const FString& Parameters)
{
	UAvidScriptBindingsTestObject* Fixture =
		NewObject<UAvidScriptBindingsTestObject>(GetTransientPackage());
	UObject* ReferencedObject =
		NewObject<UAvidScriptBindingsDerivedTestObject>(GetTransientPackage());
	if (!TestNotNull(TEXT("Composite fixture exists"), Fixture)
		|| !TestNotNull(TEXT("Referenced object exists"), ReferencedObject))
	{
		return false;
	}

	Fixture->CompositeTextProperty = FText::FromString(TEXT("Avid composite text"));
	Fixture->CompositeSoftObjectProperty = ReferencedObject;
	Fixture->CompositeWeakObjectProperty = ReferencedObject;

	struct FCase
	{
		const TCHAR* PropertyName;
		const TCHAR* TypeId;
		EAvidScriptCompositeValueKind Kind;
	};
	const FCase Cases[] = {
		{ TEXT("CompositeTextProperty"), TEXT("type:text"), EAvidScriptCompositeValueKind::Text },
		{ TEXT("CompositeSoftObjectProperty"), TEXT("type:soft"), EAvidScriptCompositeValueKind::SoftObject },
		{ TEXT("CompositeWeakObjectProperty"), TEXT("type:weak"), EAvidScriptCompositeValueKind::WeakObject }
	};

	FAvidScriptCompositeValueHeap Heap;
	for (const FCase& Case : Cases)
	{
		FProperty* Property = FindFProperty<FProperty>(
			UAvidScriptBindingsTestObject::StaticClass(),
			Case.PropertyName);
		if (!TestNotNull(*FString::Printf(TEXT("%s property exists"), Case.PropertyName), Property))
		{
			continue;
		}

		FAvidScriptCompositeValueReservation Reservation;
		FString Error;
		if (!TestTrue(
				*FString::Printf(TEXT("%s reserves a bounded slot"), Case.PropertyName),
				Heap.Reserve(Reservation, Error)))
		{
			AddError(Error);
			continue;
		}
		uint32 Token = 0;
		if (!TestTrue(
				*FString::Printf(TEXT("%s publishes exact property identity"), Case.PropertyName),
				Heap.PublishReserved(
					Reservation,
					Case.TypeId,
					Case.Kind,
					*Property,
					Property->ContainerPtrToValuePtr<void>(Fixture),
					TConstArrayView<uint32>(),
					Token,
					Error)))
		{
			AddError(Error);
			continue;
		}

		Property->ClearValue_InContainer(Fixture);
		TestTrue(
			*FString::Printf(TEXT("%s restores through the exact reflected property"), Case.PropertyName),
			Heap.CopyToProperty(
				Token,
				Case.TypeId,
				*Property,
				Property->ContainerPtrToValuePtr<void>(Fixture),
				Error));
		if (Case.Kind == EAvidScriptCompositeValueKind::Text)
		{
			TestTrue(
				TEXT("FText identity survives the capability boundary"),
				Fixture->CompositeTextProperty.EqualTo(
					FText::FromString(TEXT("Avid composite text"))));
		}
		else if (Case.Kind == EAvidScriptCompositeValueKind::SoftObject)
		{
			TestEqual(
				TEXT("Soft object identity remains lazy and exact"),
				Fixture->CompositeSoftObjectProperty.Get(),
				ReferencedObject);
		}
		else
		{
			TestEqual(
				TEXT("Weak object identity does not become a strong handle"),
				Fixture->CompositeWeakObjectProperty.Get(),
				ReferencedObject);
		}

		TestTrue(TEXT("Published composite token releases"), Heap.ReleaseValue(Token, Error));
		TestFalse(
			TEXT("Released composite token fails closed"),
			Heap.CopyToProperty(
				Token,
				Case.TypeId,
				*Property,
				Property->ContainerPtrToValuePtr<void>(Fixture),
				Error));
	}

	const FAvidScriptCompositeValueHeapStats Stats = Heap.GetStats();
	TestEqual(TEXT("All composite slots are released"), Stats.LiveValues, 0);
	TestEqual(
		TEXT("All composite bytes are released"),
		Stats.LiveBytes,
		static_cast<uint64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCompositeValueHeapRecursiveBudgetTest,
	"AvidScript.Bindings.CompositeValueHeap.RecursiveBudget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompositeValueHeapRecursiveBudgetTest::RunTest(const FString& Parameters)
{
	UAvidScriptBindingsTestObject* Fixture =
		NewObject<UAvidScriptBindingsTestObject>(GetTransientPackage());
	FArrayProperty* ArrayProperty = FindFProperty<FArrayProperty>(
		UAvidScriptBindingsTestObject::StaticClass(),
		TEXT("CompositeStringArrayProperty"));
	FStructProperty* StrongStructProperty = FindFProperty<FStructProperty>(
		UAvidScriptBindingsTestObject::StaticClass(),
		TEXT("RecursiveStructProperty"));
	if (!TestNotNull(TEXT("Composite fixture exists"), Fixture)
		|| !TestNotNull(TEXT("String array property exists"), ArrayProperty)
		|| !TestNotNull(TEXT("Strong object struct property exists"), StrongStructProperty))
	{
		return false;
	}

	FAvidScriptCompositeValueHeap Heap;
	FString Error;
	Fixture->CompositeStringArrayProperty = { TEXT("alpha"), TEXT("beta") };
	FAvidScriptCompositeValueReservation Reservation;
	TestTrue(TEXT("String array reserves a slot"), Heap.Reserve(Reservation, Error));
	uint32 Token = 0;
	TestTrue(
		TEXT("Bounded recursive string array publishes"),
		Heap.PublishReserved(
			Reservation,
			TEXT("type:string-array"),
			EAvidScriptCompositeValueKind::Array,
			*ArrayProperty,
			ArrayProperty->ContainerPtrToValuePtr<void>(Fixture),
			TConstArrayView<uint32>(),
			Token,
			Error));
	TestTrue(
		TEXT("Dynamic string storage contributes to live byte accounting"),
		Heap.GetStats().LiveBytes > static_cast<uint64>(ArrayProperty->GetSize()));

	Fixture->CompositeStringArrayProperty = {
		FString::ChrN(
			static_cast<int32>(FAvidScriptCompositeValueHeap::MaxValueBytes),
			TEXT('x'))
	};
	TestFalse(
		TEXT("Oversized replacement fails before mutating the stored value"),
		Heap.ReplaceValue(
			Token,
			*ArrayProperty,
			ArrayProperty->ContainerPtrToValuePtr<void>(Fixture),
			Error));
	TestTrue(
		TEXT("Oversized replacement reports the recursive graph limit"),
		Error.Contains(TEXT("composite_value_limit_exceeded")));
	Fixture->CompositeStringArrayProperty.Reset();
	TestTrue(
		TEXT("Stored value remains readable after rejected replacement"),
		Heap.CopyToProperty(
			Token,
			TEXT("type:string-array"),
			*ArrayProperty,
			ArrayProperty->ContainerPtrToValuePtr<void>(Fixture),
			Error));
	TestEqual(TEXT("Rejected replacement preserves element count"), Fixture->CompositeStringArrayProperty.Num(), 2);
	TestEqual(TEXT("Rejected replacement preserves first value"), Fixture->CompositeStringArrayProperty[0], FString(TEXT("alpha")));

	FAvidScriptCompositeValueReservation StrongReservation;
	TestTrue(TEXT("Strong object graph reserves a slot"), Heap.Reserve(StrongReservation, Error));
	uint32 StrongToken = 0;
	TestFalse(
		TEXT("Strong UObject leaves fail closed without a GC anchor"),
		Heap.PublishReserved(
			StrongReservation,
			TEXT("type:strong-struct"),
			EAvidScriptCompositeValueKind::DelegateFrame,
			*StrongStructProperty,
			StrongStructProperty->ContainerPtrToValuePtr<void>(Fixture),
			TConstArrayView<uint32>(),
			StrongToken,
			Error));
	TestTrue(
		TEXT("Strong object graph reports its GC contract"),
		Error.Contains(TEXT("composite_value_gc_unsupported")));
	Heap.ReleaseReservation(StrongReservation);

	TestTrue(TEXT("Recursive test value releases"), Heap.ReleaseValue(Token, Error));
	TestEqual(TEXT("Recursive budget test leaves no live bytes"), Heap.GetStats().LiveBytes, static_cast<uint64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FAvidScriptCompositeValueHeapBenchmarkTest,
	"AvidScript.Performance.CompositeValueHeap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAvidScriptCompositeValueHeapBenchmarkTest::RunTest(const FString& Parameters)
{
	constexpr int32 WarmupIterations = 64;
	constexpr int32 SampleCount = 12;
	constexpr int32 IterationsPerSample = 256;
	constexpr double CatastrophicP95Microseconds = 5000.0;

	UAvidScriptBindingsTestObject* Source =
		NewObject<UAvidScriptBindingsTestObject>(GetTransientPackage());
	UAvidScriptBindingsTestObject* Destination =
		NewObject<UAvidScriptBindingsTestObject>(GetTransientPackage());
	if (!TestNotNull(TEXT("Composite benchmark source exists"), Source)
		|| !TestNotNull(TEXT("Composite benchmark destination exists"), Destination))
	{
		return false;
	}

	Source->CompositeTextProperty = FText::FromString(TEXT("AvidScript localized benchmark text"));
	for (int32 Index = 0; Index < 32; ++Index)
	{
		Source->CompositeStringArrayProperty.Add(FString::Printf(TEXT("array-%02d"), Index));
		FAvidScriptBindingsNestedStruct& StructValue =
			Source->CompositeStructArrayProperty.AddDefaulted_GetRef();
		StructValue.Count = Index;
		StructValue.Ratio = static_cast<float>(Index) * 0.25f;
		Source->CompositeStringSetProperty.Add(FString::Printf(TEXT("set-%02d"), Index));
		Source->CompositeNameStringMapProperty.Add(
			FName(*FString::Printf(TEXT("Key%02d"), Index)),
			FString::Printf(TEXT("map-value-%02d"), Index));
	}

	const FAvidScriptCompositeBenchmarkCase Cases[] = {
		{ TEXT("ftext"), TEXT("CompositeTextProperty"), TEXT("benchmark:text"), EAvidScriptCompositeValueKind::Text },
		{ TEXT("string_array"), TEXT("CompositeStringArrayProperty"), TEXT("benchmark:array:string"), EAvidScriptCompositeValueKind::Array },
		{ TEXT("struct_array"), TEXT("CompositeStructArrayProperty"), TEXT("benchmark:array:struct"), EAvidScriptCompositeValueKind::Array },
		{ TEXT("string_set"), TEXT("CompositeStringSetProperty"), TEXT("benchmark:set:string"), EAvidScriptCompositeValueKind::Set },
		{ TEXT("name_string_map"), TEXT("CompositeNameStringMapProperty"), TEXT("benchmark:map:name-string"), EAvidScriptCompositeValueKind::Map }
	};

	FAvidScriptCompositeValueHeap Heap;
	FString Error;
	for (const FAvidScriptCompositeBenchmarkCase& Case : Cases)
	{
		FProperty* Property = FindFProperty<FProperty>(
			UAvidScriptBindingsTestObject::StaticClass(),
			Case.PropertyName);
		if (!TestNotNull(
				*FString::Printf(TEXT("%s benchmark property exists"), Case.Label),
				Property))
		{
			continue;
		}

		const void* SourceValue = Property->ContainerPtrToValuePtr<void>(Source);
		void* DestinationValue = Property->ContainerPtrToValuePtr<void>(Destination);
		for (int32 Index = 0; Index < WarmupIterations; ++Index)
		{
			if (!RunAvidScriptCompositeBenchmarkIteration(
					Heap,
					*Property,
					Case.TypeId,
					Case.Kind,
					SourceValue,
					DestinationValue,
					Error))
			{
				AddError(Error);
				return false;
			}
		}

		TArray<double> SampleMicroseconds;
		SampleMicroseconds.Reserve(SampleCount);
		for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
		{
			const uint64 StartCycles = FPlatformTime::Cycles64();
			for (int32 Index = 0; Index < IterationsPerSample; ++Index)
			{
				if (!RunAvidScriptCompositeBenchmarkIteration(
						Heap,
						*Property,
						Case.TypeId,
						Case.Kind,
						SourceValue,
						DestinationValue,
						Error))
				{
					AddError(Error);
					return false;
				}
			}
			const double ElapsedSeconds = FPlatformTime::ToSeconds64(
				FPlatformTime::Cycles64() - StartCycles);
			SampleMicroseconds.Add(
				ElapsedSeconds * 1000000.0 / static_cast<double>(IterationsPerSample));
		}

		SampleMicroseconds.Sort();
		double SumMicroseconds = 0.0;
		for (const double Value : SampleMicroseconds)
		{
			SumMicroseconds += Value;
		}
		const double AverageMicroseconds = SumMicroseconds / SampleMicroseconds.Num();
		const double P50Microseconds = SampleMicroseconds[SampleMicroseconds.Num() / 2];
		const int32 P95Index = FMath::Min(
			SampleMicroseconds.Num() - 1,
			FMath::CeilToInt(static_cast<double>(SampleMicroseconds.Num()) * 0.95) - 1);
		const double P95Microseconds = SampleMicroseconds[P95Index];
		const FAvidScriptCompositeValueHeapStats Stats = Heap.GetStats();
		AddInfo(FString::Printf(
			TEXT("p58_composite_value_benchmark case=%s samples=%d iterations=%d avg_us=%.3f p50_us=%.3f p95_us=%.3f peak_live_bytes=%llu"),
			Case.Label,
			SampleCount,
			IterationsPerSample,
			AverageMicroseconds,
			P50Microseconds,
			P95Microseconds,
			Stats.PeakLiveBytes));
		TestTrue(
			*FString::Printf(TEXT("%s benchmark records positive latency"), Case.Label),
			AverageMicroseconds > 0.0 && P50Microseconds > 0.0 && P95Microseconds > 0.0);
		TestTrue(
			*FString::Printf(TEXT("%s benchmark avoids catastrophic latency"), Case.Label),
			P95Microseconds < CatastrophicP95Microseconds);
		TestEqual(
			*FString::Printf(TEXT("%s benchmark leaves no live values"), Case.Label),
			Stats.LiveValues,
			0);
		TestEqual(
			*FString::Printf(TEXT("%s benchmark leaves no reservations"), Case.Label),
			Stats.ReservedValues,
			0);
		TestEqual(
			*FString::Printf(TEXT("%s benchmark leaves no live bytes"), Case.Label),
			Stats.LiveBytes,
			static_cast<uint64>(0));
	}

	const FAvidScriptCompositeValueHeapStats FinalStats = Heap.GetStats();
	TestEqual(
		TEXT("Composite benchmark balances every published value"),
		FinalStats.PublishedValues,
		FinalStats.ReleasedValues);
	return true;
}

#endif
