using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

namespace AvidScript
{
    public readonly struct AvidCancellationSource
    {
        private readonly CancellationTokenSource source;
        private AvidCancellationSource(CancellationTokenSource source) { this.source = source; }
        public CancellationToken Token => source.Token;
        public static AvidCancellationSource Create() => new(new CancellationTokenSource());
        public void Cancel() => source.Cancel();
        public void Release() => source.Dispose();
    }

    public static class AvidContinuations
    {
        private static readonly ConcurrentQueue<TaskCompletionSource> pending = new();
        public static Task NextTickAsync()
        {
            TaskCompletionSource completion = new(TaskCreationOptions.RunContinuationsAsynchronously);
            pending.Enqueue(completion);
            return completion.Task;
        }
        public static bool AdvanceNext()
        {
            if (!pending.TryDequeue(out var completion)) return false;
            completion.SetResult();
            return true;
        }
        public static void DiscardPending() => pending.Clear();
    }

    public static class AvidCancellationExtensions
    {
        public static Task WithCancellation(this Task task, CancellationToken token) => task.WaitAsync(token);
    }
}

internal static class Program
{
    private sealed class Driver : SynchronizationContext
    {
        private readonly ConcurrentQueue<(SendOrPostCallback Callback, object? State)> ready = new();
        public override void Post(SendOrPostCallback callback, object? state) => ready.Enqueue((callback, state));
        public bool AdvanceNext()
        {
            if (!ready.TryDequeue(out var continuation)) return false;
            continuation.Callback(continuation.State);
            return true;
        }
    }

    private static void Main(string[] args)
    {
        int seed = args.Length == 0 ? 16 : int.Parse(args[0]);
        int passed = 0;
        foreach (bool cancel in new[] { false, true })
        {
            TaskLifetimeScript.ChildFinally = TaskLifetimeScript.InnerCatch = TaskLifetimeScript.OuterCatch = 0;
            TaskLifetimeScript.OuterFinally = TaskLifetimeScript.Iterations = TaskLifetimeScript.AfterWait = 0;
            TaskLifetimeScript.Lifetime = AvidScript.AvidCancellationSource.Create();
            var previous = SynchronizationContext.Current;
            Driver driver = new();
            SynchronizationContext.SetSynchronizationContext(driver);
            try
            {
                var task = TaskLifetimeScript.Run();
                if (task.IsCompleted) throw new InvalidOperationException("Expected pending child tasks.");
                if (cancel) TaskLifetimeScript.Lifetime.Cancel();
                var timer = Stopwatch.StartNew();
                while ((!task.IsCompleted || TaskLifetimeScript.ChildFinally != 2) && timer.Elapsed < TimeSpan.FromSeconds(5))
                    if (!driver.AdvanceNext() && !AvidScript.AvidContinuations.AdvanceNext()) Thread.Yield();
                if (!task.IsCompleted) throw new TimeoutException("Task lifetime reference did not complete.");
                int expected = cancel ? 90 : seed * 2 + 1;
                if (task.GetAwaiter().GetResult() != expected || TaskLifetimeScript.ChildFinally != 2
                    || TaskLifetimeScript.InnerCatch != (cancel ? 2 : 0) || TaskLifetimeScript.OuterCatch != (cancel ? 1 : 0)
                    || TaskLifetimeScript.OuterFinally != 1 || TaskLifetimeScript.AfterWait != 1
                    || TaskLifetimeScript.Iterations != (cancel ? 0 : 2))
                    throw new InvalidOperationException($"Task lifetime mismatch: cancel={cancel}, result={task.Result}");
                Console.WriteLine($"cancel={cancel}, result={expected}, children=2, finally=1, post-wait=1");
                passed++;
            }
            finally
            {
                SynchronizationContext.SetSynchronizationContext(previous);
                TaskLifetimeScript.Lifetime.Release();
                AvidScript.AvidContinuations.DiscardPending();
            }
        }
        Console.WriteLine($"TaskLocalLifetimeLifecycle.Reference: {passed}/2 passed");
    }
}
