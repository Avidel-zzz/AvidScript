using System;

internal static unsafe class Program
{
    private static void Main()
    {
        delegate* unmanaged<int> normal = &Script.Normal;
        delegate* unmanaged<int> earlyReturn = &Script.EarlyReturn;
        delegate* unmanaged<int> nestedReturn = &Script.NestedReturn;
        delegate* unmanaged<int> cleanupCount = &Script.ReadCleanupCount;
        delegate* unmanaged<int> loopExits = &Script.LoopExits;
        delegate* unmanaged<float> convertedReturn = &Script.ConvertedReturn;
        Console.WriteLine($"finally_normal: {normal()}");
        Console.WriteLine($"finally_early_return: {earlyReturn()}");
        Console.WriteLine($"finally_nested_return: {nestedReturn()}");
        Console.WriteLine($"finally_cleanup_count: {cleanupCount()}");
        Console.WriteLine($"finally_loop_exits: {loopExits()}");
        Console.WriteLine($"finally_converted_return: {convertedReturn()}");
    }
}
