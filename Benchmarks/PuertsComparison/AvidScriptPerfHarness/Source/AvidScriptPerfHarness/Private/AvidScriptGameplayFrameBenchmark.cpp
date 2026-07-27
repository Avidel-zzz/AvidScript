#include "AvidScriptGameplayFrameBenchmark.h"

#include "AvidScriptPerfFixture.h"

namespace
{
	constexpr uint32 MixMultiplier = 1664525u;
	constexpr uint32 MixIncrement = 1013904223u;
	constexpr int32 SmallScalarCount = 32;
	constexpr int32 SmallPropertyWriteCount = 32;
	constexpr int32 SmallVectorCount = 8;
	constexpr int32 SmallObjectCount = 4;
	constexpr int32 SmallEventCount = 2;
	constexpr int32 DenseEntityCount = 1024;
	constexpr int32 DenseScalarCount = 4;
	constexpr int32 DensePropertyWriteCount = 4;
	constexpr int32 DenseVectorCount = 2;

	uint32 Mix(const uint32 Value)
	{
		return Value * MixMultiplier + MixIncrement;
	}

	uint32 MakeToken(
		const uint32 Seed,
		const int32 Frame,
		const int32 Entity,
		const int32 Operation)
	{
		return Mix(
			Seed ^
			static_cast<uint32>(Frame * 131 + Entity * 17 + Operation));
	}

	uint32 PackVector(const FVector& Value)
	{
		return static_cast<uint32>(Value.X) ^
			(static_cast<uint32>(Value.Y) << 8) ^
			(static_cast<uint32>(Value.Z) << 16);
	}

	uint32 RunScalar(
		AAvidScriptPerfFixture& Fixture,
		const uint32 Accumulator,
		const uint32 Token)
	{
		return Mix(static_cast<uint32>(Fixture.NativeAddInt32(
			static_cast<int32>(Accumulator),
			static_cast<int32>(Token))));
	}

	uint32 AdvanceProperty(
		const uint32 Accumulator,
		const uint32 Token,
		uint32& OutValue)
	{
		OutValue = Accumulator ^ Token;
		return Mix(OutValue ^ Token);
	}

	uint32 RunPropertyBatch4(
		AAvidScriptPerfFixture& Fixture,
		const uint32 Accumulator,
		const uint32 Token0,
		const uint32 Token1,
		const uint32 Token2,
		const uint32 Token3)
	{
		uint32 Value0 = 0;
		uint32 Value1 = 0;
		uint32 Value2 = 0;
		uint32 Value3 = 0;
		const uint32 Accumulator0 =
			AdvanceProperty(Accumulator, Token0, Value0);
		const uint32 Accumulator1 =
			AdvanceProperty(Accumulator0, Token1, Value1);
		const uint32 Accumulator2 =
			AdvanceProperty(Accumulator1, Token2, Value2);
		const uint32 Accumulator3 =
			AdvanceProperty(Accumulator2, Token3, Value3);
		Fixture.NativeSetScalar(static_cast<int32>(Value0));
		Fixture.NativeSetScalar(static_cast<int32>(Value1));
		Fixture.NativeSetScalar(static_cast<int32>(Value2));
		Fixture.NativeSetScalar(static_cast<int32>(Value3));
		return Accumulator3;
	}

	uint32 RunVector(
		AAvidScriptPerfFixture& Fixture,
		const uint32 Accumulator,
		const uint32 Token)
	{
		const FVector Value(
			static_cast<double>(Token & 31u),
			static_cast<double>((Token >> 5) & 31u),
			static_cast<double>((Token >> 10) & 31u));
		return Mix(Accumulator ^ PackVector(Fixture.NativeVectorValue(Value)));
	}

	uint32 RunObject(
		AAvidScriptPerfFixture& Fixture,
		const uint32 Accumulator,
		const uint32 Token)
	{
		const uint32 Result =
			Fixture.NativeObjectRoundtrip(&Fixture) == &Fixture
				? Token
				: ~Token;
		return Mix(Accumulator ^ Result);
	}

	uint32 RunEvent(
		AAvidScriptPerfFixture& Fixture,
		const uint32 Accumulator,
		const uint32 Token)
	{
		return Mix(static_cast<uint32>(Fixture.NativeEventStep(
			static_cast<int32>(Accumulator),
			static_cast<int32>(Token))));
	}

