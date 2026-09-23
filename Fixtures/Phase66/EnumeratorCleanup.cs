using System;
using System.Runtime.InteropServices;

public sealed class CounterCollection
{
    public CounterEnumerator GetEnumerator()
    {
        Script.AcquiredCount++;
        return new CounterEnumerator();
    }
}

public sealed class CounterEnumerator : IDisposable
{
    private int index;

    public int Current => index * 2;

    public bool MoveNext()
    {
        index++;
        return index <= 3;
    }

    public void Dispose() => Script.DisposeCount++;
}

public static class Script
{
    public static int AcquiredCount;
    public static int DisposeCount;
    public static int RuntimeNormal;
    public static int RuntimeBreakContinue;
    public static int RuntimeEarlyReturn;
    public static int RuntimeCounts;

    private static CounterCollection Create() => new CounterCollection();

    private static int NormalCore()
    {
        int sum = 0;
        foreach (int value in Create()) sum += value;
        return sum;
    }

    private static int BreakContinueCore()
    {
        int sum = 0;
        foreach (int value in Create())
        {
            if (value == 4) continue;
            if (value == 6) break;
            sum += value;
        }
        return sum;
    }

    private static int EarlyReturnCore()
    {
        foreach (int value in Create()) return DisposeCount * 10 + value;
        return -1;
    }

    [UnmanagedCallersOnly(EntryPoint = "enumerator_normal")]
    public static int Normal() => NormalCore();

    [UnmanagedCallersOnly(EntryPoint = "enumerator_break_continue")]
    public static int BreakContinue() => BreakContinueCore();

    [UnmanagedCallersOnly(EntryPoint = "enumerator_early_return")]
    public static int EarlyReturn() => EarlyReturnCore();

    [UnmanagedCallersOnly(EntryPoint = "enumerator_counts")]
    public static int Counts() => AcquiredCount * 10 + DisposeCount;

    [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
    public static void BeginPlay()
    {
        RuntimeNormal = NormalCore();
        RuntimeBreakContinue = BreakContinueCore();
        RuntimeEarlyReturn = EarlyReturnCore();
        RuntimeCounts = AcquiredCount * 10 + DisposeCount;
    }
}
