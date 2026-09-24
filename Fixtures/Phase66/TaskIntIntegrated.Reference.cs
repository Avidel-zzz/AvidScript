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
        SynchronizationContext? previous = SynchronizationContext.Current;
        ReferenceTickContext ticks = new();
        SynchronizationContext.SetSynchronizationContext(ticks);
        try
        {
            Task<int> task = Script.RunScenarioAsync();
            while (!task.IsCompleted) ticks.DispatchNext();
            Console.WriteLine($"result={task.GetAwaiter().GetResult()}; cleanups={Script.Cleanups}");
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(previous);
        }
    }
}
