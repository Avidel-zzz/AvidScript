using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
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

internal static class CSharpGuestSharedTaskLifetimeTests
{
    public static int Run()
    {
        int checks = 0;
        void Check(bool valid, string message) { if (!valid) throw new InvalidOperationException(message); checks++; }
        const string sourceId = "Fixtures/Phase66/SharedTaskLifetime.cs";
        string source = File.ReadAllText(sourceId);
        foreach (int offset in new[] { 0, 16 })
        {
            string generation = source.Replace("const int CodeOffset = 0;", $"const int CodeOffset = {offset};", StringComparison.Ordinal);
            Reference(generation, offset);
            checks += 4;
            var frontend = FrontendAnalyzer.Analyze(generation, sourceId);
            var semantic = SemanticAnalyzer.Analyze(generation, sourceId, frontend.Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + GuestFacade,
                    "generated://AvidScript.Continuations.generated.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true);
            Check(semantic.SchemaVersion == 45 && SemanticAsyncTaskLocalLifetimeValidator.IsValid(semantic)
                && semantic.Diagnostics.All(d => d.Severity != "error" || d.Code == "ASCS5422"),
                "Shared Task semantic contract: " + string.Join(" | ", semantic.Diagnostics.Select(d => d.Code + ": " + d.Message)));
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            var lowered = CSharpGuestLowerer.Lower(semantic, hash, enableAsyncLanguageErrors: true);
            Check(lowered.Succeeded, "Shared Task lowering: " + string.Join(" | ", lowered.Diagnostics.Select(d => d.Code + ": " + d.Message)));
            var module = lowered.Module!;
            Check(module.SchemaVersion == 25 && module.TaskLocalLifetimes?.ExceptionModel == "cancellation",
                "Shared Task IR ownership contract");
            var compiled = WasmModuleCompiler.Compile(module);
            Check(compiled.Succeeded, "Shared Task WASM: " + string.Join(" | ", compiled.Diagnostics.Select(d => d.Message)));
            Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes),
                "Shared Task WASM deterministic round trip");
            var child = semantic.AsyncMethods.Single(method => method.MethodSymbolId.Contains("SharedTaskFlow.Child(", StringComparison.Ordinal));
            Check(child.ExceptionPlan is null && child.Segments.Count(segment => segment.AwaitSite is not null) == 1,
                "Plain child source must not acquire invented try/catch regions");
            var route = module.DirectAwaitRoutes!.Single(item => item.MethodFunctionId.Contains("SharedTaskFlow.Child(", StringComparison.Ordinal));
            void Reject(GuestModule invalid, string code, string message) => Check(
                GuestModuleValidator.Validate(invalid).Diagnostics.Any(d => d.Code == code)
                    && !WasmModuleCompiler.Compile(invalid).Succeeded, message);
            Reject(module with { DirectAwaitRoutes = module.DirectAwaitRoutes!.Where(item => item != route).ToArray() },
                "ASIR1031", "Implicit cancellation cannot omit its checked source route");
            Reject(module with { DirectAwaitRoutes = module.DirectAwaitRoutes!.Select(item => item == route
                ? item with { Cancellation = item.Cancellation! with { SourceToken = int.MaxValue } } : item).ToArray() },
                "ASIR1031", "Implicit cancellation cannot forge its source token");
            Reject(module with { AsyncExceptionTransfers = module.AsyncExceptionTransfers!.Where(item =>
                item.BlockId != route.CancellationTargetBlockId).ToArray() },
                "ASIR1033", "Implicit cancellation cannot omit producer propagation and release");
            var declaration = semantic.UeTypeDeclarations.Single();
            var entry = declaration.Functions.Single();
            var ordinals = SemanticUeTypeRuntimeContract.BuildMemberOrdinals(declaration);
            string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_SHARED_TASK_FIXTURE_DIR");
            if (string.IsNullOrWhiteSpace(directory)) continue;
            Directory.CreateDirectory(directory);
            string stem = Path.Combine(directory, "shared-task-" + offset);
            File.WriteAllBytes(stem + ".wasm", compiled.Bytes);
            File.WriteAllBytes(stem + ".semantic.json", SemanticSerializer.Serialize(semantic));
            File.WriteAllBytes(stem + ".guestir.json", GuestIrSerializer.Serialize(module));
            File.WriteAllText(stem + ".json", JsonSerializer.Serialize(new {
                module_id = module.ModuleId, wasm_sha256 = Convert.ToHexString(SHA256.HashData(compiled.Bytes)).ToLowerInvariant(),
                type_id = declaration.TypeId, method_id = entry.MethodSymbolId,
                member_ordinal = ordinals[entry.MethodSymbolId],
                export_name = SemanticUeTypeRuntimeContract.GetFunctionExportName(entry.MethodSymbolId),
                completed_offset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains("SharedTaskFlow.Completed:", StringComparison.Ordinal)).Offset,
                properties = SemanticUeTypeRuntimeContract.BuildPropertyPlans(semantic).Select(plan => new {
                    member_ordinal = plan.MemberOrdinal, stable_member_id = plan.PropertySymbolId,
                    name = declaration.Properties.Single(property => property.SymbolId == plan.PropertySymbolId).Name,
                    getter_import_name = plan.GetterImportName, setter_import_name = plan.SetterImportName
                }).ToArray(),
                imports = module.Imports.Select(import => new { module = import.Module, name = import.Name }).ToArray()
            }));
        }
        return checks;
    }

    private const string Attributes = """
        [AttributeUsage(AttributeTargets.Class)] public sealed class UClassAttribute : Attribute { }
        [AttributeUsage(AttributeTargets.Method)] public sealed class UFunctionAttribute : Attribute { }
        [AttributeUsage(AttributeTargets.Property)] public sealed class UPropertyAttribute : Attribute { }
        public abstract class AvidActor { }
        """;
    private const string GuestFacade = Attributes + """

        internal static class CancelResumeHost {
            [System.Runtime.InteropServices.DllImport("avidscript", EntryPoint = "avid_continuation_delay_cancel_resume_v1")]
            internal static extern long Delay(float seconds, int callbackId);
        }
        """;

    private static void Reference(string source, int offset)
    {
        const string adapter = """
            using System;
            using System.Threading;
            using System.Threading.Tasks;
            using System.Collections.Concurrent;
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
                private static readonly ConcurrentQueue<TaskCompletionSource> pending = new();
                public static Task NextTickAsync() {
                    TaskCompletionSource next = new(TaskCreationOptions.RunContinuationsAsynchronously);
                    pending.Enqueue(next); return next.Task;
                }
                public static void Advance() { if (pending.TryDequeue(out var next)) next.SetResult(); }
            }
            public static class CancellationExtensions {
                public static Task WithCancellation(this Task task, AvidCancellationToken token) => task.WaitAsync(token.Value);
            }
            """;
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("SharedTaskReference", new[] {
            CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(adapter + Attributes) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("shared-task-reference", isCollectible: true);
        try
        {
            var assembly = context.LoadFromStream(bytes);
            var flow = assembly.GetType("SharedTaskFlow")!;
            var advance = assembly.GetType("AvidScript.AvidContinuations")!.GetMethod("Advance")!;
            foreach (int seed in new[] { 5, 10, -1, -2 })
            {
                var task = (Task<int>)flow.GetMethod("Run")!.Invoke(null, new object[] { seed })!;
                var timer = Stopwatch.StartNew();
                while (!task.IsCompleted && timer.Elapsed < TimeSpan.FromSeconds(5)) {
                    advance.Invoke(null, null); Thread.Yield();
                }
                if (!task.IsCompleted) throw new TimeoutException("Shared Task .NET reference timed out");
                if (seed == -2) {
                    if (task.Exception?.InnerException is not DivideByZeroException) throw new InvalidOperationException("Expected division failure");
                } else if (task.GetAwaiter().GetResult() != (seed == -1 ? 90 : seed * 2 + 1 + offset * 2)) {
                    throw new InvalidOperationException("Shared Task .NET result mismatch");
                }
            }
        }
        finally { context.Unload(); }
    }
}
