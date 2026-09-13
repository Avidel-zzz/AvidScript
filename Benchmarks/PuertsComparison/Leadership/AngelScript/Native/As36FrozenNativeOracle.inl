// Extracted unchanged from the frozen 8c032e9f native benchmark oracle.
// Included inside the validation commandlet anonymous namespace.
constexpr uint32 PerfRunnerMixMultiplier = 1664525u;
constexpr uint32 PerfRunnerMixIncrement = 1013904223u;
constexpr float PerfRunnerTickDeltaSeconds = 1.0f / 60.0f;
uint32 PerfRunnerMix(const uint32 Value) { return Value * PerfRunnerMixMultiplier + PerfRunnerMixIncrement; }

	uint32 PackVectorRefOutResult(
		const FVector& InOutValue,
		const FVector& OutValue)
	{
		return static_cast<uint32>(
			static_cast<int32>(InOutValue.X) +
			static_cast<int32>(InOutValue.Y) * 37 +
			static_cast<int32>(InOutValue.Z) * 101 +
			static_cast<int32>(OutValue.X) * 257 +
			static_cast<int32>(OutValue.Y) * 521 +
			static_cast<int32>(OutValue.Z) * 1031);
	}

	uint32 RunNativeWorkload(
		AAvidScriptPerfFixture& Fixture,
		const EAvidScriptPerfWorkload Workload,
		const int32 Iterations,
		const uint32 Seed)
	{
		uint32 Accumulator = Seed;
		for (int32 Index = 0; Index < Iterations; ++Index)
		{
			switch (Workload)
			{
			case EAvidScriptPerfWorkload::PureInteger:
				Accumulator = PerfRunnerMix(Accumulator ^ static_cast<uint32>(Index));
				break;
			case EAvidScriptPerfWorkload::ScalarNoOp:
				Accumulator = PerfRunnerMix(static_cast<uint32>(Fixture.NativeNoOp(static_cast<int32>(Accumulator))) ^
					static_cast<uint32>(Index));
				break;
			case EAvidScriptPerfWorkload::ScalarAddInt32:
				Accumulator = PerfRunnerMix(static_cast<uint32>(Fixture.NativeAddInt32(
					static_cast<int32>(Accumulator),
					Index)));
				break;
			case EAvidScriptPerfWorkload::PropertyGetSet:
				Fixture.ScalarValue = static_cast<int32>(Accumulator ^ static_cast<uint32>(Index));
				Accumulator = PerfRunnerMix(static_cast<uint32>(Fixture.ScalarValue));
				break;
			case EAvidScriptPerfWorkload::VectorValue:
			{
				const FVector Value(
					static_cast<double>(Index & 31),
					static_cast<double>((Index * 3) & 31),
					static_cast<double>((Index * 7) & 31));
				const FVector Result = Fixture.NativeVectorValue(Value);
				const uint32 Packed = static_cast<uint32>(Result.X) ^
					(static_cast<uint32>(Result.Y) << 8) ^
					(static_cast<uint32>(Result.Z) << 16);
				Accumulator = PerfRunnerMix(Accumulator ^ Packed);
				break;
			}
			case EAvidScriptPerfWorkload::ObjectRoundtrip:
				Accumulator = PerfRunnerMix(Accumulator ^
					(Fixture.NativeObjectRoundtrip(&Fixture) == &Fixture ? static_cast<uint32>(Index) : 0xffffffffu));
				break;
			case EAvidScriptPerfWorkload::BatchScalar:
				Accumulator = PerfRunnerMix(static_cast<uint32>(Fixture.NativeBatchAdd(
					static_cast<int32>(Accumulator),
					8)));
				break;
			case EAvidScriptPerfWorkload::CallbackEmpty:
				Fixture.NativeEmptyCallback(Index);
				Accumulator = static_cast<uint32>(
					Fixture.GetNativeCallbackChecksum());
				break;
			case EAvidScriptPerfWorkload::CallbackTick:
				Fixture.NativeTickCallback(PerfRunnerTickDeltaSeconds);
				Accumulator = static_cast<uint32>(
					Fixture.GetNativeCallbackChecksum());
				break;
			case EAvidScriptPerfWorkload::VectorRefOut:
			{
				FVector InOutValue(
					static_cast<double>(Index & 31),
					static_cast<double>((Index * 3) & 31),
					static_cast<double>((Index * 7) & 31));
				FVector OutValue = FVector::ZeroVector;
				Fixture.NativeVectorRefOut(InOutValue, OutValue);
				Accumulator = PerfRunnerMix(
					Accumulator ^
					PackVectorRefOutResult(InOutValue, OutValue));
				break;
			}
			case EAvidScriptPerfWorkload::GameplayFrameSmall:
			case EAvidScriptPerfWorkload::GameplayFrameDense:
				return FAvidScriptGameplayFrameBenchmark::RunNative(
					Fixture,
					Workload,
					Iterations,
					Seed);
			default:
				checkNoEntry();
				break;
			}
		}
		return Accumulator;
	}
