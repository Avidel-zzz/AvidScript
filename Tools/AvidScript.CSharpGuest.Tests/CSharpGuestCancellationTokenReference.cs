using System;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestCancellationTokenReference
{
    // Business source is identical. Only the UE scheduler/source facade is
    // replaced with BCL Tasks and CancellationTokenSource for the CLR oracle.
    internal static int Execute(string source, bool asynchronous)
    {
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("TokenReference", new[] {
            CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(Facade),
        }, references, new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("token-reference", isCollectible: true);
        var previous = SynchronizationContext.Current;
        try {
            SynchronizationContext.SetSynchronizationContext(null);
            var assembly = context.LoadFromStream(bytes);
            var script = assembly.GetType("Script")!;
            if (!asynchronous) return (int)script.GetMethod("Main")!.Invoke(null, null)!;
            var step = assembly.GetType("AvidScript.AvidContinuations")!.GetMethod("Step")!;
            var task = (Task<int>)script.GetMethod("Run")!.Invoke(null, null)!;
            for (int tick = 0; !task.IsCompleted && tick < 256; tick++)
                if (!(bool)step.Invoke(null, null)!) throw new InvalidOperationException("Token reference stalled without a pending tick.");
            if (!task.IsCompleted) throw new InvalidOperationException("Token reference exceeded its tick budget.");
            return task.GetAwaiter().GetResult();
        } finally {
            SynchronizationContext.SetSynchronizationContext(previous);
            context.Unload();
        }
    }

    internal const string Facade = """
        using System;
        using System.Collections.Generic;
        using System.Runtime.CompilerServices;
        using System.Threading;
        using System.Threading.Tasks;
        namespace AvidScript;
        internal static class TokenIdentity {
            private static readonly Dictionary<long, (CancellationTokenSource Source, CancellationToken Token)> Sources = new();
            private static long next = 100;
            internal static long Create() { while (Sources.ContainsKey(next)) next++; return next++; }
            private static (CancellationTokenSource Source, CancellationToken Token) Entry(long value) {
                if (!Sources.TryGetValue(value, out var entry)) {
                    var source = new CancellationTokenSource();
                    Sources.Add(value, entry = (source, source.Token));
                }
                return entry;
            }
            internal static CancellationTokenSource Source(long value) => Entry(value).Source;
            internal static CancellationToken Token(long value) => Entry(value).Token;
        }
        public readonly struct AvidCancellationToken {
            internal readonly long Value;
            internal AvidCancellationToken(long value) { Value = value; }
            public static implicit operator CancellationToken(AvidCancellationToken token) =>
                token.Value == 0 ? CancellationToken.None : TokenIdentity.Token(token.Value);
        }
        public readonly struct AvidCancellationSource {
            private readonly CancellationTokenSource source;
            public readonly AvidCancellationToken Token;
            private AvidCancellationSource(long value) { source = TokenIdentity.Source(value); Token = new(value); }
            public static AvidCancellationSource Create() => new(TokenIdentity.Create());
            public bool Cancel() { source.Cancel(); return true; }
            public bool Release() { source.Dispose(); return true; }
        }
        public readonly struct AvidDelayAwaitable {
            private readonly CancellationToken token;
            internal AvidDelayAwaitable(CancellationToken token) { this.token = token; }
            public AvidDelayAwaitable WithCancellation(CancellationToken value) => new(value);
            public AvidDelayAwaitable WithCancellation(AvidCancellationToken value) => new(value);
            public TaskAwaiter GetAwaiter() => AvidContinuations.Schedule(token).GetAwaiter();
        }
        public static class AvidContinuations {
            private static readonly Queue<(TaskCompletionSource Completion, CancellationToken Token)> Pending = new();
            public static AvidDelayAwaitable NextTickAsync() => default;
            internal static Task Schedule(CancellationToken token) {
                if (token.IsCancellationRequested) return Task.FromCanceled(token);
                var completion = new TaskCompletionSource();
                Pending.Enqueue((completion, token));
                return completion.Task;
            }
            public static bool Step() {
                if (!Pending.TryDequeue(out var pending)) return false;
                if (pending.Token.IsCancellationRequested) pending.Completion.SetCanceled(pending.Token);
                else pending.Completion.SetResult();
                return true;
            }
        }
        """;
}
