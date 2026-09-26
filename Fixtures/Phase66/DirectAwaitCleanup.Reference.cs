using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace AvidScript
{
    public sealed class AvidCancellationSource
    {
        private readonly CancellationTokenSource source = new();

        public CancellationToken Token => source.Token;
        public static AvidCancellationSource Create() => new();
        public void Cancel() => source.Cancel();
        public void Release() => source.Dispose();
    }

    public static class AvidContinuations
    {
        private static readonly Queue<TaskCompletionSource> pending = new();

        public static Task NextTickAsync()
        {
            TaskCompletionSource completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
            pending.Enqueue(completion);
            return completion.Task;
        }

        public static void AdvanceNext()
        {
            if (pending.Count == 0) throw new InvalidOperationException("No pending tick.");
            pending.Dequeue().SetResult();
        }

        public static void DiscardPending() => pending.Clear();
    }

    public static class AvidCancellationExtensions
    {
        public static Task WithCancellation(this Task task, CancellationToken token) =>
            task.WaitAsync(token);
    }
}

internal static class Program
{
    private static void Main()
    {
        RunCase(cancel: false);
        RunCase(cancel: true);
        Console.WriteLine("DirectAwaitCleanup.Reference: 2/2 passed");
    }

    private static void RunCase(bool cancel)
    {
        Script.CleanupCount = 0;
        Script.CatchCount = 0;
        Script.Lifetime = AvidScript.AvidCancellationSource.Create();
        try
        {
            Task<int> task = Script.RunAsync();
            if (cancel)
            {
                Script.Lifetime.Cancel();
                try
                {
                    task.GetAwaiter().GetResult();
                    throw new InvalidOperationException("Cancelled task returned normally.");
                }
                catch (OperationCanceledException)
                {
                    // Cancellation bypasses the InvalidOperationException handler.
                }
            }
            else
            {
                AvidScript.AvidContinuations.AdvanceNext();
                int result = task.GetAwaiter().GetResult();
                if (result != 16) throw new InvalidOperationException($"Expected 16, got {result}.");
            }
            if (Script.CleanupCount != 1)
                throw new InvalidOperationException($"Expected one cleanup, got {Script.CleanupCount}.");
            if (Script.CatchCount != 0)
                throw new InvalidOperationException($"Cancellation entered catch {Script.CatchCount} times.");
            Console.WriteLine($"cancel={cancel}: result={(cancel ? "cancelled" : "16")}, cleanup=1, catch=0");
        }
        finally
        {
            Script.Lifetime.Release();
            AvidScript.AvidContinuations.DiscardPending();
        }
    }
}
