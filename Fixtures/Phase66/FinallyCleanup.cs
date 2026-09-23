using System.Runtime.InteropServices;

public static class Script
{
    private static int CleanupCount;
    public static int RuntimeNormal;
    public static int RuntimeEarlyReturn;
    public static int RuntimeNestedReturn;
    public static int RuntimeCleanupCount;
    public static int RuntimeLoopExits;

    private static int NormalCore()
    {
        int value = 1;
        try { value += 2; }
        finally { value += 4; }
        return value;
    }

    private static int EarlyReturnCore()
    {
        try { return CleanupCount + 11; }
        finally { CleanupCount += 5; }
    }

    private static int NestedReturnCore()
    {
        try
        {
            try { return 13; }
            finally { CleanupCount += 2; }
        }
        finally { CleanupCount *= 3; }
    }

    private static int LoopExitsCore()
    {
        int sum = 0;
        for (int index = 0; index < 4; ++index)
        {
            try
            {
                if (index == 1) continue;
                if (index == 3) break;
                sum += index;
            }
            finally { sum += 10; }
        }
        return sum;
    }

    [UnmanagedCallersOnly(EntryPoint = "finally_normal")]
    public static int Normal() => NormalCore();

    [UnmanagedCallersOnly(EntryPoint = "finally_early_return")]
    public static int EarlyReturn() => EarlyReturnCore();

    [UnmanagedCallersOnly(EntryPoint = "finally_nested_return")]
    public static int NestedReturn() => NestedReturnCore();

    [UnmanagedCallersOnly(EntryPoint = "finally_cleanup_count")]
    public static int ReadCleanupCount() => CleanupCount;

    [UnmanagedCallersOnly(EntryPoint = "finally_loop_exits")]
    public static int LoopExits() => LoopExitsCore();

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        RuntimeNormal = NormalCore();
        RuntimeEarlyReturn = EarlyReturnCore();
        RuntimeNestedReturn = NestedReturnCore();
        RuntimeCleanupCount = CleanupCount;
        RuntimeLoopExits = LoopExitsCore();
    }
}
