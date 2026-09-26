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
    // Keep source continuations and next-tick completions on one driver thread,
    // like the UE Game Thread. A thread-pool race must not complete a new tick
    // while its caller is still attaching an already-cancelled token.
    private sealed class ReferenceSynchronizationContext : SynchronizationContext
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
        int completionValue = args.Length == 0 ? 16 : int.Parse(args[0]);
        int passed = 0;
        foreach (bool cancel in new[] { false, true })
        {
            for (int mode = 0; mode < 3; ++mode)
            {
                int selectedMode = mode;
                RunCase($"outer:{mode}", () => CancellationScript.OuterAsync(selectedMode), cancel,
                    cancel ? mode == 0 ? 18 : mode == 1 ? 17 : 20 : completionValue,
                    expectedTrace: 12, expectedInner: cancel ? 1 : 0,
                    expectedOuter: cancel && mode == 2 ? 1 : 0);
                passed++;
            }
            RunCase("unhandled", CancellationScript.UnhandledAsync, cancel, completionValue,
                12, cancel ? 1 : 0, 0, expectCancelled: cancel);
            RunCase("nested", CancellationScript.NestedAsync, cancel, cancel ? 21 : completionValue,
                12, cancel ? 1 : 0, cancel ? 1 : 0);
            RunCase("catch-all", CancellationScript.CatchAllAsync, cancel, cancel ? 22 : completionValue,
                1, cancel ? 1 : 0, 0);
            passed += 3;
            for (int mode = 0; mode < 4; ++mode)
            {
                int selectedMode = mode;
                RunCase($"repeated:{mode}", () => CancellationScript.RepeatedAsync(selectedMode), cancel,
                    cancel ? mode == 2 ? 21 : 40 : 2 * completionValue,
                    1, cancel ? 1 : 0, cancel ? mode >= 2 ? 1 : 2 : 0,
                    expectCancelled: cancel && mode == 3,
                    expectedRepeatTrace: cancel && mode >= 2 ? 1 : 12);
                passed++;
            }
            for (int mode = 0; mode < 4; ++mode)
            {
                int selectedMode = mode;
                RunCase($"conditional:{mode}", () => CancellationScript.ConditionalAsync(selectedMode), cancel,
                    mode == 2 ? 5 : cancel ? 30 : mode == 3 ? 2 * completionValue : completionValue,
                    mode == 2 ? 0 : 1, cancel && mode != 2 ? 1 : 0, cancel && mode != 2 ? 1 : 0,
                    expectedConditionalTrace: 12);
                passed++;
            }
        }
        Console.WriteLine($"AsyncCancellationFlow.Reference: {passed}/28 passed");
    }

    private static void RunCase(string name, Func<Task<int>> start, bool cancel,
        int expectedResult, int expectedTrace, int expectedInner, int expectedOuter,
        bool expectCancelled = false, int expectedRepeatTrace = 0, int expectedConditionalTrace = 0)
    {
        CancellationScript.Trace = 0;
        CancellationScript.InnerCatch = 0;
        CancellationScript.OuterCatch = 0;
        CancellationScript.WrongCatch = 0;
        CancellationScript.RepeatTrace = 0;
        CancellationScript.ConditionalTrace = 0;
        CancellationScript.Lifetime = AvidScript.AvidCancellationSource.Create();
        SynchronizationContext? priorContext = SynchronizationContext.Current;
        ReferenceSynchronizationContext scheduler = new();
        SynchronizationContext.SetSynchronizationContext(scheduler);
        try
        {
            Task<int> task = start();
            if (task.IsCompleted) throw new InvalidOperationException("Expected a suspended task.");
            if (cancel) CancellationScript.Lifetime.Cancel();
            Stopwatch timer = Stopwatch.StartNew();
            while (!task.IsCompleted && timer.Elapsed < TimeSpan.FromSeconds(5))
            {
                if (!scheduler.AdvanceNext() && !AvidScript.AvidContinuations.AdvanceNext()) Thread.Yield();
            }
            if (!task.IsCompleted) throw new TimeoutException(name);
            int result = 0;
            Exception? error = null;
            try { result = task.GetAwaiter().GetResult(); }
            catch (Exception caught) { error = caught; }
            bool terminalMatches = expectCancelled
                ? task.IsCanceled && error is TaskCanceledException cancellation
                    && cancellation.CancellationToken == CancellationScript.Lifetime.Token
                : task.IsCompletedSuccessfully && error is null && result == expectedResult;
            if (!terminalMatches || CancellationScript.Trace != expectedTrace
                || CancellationScript.InnerCatch != expectedInner
                || CancellationScript.OuterCatch != expectedOuter || CancellationScript.WrongCatch != 0
                || CancellationScript.RepeatTrace != expectedRepeatTrace
                || CancellationScript.ConditionalTrace != expectedConditionalTrace)
                throw new InvalidOperationException($"{name}: result={result}, state={task.Status}, error={error?.GetType().Name}, trace={CancellationScript.Trace}, inner={CancellationScript.InnerCatch}, outer={CancellationScript.OuterCatch}, wrong={CancellationScript.WrongCatch}");
            if (expectCancelled)
            {
                Exception? repeated = null;
                try { task.GetAwaiter().GetResult(); }
                catch (Exception again) { repeated = again; }
                if (!ReferenceEquals(error, repeated))
                    throw new InvalidOperationException("Repeated await lost the cancellation exception identity.");
            }
            Console.WriteLine($"{name}, cancel={cancel}: {task.Status}, result={result}, trace={expectedTrace}, inner={expectedInner}, outer={expectedOuter}");
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(priorContext);
            CancellationScript.Lifetime.Release();
            AvidScript.AvidContinuations.DiscardPending();
        }
    }
}
