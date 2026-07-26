using System.Runtime.InteropServices;

namespace AvidScript;

[AvidStateContract(AvidStateMode.Explicit)]
public static class AvidScriptPerfWorkload
{
    private const int MixMultiplier = 1664525;
    private const int MixIncrement = 1013904223;
    private const int WorkloadShift = 24;
    private const int IterationMask = 0x00ffffff;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        UE.Self.ReflectSetScalar(0);
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
    }

    [UnmanagedCallersOnly(EntryPoint = "avid_on_event")]
    public static void Run(int packedWorkload, float seedValue)
    {
        UAvidScriptPerfFixture fixture = UE.Self;
        int workload = (packedWorkload >> WorkloadShift) & 0x7f;
        int iterations = packedWorkload & IterationMask;
        int accumulator = (int)seedValue;

        for (int index = 0; index < iterations; ++index)
        {
            switch (workload)
            {
                case 0:
                    accumulator = Mix(accumulator ^ index);
                    break;
                case 1:
                    accumulator = Mix(fixture.ReflectNoOp(accumulator) ^ index);
                    break;
                case 2:
                    accumulator = Mix(fixture.ReflectAddInt32(accumulator, index));
                    break;
                case 3:
                    fixture.ReflectSetScalar(accumulator ^ index);
                    accumulator = Mix(fixture.ReflectGetScalar());
                    break;
                case 4:
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
                    break;
                }
                case 5:
                {
                    UObject expected = fixture;
                    UObject result = fixture.ReflectObjectRoundtrip(expected);
                    accumulator = Mix(
                        accumulator ^
                        (result.AvidScriptSlot == expected.AvidScriptSlot ? index : -1));
                    break;
                }
                case 6:
                    accumulator = Mix(fixture.ReflectBatchAdd(accumulator, 8));
                    break;
                default:
                    accumulator = Mix(accumulator ^ -1);
                    break;
            }
        }

        fixture.ReflectSetScalar(accumulator);
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
