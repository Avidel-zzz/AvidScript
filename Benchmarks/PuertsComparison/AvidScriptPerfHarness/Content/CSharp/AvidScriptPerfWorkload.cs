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
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void Run(int packedWorkload, float seedValue)
    {
        AAvidScriptPerfFixture fixture = UE.Self;
        int workload = (packedWorkload >> WorkloadShift) & 0x7f;
        int iterations = packedWorkload & IterationMask;
        int accumulator = (int)seedValue;

        if (workload == 0)
        {
            accumulator = RunPureInteger(iterations, accumulator);
        }
        else if (workload == 1)
        {
            accumulator = RunScalarNoOp(fixture, iterations, accumulator);
        }
        else if (workload == 2)
        {
            accumulator = RunScalarAdd(fixture, iterations, accumulator);
        }
        else if (workload == 3)
        {
            accumulator = RunProperty(fixture, iterations, accumulator);
        }
        else if (workload == 4)
        {
            accumulator = RunVectorValue(fixture, iterations, accumulator);
        }
        else if (workload == 5)
        {
            accumulator = RunObjectRoundtrip(fixture, iterations, accumulator);
        }
        else if (workload == 6)
        {
            accumulator = RunBatchScalar(fixture, iterations, accumulator);
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

    [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
    public static void EndPlay()
    {
    }

    private static int Mix(int value)
    {
        return value * MixMultiplier + MixIncrement;
    }
}
