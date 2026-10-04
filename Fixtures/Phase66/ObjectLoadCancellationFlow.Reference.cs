using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace AvidScript
{
    public readonly struct AvidCancellationSource
    {
        private readonly CancellationTokenSource? source;
        private AvidCancellationSource(CancellationTokenSource source) { this.source = source; }
        public CancellationToken Token => source!.Token;
        public static AvidCancellationSource Create() => new(new CancellationTokenSource());
        public bool Cancel() { source!.Cancel(); return true; }
        public bool Release() { source?.Dispose(); return true; }
    }

    public readonly struct AvidLoadedObject
    {
        public readonly int Slot;
        public readonly int Generation;
        internal AvidLoadedObject(int slot, int generation) { Slot = slot; Generation = generation; }
    }

    // This driver models the documented queue ownership rule: cancellation can
    // replace a ready result until dispatch; it does not replace shared method bodies.
    internal sealed class ObjectRequest
    {
        internal bool Ready;
        internal bool DirectCancelled;
        internal CancellationToken Token;
        internal AvidLoadedObject Value;
        private Action? continuation;
        private bool queued;
        private CancellationTokenRegistration registration;
        internal void Bind(Action action)
        {
            continuation = action;
            registration = Token.Register(Queue);
            if (Ready || DirectCancelled) Queue();
        }
        internal void Queue()
        {
            if (queued || continuation is null) return;
            queued = true;
            ReferenceDriver.Enqueue(continuation);
        }
        internal AvidLoadedObject Result()
        {
            registration.Dispose();
            if (DirectCancelled) throw new TaskCanceledException("direct cancellation", null, CancellationToken.None);
            if (Token.IsCancellationRequested) throw new TaskCanceledException("source cancellation", null, Token);
            return Value;
        }
    }

    public readonly struct AvidObjectAwaitable
    {
        private readonly ObjectRequest request;
        internal AvidObjectAwaitable(ObjectRequest request) { this.request = request; }
        public AvidObjectAwaitable WithCancellation(CancellationToken token) { request.Token = token; return this; }
        public AvidObjectAwaiter GetAwaiter() => new(request);
    }

    public readonly struct AvidObjectAwaiter : INotifyCompletion
    {
        private readonly ObjectRequest request;
        internal AvidObjectAwaiter(ObjectRequest request) { this.request = request; }
        public bool IsCompleted => request.Ready || request.DirectCancelled || request.Token.IsCancellationRequested;
        public void OnCompleted(Action continuation) => request.Bind(continuation);
        public AvidLoadedObject GetResult() => request.Result();
    }

    public static class AvidAssets
    {
        internal static ObjectRequest? Pending;
        public static AvidObjectAwaitable LoadObjectAsync(string path)
        {
            if (path != "/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture") throw new InvalidOperationException(path);
            Pending = new ObjectRequest();
            return new(Pending);
        }
        internal static void Complete(bool success)
        {
            Pending!.Value = success ? new AvidLoadedObject(111, 7) : default;
            Pending.Ready = true;
            Pending.Queue();
        }
        internal static void CancelDirect() { Pending!.DirectCancelled = true; Pending.Queue(); }
    }

    public static class AvidContinuations
    {
        public static Task NextTickAsync()
        {
            var completion = new TaskCompletionSource();
            ReferenceDriver.Enqueue(() => completion.SetResult());
            return completion.Task;
        }
    }
}

internal sealed class ReferenceDriver : SynchronizationContext
{
    private static readonly ConcurrentQueue<Action> pending = new();
    internal static void Enqueue(Action action) => pending.Enqueue(action);
    public override void Post(SendOrPostCallback callback, object? state) => Enqueue(() => callback(state));
    internal static bool Advance()
    {
        if (!pending.TryDequeue(out var action)) return false;
        action();
        return true;
    }
}

