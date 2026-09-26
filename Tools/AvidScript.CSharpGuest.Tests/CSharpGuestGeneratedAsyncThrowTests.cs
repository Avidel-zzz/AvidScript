using System;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Threading.Tasks;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestGeneratedAsyncThrowTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        const string sourceId = "Fixtures/Phase66/GeneratedAsyncThrow.cs";
        string original = File.ReadAllText(sourceId);
        foreach (int offset in new[] { 0, 16 })
        {
            string source = original.Replace("const int CodeOffset = 0;", $"const int CodeOffset = {offset};", StringComparison.Ordinal);
            Reference(source, offset, Check);
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + Attributes + CancelHost,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true);
            Check(semantic.SchemaVersion == 46 && SemanticAsyncInvocationValidator.IsValid(semantic),
                "Generated throw source: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            Check(!CSharpGuestLowerer.Lower(semantic, hash).Succeeded, "Generated throws require bounded errors");
            var lowered = CSharpGuestLowerer.Lower(semantic, hash, enableAsyncLanguageErrors: true);
            Check(lowered.Succeeded, "Generated throw lowering: " + string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
            var module = lowered.Module!;
            Check(module.SchemaVersion == 26 && module.IrVersion == "1.25"
                && module.AsyncExceptionTransfers!.Where(item => item.Raise is not null)
                    .Select(item => item.Raise!.SourceToken).Distinct().Count() == 3,
                "Generated throws preserve all three source throw sites");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, "Generated throw WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            Check(wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes),
                "Generated throw deterministic WASM");
        }
        return count;
    }

    private const string Attributes = """
        [AttributeUsage(AttributeTargets.Class)] public sealed class UClassAttribute : Attribute { }
        [AttributeUsage(AttributeTargets.Method)] public sealed class UFunctionAttribute : Attribute { }
        [AttributeUsage(AttributeTargets.Property)] public sealed class UPropertyAttribute : Attribute { }
        public abstract class AvidActor { }
        """;

    private const string CancelHost = """

        internal static class CancelResumeHost {
            [System.Runtime.InteropServices.DllImport("avidscript", EntryPoint = "avid_continuation_delay_cancel_resume_v1")]
            internal static extern long Delay(float seconds, int callbackId);
        }
        """;

    private static void Reference(string source, int offset, Action<bool, string> check)
    {
        const string adapter = """
            using System;
            using System.Collections.Generic;
            using System.Threading;
            using System.Threading.Tasks;
            namespace AvidScript;
            public readonly struct AvidCancellationToken {
                public readonly CancellationToken Value;
                public AvidCancellationToken(CancellationToken value) { Value = value; }
            }
            public readonly struct AvidCancellationSource {
                private readonly CancellationTokenSource source;
                private AvidCancellationSource(CancellationTokenSource source) { this.source = source; }
                public AvidCancellationToken Token => new(source.Token);
                public static AvidCancellationSource Create() => new(new CancellationTokenSource());
                public void Cancel() => source.Cancel();
                public void Release() => source.Dispose();
            }
            public static class AvidContinuations {
                private static readonly Queue<TaskCompletionSource> pending = new();
                public static Task NextTickAsync() => DelayAsync(0);
                public static Task DelayAsync(float seconds) {
                    TaskCompletionSource next = new(); pending.Enqueue(next); return next.Task;
                }
                public static void Advance() { if (pending.TryDequeue(out var next)) next.SetResult(); }
            }
            public static class CancellationExtensions {
                public static Task WithCancellation(this Task task, AvidCancellationToken token) => task.WaitAsync(token.Value);
            }
            """;
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("GeneratedThrowReference", new[] {
            CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(adapter + Attributes) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("generated-throw-reference", isCollectible: true);
        try
        {
            var assembly = context.LoadFromStream(bytes);
            var type = assembly.GetType("AsyncThrowActor")!;
            var advance = assembly.GetType("AvidScript.AvidContinuations")!.GetMethod("Advance")!;
            int[] expected = { 700024, 8001234, 9000254, 9001254, 8001234, 8000234 };
            for (int scenario = 0; scenario < expected.Length; scenario++)
            {
                var actor = Activator.CreateInstance(type)!;
                var value = type.GetProperty("Value")!;
                var stage = type.GetProperty("Stage")!;
                value.SetValue(actor, scenario);
                check((int)type.GetMethod("GetScriptValue")!.Invoke(actor, null)! == scenario,
                    "Reference entry returns before completion");
                for (int round = 0; round < 16 && (int)stage.GetValue(actor)! != 1; round++) advance.Invoke(null, null);
                check((int)stage.GetValue(actor)! == 1 && (int)value.GetValue(actor)! == scenario,
                    "Reference suspends after cleanup without writing the result");
                for (int round = 0; round < 16 && (int)stage.GetValue(actor)! != 2; round++) advance.Invoke(null, null);
                check((int)stage.GetValue(actor)! == 2 && (int)value.GetValue(actor)! == expected[scenario] + offset,
                    $"Reference result/cleanup order: scenario={scenario} generation={offset}");
            }
        }
        finally { context.Unload(); }
    }
}
