using System;

internal static unsafe class Program
{
    private static void Main()
    {
        delegate* unmanaged<int> normal = &Script.Normal;
        delegate* unmanaged<int> breakContinue = &Script.BreakContinue;
        delegate* unmanaged<int> earlyReturn = &Script.EarlyReturn;
        delegate* unmanaged<int> counts = &Script.Counts;
        Console.WriteLine($"enumerator_normal: {normal()}");
        Console.WriteLine($"enumerator_break_continue: {breakContinue()}");
        Console.WriteLine($"enumerator_early_return: {earlyReturn()}");
        Console.WriteLine($"enumerator_counts: {counts()}");
    }
}
