using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace AvidScript
{
    public static class AvidContinuations
    {
        public static async Task NextTickAsync() => await Task.Yield();
    }
}

internal sealed class ReferenceTickContext : SynchronizationContext
{
    private readonly Queue<(SendOrPostCallback Callback, object? State)> pending = new();

    public override void Post(SendOrPostCallback callback, object? state) =>
        pending.Enqueue((callback, state));

    public void DispatchNext()
    {
        if (pending.Count == 0) throw new InvalidOperationException("The reference task stalled.");
        (SendOrPostCallback callback, object? state) = pending.Dequeue();
        callback(state);
    }
}

internal static class Program
{
    private static void Main()
    {
        RunCase(0, "12", 1);
        RunCase(1, "7", 1);
        RunCase(2, nameof(ArgumentException), 1);
        RunCase(3, nameof(ArgumentException), 1);
        RunCase(4, nameof(InvalidOperationException), 1);
        RunNestedCase();
        Console.WriteLine("AsyncExceptionFlow.Reference: 6/6 passed");
    }

    private static void RunCase(int mode, string expected, int expectedCleanups) =>
        Run($"mode={mode}", () => Script.RunAsync(mode), expected, expectedCleanups);

    private static void RunNestedCase() =>
        Run("nested", Script.RunNestedAsync, "7", 11);

    private static void Run(string label, Func<Task<int>> start,
        string expected, int expectedCleanups)
    {
        SynchronizationContext? previous = SynchronizationContext.Current;
        ReferenceTickContext ticks = new();
        Script.CleanupCount = 0;
        SynchronizationContext.SetSynchronizationContext(ticks);
        try
        {
            Task<int> task = start();
            while (!task.IsCompleted) ticks.DispatchNext();
            string outcome;
            try
            {
                outcome = task.GetAwaiter().GetResult().ToString();
            }
            catch (Exception error)
            {
                outcome = error.GetType().Name;
            }
            if (outcome != expected || Script.CleanupCount != expectedCleanups)
            {
                throw new InvalidOperationException(
                    $"{label}: expected {expected}/{expectedCleanups}, got {outcome}/{Script.CleanupCount}.");
            }
            Console.WriteLine($"{label}: {outcome}, cleanups={Script.CleanupCount}");
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }
}
