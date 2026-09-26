using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
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
        private static readonly Queue<Action> pending = new();
        public static ReferenceTickAwaitable NextTickAsync() => new(CancellationToken.None);
        internal static void Enqueue(Action continuation) => pending.Enqueue(continuation);
        public static int PendingCount => pending.Count;
        public static void Advance()
        {
            // A newly requested tick belongs to the next frame, not this one.
            int count = pending.Count;
            while (count-- > 0) pending.Dequeue()();
        }
    }

    public readonly struct ReferenceTickAwaitable : INotifyCompletion
    {
        private readonly CancellationToken token;
        public ReferenceTickAwaitable(CancellationToken token) { this.token = token; }
        public ReferenceTickAwaitable WithCancellation(CancellationToken cancellation) => new(cancellation);
        public ReferenceTickAwaitable GetAwaiter() => this;
        public bool IsCompleted => token.IsCancellationRequested;
        public void OnCompleted(Action continuation) => AvidContinuations.Enqueue(continuation);
        public void GetResult() => token.ThrowIfCancellationRequested();
    }
}

internal static class Program
{
    private sealed class ReferenceContext : SynchronizationContext
    {
        private readonly Queue<(SendOrPostCallback Callback, object? State)> pending = new();
        public override void Post(SendOrPostCallback callback, object? state) => pending.Enqueue((callback, state));
        public int PendingCount => pending.Count;
        public void Drain()
        {
            int budget = 128;
            while (pending.Count > 0)
            {
                if (--budget == 0) throw new InvalidOperationException("Reference continuation budget exceeded.");
                var item = pending.Dequeue();
                item.Callback(item.State);
            }
        }
    }

    private static int passed;
    private static ReferenceContext context = new();

    private static void Main()
    {
        SynchronizationContext? previous = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(context);
        try
        {
            foreach (bool property in new[] { false, true })
            {
                for (int mode = 0; mode < 8; mode++) RunSingle(property, mode);
                RunImplicit(property);
                RunRepeated(property);
                RunConcurrent(property);
                RunTaskLocal(property, ready: false);
                RunTaskLocal(property, ready: true);
            }
            RunSingle(property: true, mode: 8);
            RunTemporary(cancel: false);
            RunTemporary(cancel: true);
            Check(passed == 29, "Missing reference scenario.");
            Console.WriteLine($"AwaitMemberAssignment.Reference: {passed}/29 passed");
        }
        finally { SynchronizationContext.SetSynchronizationContext(previous); }
    }

    private static void Reset()
    {
        Check(AvidScript.AvidContinuations.PendingCount == 0 && context.PendingCount == 0,
            "The previous case left pending work.");
        AwaitMemberAssignment.Original = new AwaitAssignmentTarget();
        AwaitMemberAssignment.Replacement = new AwaitAssignmentTarget();
        AwaitMemberAssignment.Current = AwaitMemberAssignment.Original;
        AwaitMemberAssignment.ReceiverCalls = 0;
        AwaitMemberAssignment.ProducerCalls = 0;
        AwaitMemberAssignment.CleanupCount = 0;
        AwaitMemberAssignment.Trace = 0;
        AwaitMemberAssignment.TotalSetterCalls = 0;
        AwaitMemberAssignment.LastAssignedId = 0;
        AwaitMemberAssignment.LastAssignedValue = 0;
        AwaitMemberAssignment.Lifetime = AvidScript.AvidCancellationSource.Create();
    }

    private static void Advance()
    {
        AvidScript.AvidContinuations.Advance();
        context.Drain();
    }

    private static void Complete(Task<int> task)
    {
        for (int tick = 0; tick < 8 && !task.IsCompleted; tick++) Advance();
        Check(task.IsCompleted, "Reference task stalled.");
        context.Drain();
        Check(AvidScript.AvidContinuations.PendingCount == 0 && context.PendingCount == 0,
            "Completed case left pending work.");
    }

