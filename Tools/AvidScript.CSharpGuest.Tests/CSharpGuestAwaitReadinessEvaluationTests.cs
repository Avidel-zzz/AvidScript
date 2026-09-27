using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestAwaitReadinessEvaluationTests
{
    private sealed record ReferenceResult(SortedDictionary<string, int> Final, SortedDictionary<string, int> FirstResume);

    public static int Run(bool cancellationIdentity = false)
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_AWAIT_READINESS_DIR");
        if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
        List<object> fixtures = new();
        string[] names = { "open", "pre-cancelled", "argument-cancels", "token-cancels", "argument-throws",
            "token-throws", "select-open", "select-cancelled", "argument-selects" };
        for (int mode = 0; mode < names.Length; ++mode)
        foreach (bool resumed in new[] { false, true })
        {
            string name = names[mode] + (resumed ? "-resumed" : "-entry");
            string source = Source(mode, resumed);
            var expected = Reference(source);
            int trace = mode is 1 or 2 or 3 or 7 ? 1248 : mode == 4 ? 158 : mode == 5 ? 1258 : 1238;
            int result = mode is 1 or 2 or 3 or 7 ? 13 : mode is 4 or 5 ? 11 : 7;
            Check(expected.Final["Trace"] == trace && expected.Final["Result"] == result
                && expected.Final["ArgumentCalls"] == 1 && expected.Final["TokenCalls"] == (mode == 4 ? 0 : 1)
                && expected.Final["Cleanup"] == 1, name + " .NET evaluation order/count");
            int initialTrace = resumed ? 0 : mode is 0 or 6 or 8 ? 12 : trace;
            Check(expected.Final["InitialTrace"] == initialTrace
                && expected.Final["InitialArguments"] == (resumed ? 0 : 1)
                && expected.Final["InitialTokens"] == (resumed || mode == 4 ? 0 : 1)
                && expected.Final["InitialCleanup"] == (resumed || mode is 0 or 6 or 8 ? 0 : 1),
                name + " .NET immediate side effects");
            Check(expected.FirstResume["Trace"] == (resumed && mode is 0 or 6 or 8 ? 12 : trace)
                && expected.FirstResume["Cleanup"] == (resumed && mode is 0 or 6 or 8 ? 0 : 1),
                name + " .NET first-resume side effects");
            string sourceId = "Scripts/AwaitReadiness_" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
                enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true, enableAsyncSynchronousExceptions: true);
            byte[] semanticBytes = SemanticSerializer.Serialize(semantic);
            if (!string.IsNullOrWhiteSpace(directory))
            {
                File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), semanticBytes);
            }
            Check(semantic.SchemaVersion == 50 && SemanticAsyncInvocationValidator.IsValid(semantic)
                && !semantic.Diagnostics.Any(item => item.Severity == "error" && item.Code is not ("ASCS3001" or "ASCS5422")),
                name + " semantic: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
            string hash = Convert.ToHexString(SHA256.HashData(semanticBytes)).ToLowerInvariant();
            Check(CSharpLanguageErrorCompiler.TryLower(semantic, hash, out var compilation, out string? error)
                && compilation is not null, name + " lowering: " + error);
            var module = compilation!.Module;
            Check(module.SchemaVersion == 31 && module.DirectAwaitReadiness?.BaseSchemaVersion == 29
                && GuestModuleValidator.Validate(module).Succeeded, name + " validated readiness with synchronous exceptions");
            count += CSharpGuestCancellationIdentityTests.CheckUpgrade(module, out var identityModule);
            if (cancellationIdentity) module = identityModule;
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            byte[] ir = GuestIrSerializer.Serialize(module);
            Check(ir.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(ir))), name + " canonical IR");
            if (string.IsNullOrWhiteSpace(directory)) continue;
            int Offset(string field) => module.MemoryLayout.StateSlots.Single(slot =>
                slot.GlobalId.Contains("." + field + ":", StringComparison.Ordinal)).Offset;
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), ir);
            fixtures.Add(new { name, moduleId = module.ModuleId, cancel = false, expected = result, trace,
                resultOffset = Offset("Result"), traceOffset = Offset("Trace"),
                observations = expected.Final.Select(item => new { name = item.Key, expected = item.Value, offset = Offset(item.Key) }).ToArray(),
                firstResumeObservations = expected.FirstResume.Select(item => new { name = item.Key, expected = item.Value, offset = Offset(item.Key) }).ToArray() });
        }
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        foreach (string returnType in new[] { "float", "AvidCancellationToken" })
        {
            string source = $$"""
                using AvidScript; using System; using System.Threading.Tasks;
                public static class Script {
                    public static int Trace;
                    public static {{returnType}} Fail() {
                        try { throw new ArgumentException(); }
                        finally { Trace++; }
                    }
                    public static async Task<int> Run() {
                        await AvidContinuations.NextTickAsync();
                        {{returnType}} value = Fail();
                        return 7;
                    }
                }
                """;
            const string sourceId = "Scripts/TypedCleanup.cs";
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(), enableAsyncExceptionFlow: true,
                enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true, enableAsyncSynchronousExceptions: true);
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            Check(SemanticExceptionFlowContractValidator.IsValid(semantic)
                && !CSharpLanguageErrorCompiler.TryLower(semantic, hash, out _, out string? error)
                && error is not null && error.Contains("typed cleanup storage", StringComparison.Ordinal),
                returnType + " finally still requires its own typed cleanup contract");
        }
        return count;
    }

    private static string Source(int mode, bool resumed) => $$"""
        using AvidScript;
        using System;
        using System.Runtime.InteropServices;
        using System.Threading.Tasks;
        public static class Script {
            public static int Result, Trace, ArgumentCalls, TokenCalls, Cleanup;
            public static int InitialTrace, InitialArguments, InitialTokens, InitialCleanup;
            private static int Mode;
            private static AvidCancellationSource First, Second, Selected;
            [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
            public static async void BeginPlay() { Result = await Observe(); }
            public static async Task<int> Observe() {
                Task<int> pending = Run();
                InitialTrace = Trace; InitialArguments = ArgumentCalls;
                InitialTokens = TokenCalls; InitialCleanup = Cleanup;
                int value = await pending;
                return value;
            }
            public static float DelaySeconds() {
                ArgumentCalls++; Trace = Trace * 10 + 1;
                if (Mode == 2) First.Cancel();
                if (Mode == 8) Selected = Second;
                if (Mode == 4) throw new ArgumentException();
                return 0.001f;
            }
            public static AvidCancellationToken SelectToken() {
                TokenCalls++; Trace = Trace * 10 + 2;
                if (Mode == 3) Selected.Cancel();
                if (Mode == 6 || Mode == 7) Selected = Second;
                if (Mode == 5) throw new ArgumentException();
                return Selected.Token;
            }
            public static async Task<int> Run() {
                Mode = {{mode}};
                First = AvidCancellationSource.Create(); Second = AvidCancellationSource.Create(); Selected = First;
                try {
                    {{(mode is 1 or 6 or 8 ? "First.Cancel();" : "")}}
                    {{(mode == 7 ? "Second.Cancel();" : "")}}
                    {{(resumed ? "await AvidContinuations.NextTickAsync();" : "")}}
                    await AvidContinuations.DelayAsync(DelaySeconds()).WithCancellation(SelectToken());
                    Trace = Trace * 10 + 3; return 7;
                } catch (OperationCanceledException) { Trace = Trace * 10 + 4; return 13; }
                  catch (ArgumentException) { Trace = Trace * 10 + 5; return 11; }
                finally { Cleanup++; Trace = Trace * 10 + 8; First.Release(); Second.Release(); }
            }
        }
        """;

    private static ReferenceResult Reference(string source)
    {
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("ReadinessReference", new[] {
            CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(ReferenceFacade) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emit = compilation.Emit(bytes);
        if (!emit.Success) throw new InvalidOperationException(string.Join(" | ", emit.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("readiness-reference", isCollectible: true);
        var previous = SynchronizationContext.Current;
        try
        {
            SynchronizationContext.SetSynchronizationContext(null);
            var assembly = context.LoadFromStream(bytes);
            var script = assembly.GetType("Script")!;
            var scheduler = assembly.GetType("AvidScript.AvidContinuations")!;
            var task = (Task<int>)script.GetMethod("Observe")!.Invoke(null, null)!;
            scheduler.GetMethod("Advance")!.Invoke(null, null);
            SortedDictionary<string, int> firstResume = new(StringComparer.Ordinal);
            foreach (string name in new[] { "ArgumentCalls", "TokenCalls", "Trace", "Cleanup" })
                firstResume.Add(name, (int)script.GetField(name)!.GetValue(null)!);
            for (int tick = 0; !task.IsCompleted && tick < 16; ++tick) scheduler.GetMethod("Advance")!.Invoke(null, null);
            if (!task.IsCompleted || (int)scheduler.GetProperty("PendingCount")!.GetValue(null)! != 0)
                throw new InvalidOperationException("Reference left pending work");
            script.GetField("Result")!.SetValue(null, task.GetAwaiter().GetResult());
            return new(new(script.GetFields(BindingFlags.Public | BindingFlags.Static)
                .ToDictionary(field => field.Name, field => (int)field.GetValue(null)!), StringComparer.Ordinal), firstResume);
        }
        finally { SynchronizationContext.SetSynchronizationContext(previous); context.Unload(); }
    }

    private const string ReferenceFacade = """
        namespace AvidScript {
            public readonly struct AvidCancellationToken {
                internal readonly System.Threading.CancellationToken Value;
                internal AvidCancellationToken(System.Threading.CancellationToken value) { Value = value; }
            }
            public readonly struct AvidCancellationSource {
                private readonly System.Threading.CancellationTokenSource source;
                private AvidCancellationSource(System.Threading.CancellationTokenSource value) { source = value; }
                public AvidCancellationToken Token => new(source.Token);
                public static AvidCancellationSource Create() => new(new System.Threading.CancellationTokenSource());
                public void Cancel() => source.Cancel();
                public void Release() => source.Dispose();
            }
            public static class AvidContinuations {
                private static readonly System.Collections.Generic.Queue<System.Action> pending = new();
                public static TickAwaitable NextTickAsync() => new(default);
                public static TickAwaitable DelayAsync(float seconds) => new(default);
                internal static void Enqueue(System.Action continuation) => pending.Enqueue(continuation);
                public static int PendingCount => pending.Count;
                public static void Advance() { int count = pending.Count; while (count-- > 0) pending.Dequeue()(); }
            }
            public readonly struct TickAwaitable : System.Runtime.CompilerServices.INotifyCompletion {
                private readonly AvidCancellationToken token;
                public TickAwaitable(AvidCancellationToken value) { token = value; }
                public TickAwaitable WithCancellation(AvidCancellationToken value) => new(value);
                public TickAwaitable GetAwaiter() => this;
                public bool IsCompleted => token.Value.IsCancellationRequested;
                public void OnCompleted(System.Action continuation) => AvidContinuations.Enqueue(continuation);
                public void GetResult() => token.Value.ThrowIfCancellationRequested();
            }
        }
        """;
}
