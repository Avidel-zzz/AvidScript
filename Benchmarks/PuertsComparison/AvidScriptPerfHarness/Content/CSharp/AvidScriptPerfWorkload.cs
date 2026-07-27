using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class AvidScriptPerfWorkload
{
    private const int MixMultiplier = 1664525;
    private const int MixIncrement = 1013904223;
    private const int WorkloadShift = 24;
    private const int IterationMask = 0x00ffffff;

    [AvidPersist]
    private static int ResultChecksum;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        ResultChecksum = 0;
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
        ResultChecksum = Mix(ResultChecksum ^ 1);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void Run(int packedWorkload, float seedValue)
    {
        int workload = (packedWorkload >> WorkloadShift) & 0x7f;
        int iterations = packedWorkload & IterationMask;
        if (workload == 7)
        {
            ResultChecksum = Mix(ResultChecksum ^ (int)seedValue);
            return;
        }
        if (workload == 8)
        {
            ResultChecksum = (int)seedValue;
            return;
        }

        int accumulator = (int)seedValue;

        if (workload == 0)
        {
            accumulator = RunPureInteger(iterations, accumulator);
        }
        else if (workload == 1)
        {
            AAvidScriptPerfFixture fixture = UE.Self;
            accumulator = RunScalarNoOp(fixture, iterations, accumulator);
        }
        else if (workload == 2)
        {
            AAvidScriptPerfFixture fixture = UE.Self;
            accumulator = RunScalarAdd(fixture, iterations, accumulator);
        }
        else if (workload == 3)
        {
            AAvidScriptPerfFixture fixture = UE.Self;
            accumulator = RunProperty(fixture, iterations, accumulator);
        }
        else if (workload == 4)
        {
            AAvidScriptPerfFixture fixture = UE.Self;
            accumulator = RunVectorValue(fixture, iterations, accumulator);
        }
        else if (workload == 5)
        {
            AAvidScriptPerfFixture fixture = UE.Self;
            accumulator = RunObjectRoundtrip(fixture, iterations, accumulator);
        }
        else if (workload == 6)
        {
            AAvidScriptPerfFixture fixture = UE.Self;
            accumulator = RunBatchScalar(fixture, iterations, accumulator);
        }
        else if (workload == 9)
        {
            AAvidScriptPerfFixture fixture = UE.Self;
            accumulator = RunVectorRefOut(fixture, iterations, accumulator);
        }
        else if (workload == 10 || workload == 11)
        {
            AAvidScriptPerfFixture fixture = UE.Self;
            accumulator = RunGameplayFrames(
                fixture,
                workload,
                iterations,
                accumulator);
        }
        else if (workload == 12 || workload == 13)
        {
            AAvidScriptPerfFixture fixture = UE.Self;
            accumulator = RunGameplayDataFrames(
                fixture,
                workload - 2,
                iterations,
                accumulator);
        }
        else
        {
            accumulator = Mix(accumulator ^ -1);
        }

        ResultChecksum = accumulator;
    }

    private static int RunPureInteger(int iterations, int accumulator)
    {
        for (int index = 0; index < iterations; ++index)
        {
            accumulator = Mix(accumulator ^ index);
        }
        return accumulator;
    }

    private static int RunScalarNoOp(
        AAvidScriptPerfFixture fixture,
        int iterations,
        int accumulator)
    {
        for (int index = 0; index < iterations; ++index)
        {
            accumulator = Mix(fixture.ReflectNoOp(accumulator) ^ index);
        }
        return accumulator;
    }

    private static int RunScalarAdd(
        AAvidScriptPerfFixture fixture,
        int iterations,
        int accumulator)
    {
        for (int index = 0; index < iterations; ++index)
        {
            accumulator = Mix(fixture.ReflectAddInt32(accumulator, index));
        }
        return accumulator;
    }

    private static int RunProperty(
        AAvidScriptPerfFixture fixture,
        int iterations,
        int accumulator)
    {
        for (int index = 0; index < iterations; ++index)
        {
            fixture.ScalarValue = accumulator ^ index;
            accumulator = Mix(fixture.ScalarValue);
        }
        return accumulator;
    }

    private static int RunVectorValue(
        AAvidScriptPerfFixture fixture,
        int iterations,
        int accumulator)
    {
        for (int index = 0; index < iterations; ++index)
        {
            FVector value = new FVector(
                index & 31,
                (index * 3) & 31,
                (index * 7) & 31);
            FVector result = fixture.ReflectVectorValue(value);
            int packed = (int)result.X ^
                ((int)result.Y << 8) ^
                ((int)result.Z << 16);
            accumulator = Mix(accumulator ^ packed);
        }
        return accumulator;
    }

    private static int RunObjectRoundtrip(
        AAvidScriptPerfFixture fixture,
        int iterations,
        int accumulator)
    {
        AActor expectedActor = fixture;
        UObject expected = expectedActor;
        for (int index = 0; index < iterations; ++index)
        {
            UObject result = fixture.ReflectObjectRoundtrip(expected);
            accumulator = Mix(
                accumulator ^
                (result.AvidScriptSlot == expected.AvidScriptSlot ? index : -1));
        }
        return accumulator;
    }

    private static int RunVectorRefOut(
        AAvidScriptPerfFixture fixture,
        int iterations,
        int accumulator)
    {
        for (int index = 0; index < iterations; ++index)
        {
            FVector inOutValue = new FVector(
                index & 31,
                (index * 3) & 31,
                (index * 7) & 31);
            FVector outValue;
            fixture.ReflectVectorRefOut(ref inOutValue, out outValue);
            int packed = (int)inOutValue.X +
                (int)inOutValue.Y * 37 +
                (int)inOutValue.Z * 101 +
                (int)outValue.X * 257 +
                (int)outValue.Y * 521 +
                (int)outValue.Z * 1031;
            accumulator = Mix(accumulator ^ packed);
        }
        return accumulator;
    }

    private static int RunBatchScalar(
        AAvidScriptPerfFixture fixture,
        int iterations,
        int accumulator)
    {
        for (int index = 0; index < iterations; ++index)
        {
            accumulator = Mix(fixture.ReflectBatchAdd(accumulator, 8));
        }
        return accumulator;
    }

    private static int MakeGameplayToken(
        int seed,
        int frame,
        int entity,
        int operation)
    {
        return Mix(seed ^ (frame * 131 + entity * 17 + operation));
    }

    private static int RunGameplayScalar(
        AAvidScriptPerfFixture fixture,
        int accumulator,
        int token)
    {
        return Mix(fixture.ReflectAddInt32(accumulator, token));
    }

    private static int RunGameplayProperty(
        AAvidScriptPerfFixture fixture,
        int accumulator,
        int token)
    {
        int value = accumulator ^ token;
        fixture.ScalarValue = value;
        return Mix(value ^ token);
    }

    private static int RunGameplayPropertyBatch4(
        AAvidScriptPerfFixture fixture,
        int accumulator,
        int token0,
        int token1,
        int token2,
        int token3)
    {
        int value0 = accumulator ^ token0;
        int accumulator0 = Mix(value0 ^ token0);
        int value1 = accumulator0 ^ token1;
        int accumulator1 = Mix(value1 ^ token1);
        int value2 = accumulator1 ^ token2;
        int accumulator2 = Mix(value2 ^ token2);
        int value3 = accumulator2 ^ token3;
        int accumulator3 = Mix(value3 ^ token3);
        fixture.ScalarValue = value0;
        fixture.ScalarValue = value1;
        fixture.ScalarValue = value2;
        fixture.ScalarValue = value3;
        return accumulator3;
    }

    private static int RunGameplayVector(
        AAvidScriptPerfFixture fixture,
        int accumulator,
        int token)
    {
        FVector value = new FVector(
            token & 31,
            (token >> 5) & 31,
            (token >> 10) & 31);
        FVector result = fixture.ReflectVectorValue(value);
        int packed = (int)result.X ^
            ((int)result.Y << 8) ^
            ((int)result.Z << 16);
        return Mix(accumulator ^ packed);
    }

    private static int RunGameplayObject(
        AAvidScriptPerfFixture fixture,
        int accumulator,
        int token)
    {
        AActor expectedActor = fixture;
        UObject expected = expectedActor;
        UObject result = fixture.ReflectObjectRoundtrip(expected);
        return Mix(
            accumulator ^
            (result.AvidScriptSlot == expected.AvidScriptSlot ? token : ~token));
    }

    private static int RunGameplayEvent(
        AAvidScriptPerfFixture fixture,
        int accumulator,
        int token)
    {
        return Mix(fixture.ReflectEventStep(accumulator, token));
    }

    private static int RunGameplayFrames(
        AAvidScriptPerfFixture fixture,
        int workload,
        int frames,
        int seed)
    {
        int accumulator = seed;
        for (int frame = 0; frame < frames; ++frame)
        {
            if (workload == 10)
            {
                accumulator = RunGameplaySmallFrame(
                    fixture,
                    frame,
                    seed,
                    accumulator,
                    false);
            }
            else
            {
                accumulator = RunGameplayDenseFrame(
                    fixture,
                    frame,
                    seed,
                    accumulator,
                    false);
            }
        }
        return accumulator;
    }

    private static int RunGameplayDataFrames(
        AAvidScriptPerfFixture fixture,
        int workload,
        int frames,
        int seed)
    {
        int accumulator = seed;
        for (int frame = 0; frame < frames; ++frame)
        {
            if (workload == 10)
            {
                accumulator = RunGameplaySmallFrame(
                    fixture,
                    frame,
                    seed,
                    accumulator,
                    true);
            }
            else
            {
                accumulator = RunGameplayDenseFrame(
                    fixture,
                    frame,
                    seed,
                    accumulator,
                    true);
            }
        }
        return accumulator;
    }

    private static int RunGameplaySmallFrame(
        AAvidScriptPerfFixture fixture,
        int frame,
        int seed,
        int accumulator,
        bool useDataBatch)
    {
        int operation = 0;
        for (int index = 0; index < 32; ++index)
        {
            accumulator = RunGameplayScalar(
                fixture,
                accumulator,
                MakeGameplayToken(seed, frame, 0, operation++));
        }
        for (int batch = 0; batch < 8; ++batch)
        {
            int token0 = MakeGameplayToken(seed, frame, 0, operation++);
            int token1 = MakeGameplayToken(seed, frame, 0, operation++);
            int token2 = MakeGameplayToken(seed, frame, 0, operation++);
            int token3 = MakeGameplayToken(seed, frame, 0, operation++);
            if (useDataBatch)
            {
                accumulator = RunGameplayPropertyBatch4(
                    fixture,
                    accumulator,
                    token0,
                    token1,
                    token2,
                    token3);
            }
            else
            {
                accumulator = RunGameplayProperty(fixture, accumulator, token0);
                accumulator = RunGameplayProperty(fixture, accumulator, token1);
                accumulator = RunGameplayProperty(fixture, accumulator, token2);
                accumulator = RunGameplayProperty(fixture, accumulator, token3);
            }
        }
        for (int index = 0; index < 8; ++index)
        {
            accumulator = RunGameplayVector(
                fixture,
                accumulator,
                MakeGameplayToken(seed, frame, 0, operation++));
        }
        for (int index = 0; index < 4; ++index)
        {
            accumulator = RunGameplayObject(
                fixture,
                accumulator,
                MakeGameplayToken(seed, frame, 0, operation++));
        }
        for (int index = 0; index < 2; ++index)
        {
            int token = MakeGameplayToken(seed, frame, 0, operation++);
            accumulator = RunGameplayEvent(
                fixture,
                accumulator,
                token);
        }
        return accumulator;
    }

    private static int RunGameplayDenseFrame(
        AAvidScriptPerfFixture fixture,
        int frame,
        int seed,
        int accumulator,
        bool useDataBatch)
    {
        for (int entity = 0; entity < 1024; ++entity)
        {
            int operation = 0;
            for (int index = 0; index < 4; ++index)
            {
                accumulator = RunGameplayScalar(
                    fixture,
                    accumulator,
                    MakeGameplayToken(seed, frame, entity, operation++));
            }
            int token0 = MakeGameplayToken(seed, frame, entity, operation++);
            int token1 = MakeGameplayToken(seed, frame, entity, operation++);
            int token2 = MakeGameplayToken(seed, frame, entity, operation++);
            int token3 = MakeGameplayToken(seed, frame, entity, operation++);
            if (useDataBatch)
            {
                accumulator = RunGameplayPropertyBatch4(
                    fixture,
                    accumulator,
                    token0,
                    token1,
                    token2,
                    token3);
            }
            else
            {
                accumulator = RunGameplayProperty(fixture, accumulator, token0);
                accumulator = RunGameplayProperty(fixture, accumulator, token1);
                accumulator = RunGameplayProperty(fixture, accumulator, token2);
                accumulator = RunGameplayProperty(fixture, accumulator, token3);
            }
            for (int index = 0; index < 2; ++index)
            {
                accumulator = RunGameplayVector(
                    fixture,
                    accumulator,
                    MakeGameplayToken(seed, frame, entity, operation++));
            }
            int token = MakeGameplayToken(seed, frame, entity, operation);
            accumulator = (entity & 1) == 0
                ? RunGameplayObject(fixture, accumulator, token)
                : RunGameplayEvent(fixture, accumulator, token);
        }
        return accumulator;
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
    }

    private static int Mix(int value)
    {
        return value * MixMultiplier + MixIncrement;
    }
}