    private static void RunSingle(bool property, int mode)
    {
        Reset();
        try
        {
            if (mode == 5) AwaitMemberAssignment.Lifetime.Cancel();
            if (mode == 8) AwaitMemberAssignment.Original.ThrowOnSet = true;
            Task<int> task = AwaitMemberAssignment.Assign(property, mode);
            bool suspended = mode is 0 or 2 or 4 or 6 or 8;
            Check(task.IsCompleted != suspended, $"mode={mode}: wrong initial completion state.");
            if (suspended)
            {
                Check(AvidScript.AvidContinuations.PendingCount == 1, "Expected one suspended producer.");
                Check(AwaitMemberAssignment.Trace == 12 && AwaitMemberAssignment.CleanupCount == 0,
                    "Receiver must precede producer; cleanup must wait for completion.");
                AssertValues(11, 11, property, 0, 0);
            }
            if (mode == 4) AwaitMemberAssignment.Lifetime.Cancel();
            Complete(task);
            Type? expectedError = mode switch
            {
                2 or 3 or 7 => typeof(ArgumentException),
                4 or 5 => typeof(OperationCanceledException),
                6 => typeof(NullReferenceException),
                8 => typeof(InvalidOperationException),
                _ => null,
            };
            Exception? error = null;
            int result = 0;
            try { result = task.GetAwaiter().GetResult(); }
            catch (Exception caught) { error = caught; }
            Check(expectedError is null ? error is null && result == 7 && task.IsCompletedSuccessfully
                : error is not null && expectedError.IsInstanceOfType(error), "Wrong terminal result.");
            Check(task.IsCanceled == (mode is 4 or 5), "Cancellation became an ordinary fault.");
            if (error is OperationCanceledException cancellation)
                Check(cancellation.CancellationToken == AwaitMemberAssignment.Lifetime.Token,
                    "Cancellation token identity was lost.");
            bool writes = mode is 0 or 1;
            AssertValues(writes ? 7 : 11, 11, property, property && (writes || mode == 8) ? 1 : 0, 0);
            int expectedTrace = mode switch
            {
                0 => property ? 12348 : 1238,
                1 => property ? 1248 : 128,
                2 or 6 => 1238,
                3 or 4 or 5 => 128,
                7 => 18,
                8 => 12348,
                _ => throw new InvalidOperationException(),
            };
            Check(AwaitMemberAssignment.ReceiverCalls == 1
                && AwaitMemberAssignment.ProducerCalls == (mode == 7 ? 0 : 1)
                && AwaitMemberAssignment.CleanupCount == 1
                && AwaitMemberAssignment.Trace == expectedTrace, $"mode={mode}: wrong evaluation order or count.");
            Check(ReferenceEquals(AwaitMemberAssignment.Current,
                mode == 7 ? AwaitMemberAssignment.Original : AwaitMemberAssignment.Replacement),
                "The producer must be allowed to change the source receiver variable.");
            Pass($"{(property ? "property" : "field")}/mode={mode}", task.Status);
        }
        finally { AwaitMemberAssignment.Lifetime.Release(); }
    }

    private static void RunImplicit(bool property)
    {
        Reset();
        try
        {
            Task<int> task = AwaitMemberAssignment.Original.AssignImplicit(property);
            Check(!task.IsCompleted, "Implicit receiver case must suspend.");
            AssertValues(11, 11, property, 0, 0);
            Complete(task);
            Check(task.GetAwaiter().GetResult() == 7, "Wrong implicit receiver result.");
            AssertValues(7, 11, property, property ? 1 : 0, 0);
            Check(AwaitMemberAssignment.ReceiverCalls == 0 && AwaitMemberAssignment.ProducerCalls == 1
                && AwaitMemberAssignment.CleanupCount == 1
                && AwaitMemberAssignment.Trace == (property ? 2348 : 238), "Implicit receiver was lost.");
            Pass($"implicit/{property}", task.Status);
        }
        finally { AwaitMemberAssignment.Lifetime.Release(); }
    }

    private static void RunRepeated(bool property)
    {
        Reset();
        try
        {
            Task<int> task = AwaitMemberAssignment.AssignTwice(property);
            Check(!task.IsCompleted, "Loop must suspend on its first iteration.");
            AssertValues(11, 11, property, 0, 0);
            Advance();
            Check(!task.IsCompleted && AvidScript.AvidContinuations.PendingCount == 1,
                "The second iteration needs a fresh suspension.");
            AssertValues(7, 11, property, property ? 1 : 0, 0);
            Complete(task);
            Check(task.GetAwaiter().GetResult() == 14, "Wrong loop result.");
            AssertValues(7, 7, property, property ? 1 : 0, property ? 1 : 0);
            Check(AwaitMemberAssignment.ReceiverCalls == 2 && AwaitMemberAssignment.ProducerCalls == 2
                && AwaitMemberAssignment.CleanupCount == 1
                && AwaitMemberAssignment.Trace == (property ? 123412348 : 1231238), "Loop reused a stale receiver.");
            Pass($"loop/{property}", task.Status);
        }
        finally { AwaitMemberAssignment.Lifetime.Release(); }
    }