internal static class Program
{
    private static int checks;
    private static void Check(bool valid, string message)
    {
        if (!valid) throw new InvalidOperationException(message);
        checks++;
    }
    private static void Main(string[] args)
    {
        SynchronizationContext.SetSynchronizationContext(new ReferenceDriver());
        string[] names = { "success", "failed", "timer", "cancel", "rethrow", "pre-cancel", "ready-cancel", "ready-failed-cancel", "released-cancel", "direct-cancel" };
        var cases = new List<object>();
        for (int id = 0; id < names.Length; id++)
        {
            ObjectCancellationScript.Mode = id == 2 ? 1 : id == 4 ? 2 : id == 5 ? 3 : 0;
            ObjectCancellationScript.Result = ObjectCancellationScript.Trace = ObjectCancellationScript.LoadedSlot = 0;
            ObjectCancellationScript.LoadedGeneration = ObjectCancellationScript.InnerCatch = ObjectCancellationScript.OuterCatch = 0;
            ObjectCancellationScript.IdentityMatch = ObjectCancellationScript.WrongCatch = 0;
            Task<int> task = ObjectCancellationScript.RunAsync();
            bool cancel = id >= 3;
            bool success = id == 0 || id == 2;
            if (id != 5)
            {
                Check(!task.IsCompleted && ObjectCancellationScript.Trace == 0, names[id] + " suspended without cleanup");
                if (id == 6 || id == 7) AvidScript.AvidAssets.Complete(id == 6);
                if (id == 9) AvidScript.AvidAssets.CancelDirect();
                else if (cancel) ObjectCancellationScript.Lifetime.Cancel();
                else AvidScript.AvidAssets.Complete(success);
                if (id == 8) ObjectCancellationScript.Lifetime.Release();
                Check(ObjectCancellationScript.Trace == 0, names[id] + " queue changes cannot execute cleanup");
            }
            for (int step = 0; !task.IsCompleted && step < 32; step++)
                Check(ReferenceDriver.Advance(), names[id] + " progress is queued");
            Check(task.Status == TaskStatus.RanToCompletion, names[id] + " outer task completes");
            int expected = id == 4 ? 40 : cancel ? 37 : success ? 17 : 27;
            Check(task.Result == expected, names[id] + " result");
            Check(ObjectCancellationScript.Trace == (cancel ? 34 : 234), names[id] + " finally order");
            Check(ObjectCancellationScript.InnerCatch == (cancel ? 1 : 0), names[id] + " task catch");
            Check(ObjectCancellationScript.OuterCatch == (id == 4 ? 1 : 0), names[id] + " rethrow catch");
            Check(ObjectCancellationScript.IdentityMatch == (cancel && id != 9 ? 1 : 0), names[id] + " winning token");
            Check(ObjectCancellationScript.WrongCatch == 0, names[id] + " wrong handler skipped");
            Check((ObjectCancellationScript.LoadedSlot > 0 && ObjectCancellationScript.LoadedGeneration > 0) == success,
                names[id] + " payload is consumed only after success");
            cases.Add(new { id, name = names[id], mode = ObjectCancellationScript.Mode, result = task.Result,
                trace = ObjectCancellationScript.Trace, innerCatch = ObjectCancellationScript.InnerCatch,
                outerCatch = ObjectCancellationScript.OuterCatch, identityMatch = ObjectCancellationScript.IdentityMatch,
                wrongCatch = ObjectCancellationScript.WrongCatch, hasHandle = success });
            while (ReferenceDriver.Advance()) { }
        }
        // Inspect the original child body directly: rethrow remains Cancelled,
        // and its token is the source token even after the source is released.
        ObjectCancellationScript.Mode = 2;
        var source = AvidScript.AvidCancellationSource.Create();
        CancellationToken token = source.Token;
        Task<int> child = ObjectCancellationScript.LoadAsync(token);
        source.Cancel(); source.Release();
        while (!child.IsCompleted) Check(ReferenceDriver.Advance(), "child cancellation progresses");
        Check(child.IsCanceled && !child.IsFaulted, "rethrow child terminal state is Cancelled");
        try { child.GetAwaiter().GetResult(); throw new InvalidOperationException("missing cancellation"); }
        catch (TaskCanceledException error) { Check(error.CancellationToken == token, "child token survives source release"); }
        if (args.Length == 1) File.WriteAllText(args[0], JsonSerializer.Serialize(cases));
        Console.WriteLine($"ObjectLoadCancellationFlow.Reference: {checks}/{checks} passed; cases={cases.Count}");
    }
}