	uint32 RunSmallFrame(
		AAvidScriptPerfFixture& Fixture,
		const int32 Frame,
		const uint32 Seed,
		uint32 Accumulator)
	{
		int32 Operation = 0;
		for (int32 Index = 0; Index < SmallScalarCount; ++Index)
		{
			Accumulator = RunScalar(
				Fixture,
				Accumulator,
				MakeToken(Seed, Frame, 0, Operation++));
		}
		for (int32 Batch = 0; Batch < SmallPropertyWriteCount / 4; ++Batch)
		{
			const uint32 Token0 = MakeToken(Seed, Frame, 0, Operation++);
			const uint32 Token1 = MakeToken(Seed, Frame, 0, Operation++);
			const uint32 Token2 = MakeToken(Seed, Frame, 0, Operation++);
			const uint32 Token3 = MakeToken(Seed, Frame, 0, Operation++);
			Accumulator = RunPropertyBatch4(
				Fixture,
				Accumulator,
				Token0,
				Token1,
				Token2,
				Token3);
		}
		for (int32 Index = 0; Index < SmallVectorCount; ++Index)
		{
			Accumulator = RunVector(
				Fixture,
				Accumulator,
				MakeToken(Seed, Frame, 0, Operation++));
		}
		for (int32 Index = 0; Index < SmallObjectCount; ++Index)
		{
			Accumulator = RunObject(
				Fixture,
				Accumulator,
				MakeToken(Seed, Frame, 0, Operation++));
		}
		for (int32 Index = 0; Index < SmallEventCount; ++Index)
		{
			const uint32 Token = MakeToken(Seed, Frame, 0, Operation++);
			Accumulator = RunEvent(Fixture, Accumulator, Token);
		}
		return Accumulator;
	}

	uint32 RunDenseEntity(
		AAvidScriptPerfFixture& Fixture,
		const int32 Frame,
		const int32 Entity,
		const uint32 Seed,
		uint32 Accumulator)
	{
		int32 Operation = 0;
		for (int32 Index = 0; Index < DenseScalarCount; ++Index)
		{
			Accumulator = RunScalar(
				Fixture,
				Accumulator,
				MakeToken(Seed, Frame, Entity, Operation++));
		}
		const uint32 Token0 = MakeToken(Seed, Frame, Entity, Operation++);
		const uint32 Token1 = MakeToken(Seed, Frame, Entity, Operation++);
		const uint32 Token2 = MakeToken(Seed, Frame, Entity, Operation++);
		const uint32 Token3 = MakeToken(Seed, Frame, Entity, Operation++);
		Accumulator = RunPropertyBatch4(
			Fixture,
			Accumulator,
			Token0,
			Token1,
			Token2,
			Token3);
		for (int32 Index = 0; Index < DenseVectorCount; ++Index)
		{
			Accumulator = RunVector(
				Fixture,
				Accumulator,
				MakeToken(Seed, Frame, Entity, Operation++));
		}
		const uint32 Token = MakeToken(Seed, Frame, Entity, Operation);
		return (Entity & 1) == 0
			? RunObject(Fixture, Accumulator, Token)
			: RunEvent(Fixture, Accumulator, Token);
	}

}

bool FAvidScriptGameplayFrameBenchmark::IsGameplayWorkload(
	const EAvidScriptPerfWorkload Workload)
{
	return Workload == EAvidScriptPerfWorkload::GameplayFrameSmall ||
		Workload == EAvidScriptPerfWorkload::GameplayFrameDense;
}

FAvidScriptGameplayFrameCounts FAvidScriptGameplayFrameBenchmark::GetCounts(
	const EAvidScriptPerfWorkload Workload,
	const int32 Frames)
{
	FAvidScriptGameplayFrameCounts Counts;
	if (Frames <= 0)
	{
		return Counts;
	}
	if (Workload == EAvidScriptPerfWorkload::GameplayFrameSmall)
	{
		Counts.ScalarPropertyCount = static_cast<uint64>(Frames) * 64u;
		Counts.PropertyWriteCount =
			static_cast<uint64>(Frames) * SmallPropertyWriteCount;
		Counts.VectorCount = static_cast<uint64>(Frames) * SmallVectorCount;
		Counts.ObjectCount = static_cast<uint64>(Frames) * SmallObjectCount;
		Counts.EventCount = static_cast<uint64>(Frames) * SmallEventCount;
		Counts.LogicalEntityCount = static_cast<uint64>(Frames);
	}
	else if (Workload == EAvidScriptPerfWorkload::GameplayFrameDense)
	{
		const uint64 Entities =
			static_cast<uint64>(Frames) * DenseEntityCount;
		Counts.ScalarPropertyCount = Entities * 8u;
		Counts.PropertyWriteCount = Entities * DensePropertyWriteCount;
		Counts.VectorCount = Entities * DenseVectorCount;
		Counts.ObjectCount = Entities / 2u;
		Counts.EventCount = Entities / 2u;
		Counts.LogicalEntityCount = Entities;
	}
	Counts.LogicalOperationCount =
		Counts.ScalarPropertyCount +
		Counts.VectorCount +
		Counts.ObjectCount +
		Counts.EventCount;
	return Counts;
}

uint32 FAvidScriptGameplayFrameBenchmark::RunNative(
	AAvidScriptPerfFixture& Fixture,
	const EAvidScriptPerfWorkload Workload,
	const int32 Frames,
	const uint32 Seed)
{
	uint32 Accumulator = Seed;
	for (int32 Frame = 0; Frame < Frames; ++Frame)
	{
		if (Workload == EAvidScriptPerfWorkload::GameplayFrameSmall)
		{
			Accumulator = RunSmallFrame(
				Fixture,
				Frame,
				Seed,
				Accumulator);
			continue;
		}
		for (int32 Entity = 0; Entity < DenseEntityCount; ++Entity)
		{
			Accumulator = RunDenseEntity(
				Fixture,
				Frame,
				Entity,
				Seed,
				Accumulator);
		}
	}
	return Accumulator;
}
