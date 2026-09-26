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
    private static void Main(string[] args)
    {
        int expectedResult = args.Length == 0 ? 16 : int.Parse(args[0]);
        foreach (bool cancel in new[] { false, true })
        foreach (bool cleanupThrows in new[] { false, true })
            RunCase(cancel, cleanupThrows, expectedResult);
        Console.WriteLine("DirectAwaitCleanup.Reference: 4/4 passed");
    }

    private static void RunCase(bool cancel, bool cleanupThrows, int expectedResult)
    {
        Script.CleanupCount = 0;
        Script.CatchCount = 0;
        Script.CleanupMode = cleanupThrows ? 1 : 0;
        Script.Lifetime = AvidScript.AvidCancellationSource.Create();
        try
        {
            Task<int> task = Script.RunAsync();
            if (cancel) Script.Lifetime.Cancel();
            else AvidScript.AvidContinuations.AdvanceNext();
            int result = 0;
            Exception failure = null;
            try
            {
                result = task.GetAwaiter().GetResult();
            }
            catch (Exception error) { failure = error; }
            bool valid = cleanupThrows
                ? task.IsFaulted && failure is InvalidOperationException
                : cancel ? task.IsCanceled && failure is OperationCanceledException
                : task.IsCompletedSuccessfully && failure is null && result == expectedResult;
            if (!valid)
                throw new InvalidOperationException($"Unexpected result={result}, state={task.Status}, failure={failure?.GetType().Name}.");
            if (Script.CleanupCount != 1)
                throw new InvalidOperationException($"Expected one cleanup, got {Script.CleanupCount}.");
            if (Script.CatchCount != 0)
                throw new InvalidOperationException($"Cancellation entered catch {Script.CatchCount} times.");
            Console.WriteLine($"cancel={cancel}, cleanupThrows={cleanupThrows}: state={task.Status}, cleanup=1, catch=0");
        }
        finally
        {
            Script.Lifetime.Release();
            AvidScript.AvidContinuations.DiscardPending();
        }
    }
}
