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
        RunCase(0, "16");
        RunCase(1, "17");
        RunCase(2, "17");
        RunCase(4, nameof(Exception));
        Console.WriteLine("IntegratedLanguageFlow.Reference: 4/4 passed");
    }

    private static void RunCase(int mode, string expected)
    {
        SynchronizationContext? previous = SynchronizationContext.Current;
        ReferenceTickContext ticks = new();
        Script.CleanupCount = 0;
        SynchronizationContext.SetSynchronizationContext(ticks);
        try
        {
            Task<int> task = Script.RunAsync(mode);
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
            if (outcome != expected || Script.CleanupCount != 1)
            {
                throw new InvalidOperationException(
                    $"mode={mode}: expected {expected}/1, got {outcome}/{Script.CleanupCount}.");
            }
            Console.WriteLine($"mode={mode}: {outcome}, cleanups={Script.CleanupCount}");
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }
}
