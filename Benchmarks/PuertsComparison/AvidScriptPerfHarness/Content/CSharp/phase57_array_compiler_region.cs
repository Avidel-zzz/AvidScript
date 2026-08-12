namespace AvidScript;

public static class Phase57ArrayCompilerRegion
{
    private const int MixMultiplier = 1664525;
    private const int MixIncrement = 1013904223;
    private const int FnvOffset = -2128831035;
    private const int FnvPrime = 16777619;

    public static int FullHash;

    [AvidExport("avid_on_begin_play")]
    public static void BeginPlay()
    {
    }

    [AvidExport("avid_on_tick")]
    public static void Tick(float deltaSeconds)
    {
    }

    [AvidExport("avid_on_end_play")]
    public static void EndPlay()
    {
    }

    [AvidExport("avid_on_event")]
    public static void OnEvent(int eventId, float value)
    {
    }

    [AvidExport("phase57_array_run")]
    public static void Run(int[] values, int logicalCalls)
    {
        int size = values.Length;
        for (int call = 0; call < logicalCalls; ++call)
        {
            int hash = FnvOffset;
            for (int index = 0; index < size; ++index)
            {
                int value = ((values[index] ^ index) * MixMultiplier) + MixIncrement;
                values[index] = value;
                hash = (hash ^ value) * FnvPrime;
            }
            FullHash = hash;
        }
    }
}
