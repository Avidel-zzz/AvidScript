using System;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Threading;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using AvidScript.UeTypeGenerator;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestGeneratedCompositionTests
{
    internal const string SourceId = "Fixtures/Phase66/GeneratedNaturalReload.cs";
    private const string Declarations = """

        [System.AttributeUsage(System.AttributeTargets.Class)] public sealed class UClassAttribute : System.Attribute { }
        [System.AttributeUsage(System.AttributeTargets.Method)] public sealed class UFunctionAttribute : System.Attribute { }
        [System.AttributeUsage(System.AttributeTargets.Property)] public sealed class UPropertyAttribute : System.Attribute { }
        public abstract class AvidActor { protected virtual void BeginPlay() { } }
        public abstract class AvidActorComponent { protected virtual void BeginPlay() { } }
        """;

    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string message) { if (!valid) throw new InvalidOperationException("Generated composition: " + message); count++; }
        string original = File.ReadAllText(SourceId);
        foreach (var (offset, reject, asyncSeed) in new[] { (0, 0, 0), (16, 0, 0), (16, 1, 0), (16, 2, 0), (16, 3, 0), (16, 0, 26) })
        {
            string source = original.Replace("const int CodeOffset = 0;", $"const int CodeOffset = {offset};", StringComparison.Ordinal)
                .Replace("const int RejectBegin = 0;", $"const int RejectBegin = {reject};", StringComparison.Ordinal)
                .Replace("const int RejectAsyncSeed = 0;", $"const int RejectAsyncSeed = {asyncSeed};", StringComparison.Ordinal);
            Reference(source, offset, reject, asyncSeed, Check);
            var semantic = Analyze(source);
            Check(semantic.SchemaVersion == 56 && semantic.SemanticVersion == "1.65"
                && SemanticStaticInitializationValidator.IsValid(semantic)
                && SemanticAsyncVoidErrorOwnerValidator.IsValid(semantic)
                && semantic.UeTypeDeclarations.Count == 2, "complete original UE/static/async/token contract");
            byte[] bytes = SemanticSerializer.Serialize(semantic);
            string hash = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
            Check(!CSharpGuestCompiler.Compile(semantic, hash).Succeeded, "legacy compilation cannot claim composed execution");
            var compiled = CSharpGuestCompiler.Compile(semantic, hash, boundedLanguageErrors: true);
            Check(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(d => d.Message)));
            GuestModule module = compiled.Module!;
            Check(module.Provenance.SemanticSchemaVersion == 56 && module.Provenance.SemanticVersion == "1.65"
                && module.StaticStorage is not null && module.CancellationTokens is not null
                && module.AsyncVoidErrorOwners is not null, "original provenance and execution plans survive preparation");
            var state = CSharpGuestStateSchemaProjector.Project(semantic, module);
            Check(state.OwnerTypeId == "execution_domain:" + module.ModuleId && state.Slots.Count == 3
                && state.Slots.All(slot => slot.StableId.StartsWith("state:" + state.OwnerTypeId + ":type:global::ReloadFlow:", StringComparison.Ordinal))
                && state.Slots.All(slot => !slot.StableId.EndsWith(":CandidateBegins", StringComparison.Ordinal)),
                "persistent shared helper storage is represented once and declared transient attempts are excluded");
            Check(CSharpGuestStateSchemaSerializer.Serialize(state).SequenceEqual(
                CSharpGuestStateSchemaSerializer.Serialize(CSharpGuestStateSchemaProjector.Project(semantic, module))),
                "domain state projection is deterministic");
            var shell = UeTypeShellGenerator.Generate(bytes, "AvidScriptGenerated", "5.8", allowBoundedLanguageErrors: true);
            Check(shell.Manifest.Types.Count == 2 && shell.Manifest.SemanticSchemaVersion == 56
                && shell.Manifest.SemanticVersion == "1.65" && shell.Manifest.SemanticArtifactSha256 == hash,
                "native shell preserves the exact original composed source artifact");
            Check(shell.Files.All(file => UeTypeShellGenerator.Generate(bytes, "AvidScriptGenerated", "5.8", true)
                .Files[file.Key].SequenceEqual(file.Value)), "native shell output is deterministic");
            var unrelatedError = semantic with { Diagnostics = semantic.Diagnostics.Select(diagnostic =>
                diagnostic.Severity == "error" ? diagnostic with { Code = "ASCS9999" } : diagnostic).ToArray() };
            bool rejected = false;
            try { UeTypeShellGenerator.Generate(SemanticSerializer.Serialize(unrelatedError), "AvidScriptGenerated", "5.8", true); }
            catch (InvalidOperationException error) when (error.Message.Contains("without unrelated errors", StringComparison.Ordinal))
            { rejected = true; }
            Check(rejected, "paired preview diagnostics cannot authorize an unrelated source error");
            var reflected = semantic.UeTypeDeclarations.SelectMany(type => type.Functions).ToArray();
            Check(reflected.All(function => module.Exports.Any(export => export.Name ==
                SemanticUeTypeRuntimeContract.GetFunctionExportName(function.MethodSymbolId))), "every original reflected route is executable");
            Check(module.Imports.Any(import => import.Name.EndsWith("_require_v1", StringComparison.Ordinal))
                && module.Functions.SelectMany(function => function.Blocks).SelectMany(block => block.Instructions)
                    .Any(instruction => instruction.TargetId?.StartsWith("import:$ue:receiver:", StringComparison.Ordinal) == true),
                "UE receivers retain Host identity checks instead of managed null guards");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded && wasm.Bytes.Length > 8, "ordinary source emits executable WASM");
            Check(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes.SequenceEqual(wasm.Bytes),
                "composed artifacts round trip deterministically");
            Check(!CSharpGuestCompiler.Compile(semantic with { UeTypeDeclarations = Array.Empty<SemanticUeTypeDeclaration>() }, hash,
                boundedLanguageErrors: true).Succeeded, "static preparation cannot discard the method catalog's UE declarations");
            Check(!CSharpGuestCompiler.Compile(semantic with { StaticInitialization = null }, hash,
                boundedLanguageErrors: true).Succeeded, "missing static ownership is rejected");
        }
        CheckStateVariants(original, Check);
        CheckManagedReceiver(Check);
        return count;
    }

    private static void CheckStateVariants(string source, Action<bool, string> check)
    {
        source = source.Replace("CandidateBegins++; return ++BeginCount;",
            "CandidateBegins++; Cache<int>.Count++; Cache<long>.Count += 2; return ++BeginCount;", StringComparison.Ordinal)
            + "\npublic static class Cache<T> { public static int Count; }\n";
        var semantic = Analyze(source);
        string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
        var compiled = CSharpGuestCompiler.Compile(semantic, hash, boundedLanguageErrors: true);
        check(compiled.Succeeded, "closed shared storage compilation: " + string.Join(" | ", compiled.Diagnostics.Select(d => d.Message)));
        var state = CSharpGuestStateSchemaProjector.Project(semantic, compiled.Module!);
        var caches = state.Slots.Where(slot => slot.StableId.EndsWith(":Count", StringComparison.Ordinal)).ToArray();
        check(state.Slots.Count == 5 && caches.Length == 2 && caches[0].Offset != caches[1].Offset
            && caches[0].StableId != caches[1].StableId, "closed generic static owners retain separate migration identities and actual slots");
        var regressed = semantic with { StateContracts = semantic.StateContracts!.Select(contract =>
            contract.OwnerTypeId == "type:global::ReloadFlow" ? contract with { Version = 2 } : contract).ToArray() };
        try { CSharpGuestStateSchemaProjector.Project(regressed, compiled.Module!); }
        catch (InvalidDataException error) when (error.Message.StartsWith("ASSTATE1003:", StringComparison.Ordinal))
        { check(true, "different shared owner versions cannot hide a regression"); return; }
        throw new InvalidOperationException("Generated domain accepted differing owner contract versions.");
    }

    private static void CheckManagedReceiver(Action<bool, string> check)
    {
        const string source = """
            using System;
            using System.Threading.Tasks;
            using System.Runtime.InteropServices;
            using AvidScript;
            public class Worker {
                public async Task<int> Run() { await AvidContinuations.NextTickAsync(); return 7; }
            }
            public static class Script {
                public static int Result;
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static void BeginPlay() {
                    try { Worker worker = null; Task<int> pending = worker.Run(); Result = 1; }
                    catch (NullReferenceException) { Result = 90; }
                }
            }
            """;
        var semantic = Analyze(source);
        var compiled = CSharpGuestCompiler.Compile(semantic,
            Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant(), boundedLanguageErrors: true);
        check(compiled.Succeeded, "synchronous managed Task call uses a language null guard: "
            + string.Join(" | ", compiled.Diagnostics.Select(d => d.Message))
            + $"; Semantic={semantic.SchemaVersion}/{semantic.SemanticVersion}, static={SemanticStaticInitializationValidator.IsValid(semantic)}, "
            + $"UE={SemanticUeTypeContractValidator.TryValidate(semantic, out var typeError)} ({typeError}), catalog={SemanticUeMethodCatalogValidator.IsValid(semantic)}"
            + "; " + string.Join(" | ", semantic.Diagnostics.Select(d => d.Code + ": " + d.Message)));
        check(compiled.Module!.Functions.Any(function => function.Id.StartsWith("function:$async:task_receiver:", StringComparison.Ordinal)),
            "managed reference failure remains distinct from checked UE handles");
    }

    private static SemanticDocument Analyze(string source) => SemanticAnalyzer.Analyze(source, SourceId,
        FrontendAnalyzer.Analyze(source, SourceId).Source.Sha256,
        new[] { new SemanticReferenceSource(CSharpGuestCancellationTokenTests.AsyncFacade + Declarations,
            "generated://Gameplay.cs", true) }, new SemanticCompilerWorkspace(),
        enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
        enableAsyncSynchronousExceptions: true, enableStaticInitialization: true, enableAsyncCatchVariables: true,
        enableCancellationTokens: true, enableAsyncVoidErrorOwner: true);

    private sealed class ReferenceContext : SynchronizationContext
    {
        private readonly System.Collections.Generic.Queue<(SendOrPostCallback, object?)> pending = new();
        public int Failures { get; private set; }
        public override void Post(SendOrPostCallback callback, object? state) => pending.Enqueue((callback, state));
        public void Drain()
        {
            while (pending.TryDequeue(out var item))
            {
                try { item.Item1(item.Item2); }
                catch (InvalidOperationException) { Failures++; }
            }
        }
    }

    private static void Reference(string source, int offset, int reject, int asyncSeed, Action<bool, string> check)
    {
        const string host = """
            using System.Threading;
            using System.Threading.Tasks;
            namespace AvidScript;
            [System.AttributeUsage(System.AttributeTargets.Field)] public sealed class AvidTransientAttribute : System.Attribute { }
            public readonly struct AvidCancellationToken {
                private readonly CancellationToken token;
                public AvidCancellationToken(CancellationToken value) { token = value; }
                public static implicit operator CancellationToken(AvidCancellationToken value) => value.token;
            }
            public readonly struct AvidCancellationSource {
                private readonly CancellationTokenSource source;
                private AvidCancellationSource(CancellationTokenSource value) { source = value; }
                public static AvidCancellationSource Create() => new(new CancellationTokenSource());
                public AvidCancellationToken Token => new(source.Token);
                public void Release() => source.Dispose();
            }
            public static class AvidContinuations {
                private static readonly System.Collections.Generic.Queue<TaskCompletionSource> pending = new();
                public static Task NextTickAsync() => DelayAsync(0);
                public static Task DelayAsync(float seconds) { TaskCompletionSource next = new(); pending.Enqueue(next); return next.Task; }
                public static void Advance() { if (pending.TryDequeue(out var next)) next.SetResult(); }
            }
            public static class CancellationExtensions {
                public static Task WithCancellation(this Task task, CancellationToken token) => task.WaitAsync(token);
            }
            """;
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("GeneratedReloadReference", new[] {
            CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(host + Declarations) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("generated-reload-reference", isCollectible: true);
        var priorContext = SynchronizationContext.Current;
        var referenceContext = new ReferenceContext();
        try
        {
            SynchronizationContext.SetSynchronizationContext(referenceContext);
            var assembly = context.LoadFromStream(bytes);
            var types = new[] { assembly.GetType("ReloadFlowActor")!, assembly.GetType("ReloadFlowActor")!, assembly.GetType("ReloadFlowComponent")! };
            object[] objects = types.Select(type => Activator.CreateInstance(type)!).ToArray();
            for (int i = 0; i < objects.Length; ++i)
            {
                var type = types[i];
                type.GetProperty("Value")!.SetValue(objects[i], 10 * (i + 1));
                bool failed = false;
                try { type.GetMethod("BeginPlay", BindingFlags.Instance | BindingFlags.NonPublic)!.Invoke(objects[i], null); }
                catch (TargetInvocationException error) when (error.InnerException is InvalidOperationException) { failed = true; }
                check(failed == (reject == i + 1), "natural synchronous failure occurs after owner effects at the declared ordinal");
                check((int)type.GetProperty("Value")!.GetValue(objects[i])! == 10 * (i + 1) + offset
                    && (int)type.GetProperty("Stage")!.GetValue(objects[i])! == 1, "candidate really writes properties and suspends before failure");
            }
            var flow = assembly.GetType("ReloadFlow")!;
            check((int)flow.GetField("BeginCount")!.GetValue(null)! == 3, "Actor and Component share actual C# static storage");
            var advance = assembly.GetType("AvidScript.AvidContinuations")!.GetMethod("Advance")!;
            for (int round = 0; round < 20; ++round) { advance.Invoke(null, null); referenceContext.Drain(); }
            for (int i = 0; i < objects.Length; ++i)
            {
                bool failed = asyncSeed != 0 && 10 * (i + 1) + offset == asyncSeed;
                check((int)types[i].GetProperty("Value")!.GetValue(objects[i])! == (failed ? 10 * (i + 1) + offset : 10 * (i + 1) + 7 + offset * 2)
                    && (int)types[i].GetProperty("Stage")!.GetValue(objects[i])! == (failed ? 1 : 2),
                    "same-source result or natural async void failure after two waits");
            }
            check(referenceContext.Failures == (asyncSeed == 0 ? 0 : 1), "natural async void failure reaches its .NET execution owner once");
            check((int)flow.GetField("Completed")!.GetValue(null)! == 3 && (int)flow.GetField("Cleaned")!.GetValue(null)! == 3,
                "same-source cleanup runs once per Task");
        }
        finally { SynchronizationContext.SetSynchronizationContext(priorContext); context.Unload(); }
    }
}