    private static void RunConcurrent(bool property)
    {
        Reset();
        try
        {
            Task<int> first = AwaitMemberAssignment.Assign(property, 0);
            Task<int> second = AwaitMemberAssignment.Assign(property, 0);
            Check(!first.IsCompleted && !second.IsCompleted
                && AvidScript.AvidContinuations.PendingCount == 2, "Both invocations must suspend independently.");
            AssertValues(11, 11, property, 0, 0);
            Complete(first);
            Complete(second);
            Check(first.GetAwaiter().GetResult() == 7 && second.GetAwaiter().GetResult() == 7,
                "Wrong concurrent invocation results.");
            AssertValues(7, 7, property, property ? 1 : 0, property ? 1 : 0);
            Check(AwaitMemberAssignment.ReceiverCalls == 2 && AwaitMemberAssignment.ProducerCalls == 2
                && AwaitMemberAssignment.CleanupCount == 2
                && AwaitMemberAssignment.Trace == (property ? 1212348348 : 12123838),
                "Concurrent invocations shared a captured receiver.");
            Pass($"concurrent/{property}", first.Status);
        }
        finally { AwaitMemberAssignment.Lifetime.Release(); }
    }

    private static void AssertValues(int original, int replacement, bool property, int originalCalls, int replacementCalls)
    {
        AwaitAssignmentTarget left = AwaitMemberAssignment.Original;
        AwaitAssignmentTarget right = AwaitMemberAssignment.Replacement;
        Check((property ? left.Value : left.Field) == original
            && (property ? right.Value : right.Field) == replacement
            && (property ? left.Field : left.Value) == 11
            && (property ? right.Field : right.Value) == 11
            && left.SetterCalls == originalCalls && right.SetterCalls == replacementCalls,
            $"Wrong write target/count: original={left.Field}/{left.Value}/{left.SetterCalls}, replacement={right.Field}/{right.Value}/{right.SetterCalls}.");
    }

    private static void RunTemporary(bool cancel)
    {
        Reset();
        try
        {
            Task<int> task = AwaitMemberAssignment.AssignTemporary();
            Check(!task.IsCompleted && AwaitMemberAssignment.TotalSetterCalls == 0
                && AwaitMemberAssignment.Trace == 12, "Temporary receiver must survive a suspension before writing.");
            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();
            if (cancel) AwaitMemberAssignment.Lifetime.Cancel();
            Complete(task);
            if (cancel)
            {
                Check(task.IsCanceled, "Temporary assignment did not cancel.");
                try { task.GetAwaiter().GetResult(); throw new InvalidOperationException("Expected cancellation."); }
                catch (OperationCanceledException cancellation)
                {
                    Check(cancellation.CancellationToken == AwaitMemberAssignment.Lifetime.Token,
                        "Temporary assignment lost the cancellation token.");
                }
            }
            else Check(task.GetAwaiter().GetResult() == 7, "Temporary assignment lost its result.");
            Check(AwaitMemberAssignment.ReceiverCalls == 1 && AwaitMemberAssignment.ProducerCalls == 1
                && AwaitMemberAssignment.CleanupCount == 1
                && AwaitMemberAssignment.TotalSetterCalls == (cancel ? 0 : 1)
                && AwaitMemberAssignment.LastAssignedId == (cancel ? 0 : 42)
                && AwaitMemberAssignment.LastAssignedValue == (cancel ? 0 : 7)
                && AwaitMemberAssignment.Trace == (cancel ? 128 : 12348), "Temporary receiver identity or write count changed.");
            AssertValues(11, 11, property: true, 0, 0);
            Pass($"temporary/cancel={cancel}", task.Status);
        }
        finally { AwaitMemberAssignment.Lifetime.Release(); }
    }

    private static void RunTaskLocal(bool property, bool ready)
    {
        Reset();
        try
        {
            Task<int> task = AwaitMemberAssignment.AssignTaskLocal(property, ready);
            Check(task.IsCompleted == ready, "Task-local readiness changed.");
            if (!ready)
            {
                AssertValues(11, 11, property, 0, 0);
                Check(AwaitMemberAssignment.Trace == 21, "Task creation must precede the LHS evaluation.");
            }
            Complete(task);
            Check(task.GetAwaiter().GetResult() == 7, "Wrong Task-local result.");
            AssertValues(11, 7, property, 0, property ? 1 : 0);
            Check(AwaitMemberAssignment.ReceiverCalls == 1 && AwaitMemberAssignment.ProducerCalls == 1
                && AwaitMemberAssignment.CleanupCount == 1
                && AwaitMemberAssignment.Trace == (ready ? property ? 2148 : 218 : property ? 21348 : 2138),
                "Task-local assignment captured the receiver before the source statement.");
            Pass($"task-local/{property}/ready={ready}", task.Status);
        }
        finally { AwaitMemberAssignment.Lifetime.Release(); }
    }

    private static void Check(bool condition, string reason)
    {
        if (!condition) throw new InvalidOperationException(reason);
    }

    private static void Pass(string name, TaskStatus status)
    {
        passed++;
        Console.WriteLine($"PASS {name}: {status}, trace={AwaitMemberAssignment.Trace}");
    }
}
