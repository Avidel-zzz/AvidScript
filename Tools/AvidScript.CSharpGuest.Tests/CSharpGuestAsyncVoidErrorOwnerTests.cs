using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestAsyncVoidErrorOwnerTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason)
        {
            if (!valid) throw new InvalidOperationException("Async void owner: " + reason);
            count++;
        }
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ASYNC_VOID_OWNER_FIXTURE_DIR");
        var fixtures = new List<object>();
        foreach (var scenario in new[] {
            ("before-await", "Sync(-1); int value = await Read(3); Trace = 9;", "ArgumentException", 0),
            ("after-await", "int value = await Read(3); Sync(-value); Trace = 9;", "ArgumentException", 0),
            ("child-failure", "int value = await Read(-1); Trace = 9;", "InvalidOperationException", 0),
            ("throw", "int value = await Read(3); throw new ArgumentException();", "ArgumentException", 0),
            ("caught", "try { int value = await Read(3); Sync(-value); } catch (ArgumentException error) { Trace = error != null ? 1 : 9; }", "", 1),
            ("anonymous-catch", "try { int value = await Read(3); Sync(-value); } catch { Trace = 1; }", "", 1),
            ("rethrow", "try { int value = await Read(3); Sync(-value); } catch (ArgumentException error) { Trace = error != null ? 1 : 9; throw; } finally { Trace = Trace * 10 + 2; }", "ArgumentException", 12),
            ("finally", "try { int value = await Read(3); Sync(-value); } finally { Trace = Trace * 10 + 1; }", "ArgumentException", 1),
            ("replace", "try { try { int value = await Read(-1); } finally { Trace = 1; Sync(-1); } } finally { Trace = Trace * 10 + 2; }", "ArgumentException", 12),
            ("aliases", "Task<int> pending = Read(3); Task<int> alias = pending; int value = await pending; Sync(-value); int other = await alias; Trace = 9;", "ArgumentException", 0),
        })
        foreach (bool deferred in new[] { false, true })
            Compile(scenario.Item1 + (deferred ? "-deferred" : "-ready"), scenario.Item2, deferred, false,
                scenario.Item3, scenario.Item4);
        Compile("direct-cancel", "await AvidContinuations.NextTickAsync(); Trace = 9;", true, true, "TaskCanceledException", 0);
        Compile("cancel-finally", "try { await AvidContinuations.NextTickAsync(); Trace = 9; } finally { Trace = 1; }", true, true, "TaskCanceledException", 1);
        Compile("cancel-replace", "try { await AvidContinuations.NextTickAsync(); Trace = 9; } finally { Trace = 1; Sync(-1); }", true, true, "ArgumentException", 1);
        Compile("direct-success", "await AvidContinuations.NextTickAsync(); Trace = 1;", true, false, "", 1);
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;

        void Compile(string name, string body, bool deferred, bool cancel, string errorType, int trace)
        {
            string source = Source(body, deferred);
            var expected = Reference(source, cancel);
            Check(expected == (errorType, trace), name + " identical-source .NET unhandled error / cleanup order: " + expected);
            string id = "Scripts/AsyncVoidOwner_" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, id, FrontendAnalyzer.Analyze(source, id).Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
                enableAsyncSynchronousExceptions: true, enableAsyncVoidErrorOwner: true);
            Check(SemanticContract.HasAsyncVoidErrorOwner(semantic) && SemanticAsyncInvocationValidator.IsValid(semantic),
                name + " source: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            Check(CSharpLanguageErrorCompiler.TryLower(semantic, hash, out var compiled, out string? error) && compiled is not null,
                name + " lowering: " + error);
            var module = compiled!.Module;
            Check(GuestAsyncVoidErrorOwners.HasSourceContract(module) && GuestModuleValidator.Validate(module).Succeeded,
                name + " independently validated IR36");
            Check(!CSharpGuestLowerer.Lower(semantic, hash, enableAsyncLanguageErrors: true).Succeeded,
                name + " private compiler admission did not escape to the ordinary public lowerer");
            var owner = module.AsyncVoidErrorOwners!.Owners.Single();
            Check(module.Functions.Single(function => function.Id == owner.MethodFunctionId).ReturnTypeId == "type:void"
                && module.Functions.Where(function => owner.Reports.Any(report => report.FunctionId == function.Id))
                    .All(function => !function.Locals.Any(local => local.Id ==
                        "value:local:$async:producer_task:" + owner.MethodFunctionId[9..])),
                name + " async void has no public producer Task");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            byte[] json = GuestIrSerializer.Serialize(module);
            var restored = GuestIrSerializer.Deserialize(json);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(restored)) && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(restored).Bytes),
                name + " deterministic byte round trip");
            if (name == "after-await-deferred") CheckMutants(module, Check);
            if (string.IsNullOrWhiteSpace(directory)) return;
            Directory.CreateDirectory(directory);
            File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
            File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), SemanticSerializer.Serialize(semantic));
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), json);
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            fixtures.Add(new { name, moduleId = module.ModuleId, cancel, errorType, trace,
                traceOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Trace:", StringComparison.Ordinal)).Offset });
        }
    }

    private static string Source(string body, bool deferred) =>
        "using AvidScript; using System; using System.Threading.Tasks; using System.Runtime.InteropServices; public static class Script { "
        + "public static int Trace; [UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static void BeginPlay() { Run(); } "
        + "public static async void Run() { " + body + " } "
        + "public static async Task<int> Other(int value) { if (value > 0) return 7; return 8; } "
        + (body.Contains("Sync(", StringComparison.Ordinal)
            ? "public static int Sync(int value) { if (value < 0) throw new ArgumentException(); return value; } " : "")
        + (body.Contains("Read(", StringComparison.Ordinal)
            ? "public static async Task<int> Read(int value) { " + (deferred ? "await AvidContinuations.NextTickAsync(); " : "")
                + "if (value < 0) throw new InvalidOperationException(); return value; } " : "") + "}";

    private static (string Error, int Trace) Reference(string source, bool cancel)
    {
        const string facade = """
            namespace AvidScript { public static class AvidContinuations {
                public static bool Cancel;
                private static readonly System.Collections.Generic.Queue<System.Threading.Tasks.TaskCompletionSource> Pending = new();
                public static System.Threading.Tasks.Task NextTickAsync() {
                    var completion = new System.Threading.Tasks.TaskCompletionSource();
                    Pending.Enqueue(completion); return completion.Task;
                }
                public static bool Step() {
                    if (!Pending.TryDequeue(out var completion)) return false;
                    if (Cancel) { Cancel = false; completion.SetCanceled(); } else completion.SetResult();
                    return true;
                }
            } }
            """;
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("AsyncVoidOwnerReference", new[] {
            CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(facade) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var assemblyContext = new AssemblyLoadContext("async-void-owner-reference", isCollectible: true);
        var previous = SynchronizationContext.Current;
        var pump = new ErrorPump();
        try
        {
            SynchronizationContext.SetSynchronizationContext(pump);
            var assembly = assemblyContext.LoadFromStream(bytes);
            var scheduler = assembly.GetType("AvidScript.AvidContinuations")!;
            scheduler.GetField("Cancel")!.SetValue(null, cancel);
            var step = scheduler.GetMethod("Step")!;
            var script = assembly.GetType("Script")!;
            script.GetMethod("Run")!.Invoke(null, null);
            for (int tick = 0; pump.Active != 0 || pump.Pending != 0; tick++)
            {
                if (tick >= 256) throw new InvalidOperationException("Async void reference exceeded its scheduler budget.");
                if (!pump.Drain() && !(bool)step.Invoke(null, null)!)
                    throw new InvalidOperationException("Async void reference stalled.");
            }
            if (pump.Errors.Count > 1) throw new InvalidOperationException("Async void reported an error more than once.");
            return (pump.Errors.SingleOrDefault()?.GetType().Name ?? "", (int)script.GetField("Trace")!.GetValue(null)!);
        }
        finally { SynchronizationContext.SetSynchronizationContext(previous); assemblyContext.Unload(); }
    }

    private sealed class ErrorPump : SynchronizationContext
    {
        private readonly Queue<(SendOrPostCallback Callback, object? State)> posts = new();
        public readonly List<Exception> Errors = new();
        public int Active { get; private set; }
        public int Pending => posts.Count;
        public override void OperationStarted() => Active++;
        public override void OperationCompleted() => Active--;
        public override void Post(SendOrPostCallback callback, object? state) => posts.Enqueue((callback, state));
        public bool Drain()
        {
            if (!posts.TryDequeue(out var post)) return false;
            try { post.Callback(post.State); } catch (Exception error) { Errors.Add(error); }
            return true;
        }
    }

    private static void CheckMutants(GuestModule valid, Action<bool, string> check)
    {
        void Reject(GuestModule mutant, string reason) => check(!GuestModuleValidator.Validate(mutant).Succeeded
            && !WasmModuleCompiler.Compile(mutant).Succeeded, reason + " must fail IR and WASM publication");
        var plan = valid.AsyncVoidErrorOwners!;
        var owner = plan.Owners.Single();
        Reject(valid with { AsyncVoidErrorOwners = null }, "missing owner plan");
        Reject(valid with { AsyncVoidErrorOwners = new(Array.Empty<GuestAsyncVoidErrorOwner>()) }, "empty owner plan");
        Reject(valid with { AsyncVoidErrorOwners = new(new[] { owner, owner }) }, "duplicate method owner");
        Reject(valid with { AsyncVoidErrorOwners = new(new[] { owner with { Reports = Array.Empty<GuestAsyncVoidErrorReport>() } }) }, "missing reports");
        Reject(valid with { AsyncVoidErrorOwners = new(new[] { owner with { Reports = owner.Reports.Skip(1).ToArray() } }) }, "unlisted report copy");
        Reject(valid with { Imports = valid.Imports.Select(import => import.Id != GuestAsyncVoidErrorOwners.ReportImportId ? import
            : import with { Name = "untrusted_report" }).ToArray() }, "noncanonical report import");
        Reject(valid with { Provenance = valid.Provenance with { SemanticSchemaVersion = 54, SemanticVersion = "1.63" } }, "forged old provenance");
        Reject(valid with { SchemaVersion = 35, IrVersion = "1.34" }, "forged old envelope");
        Reject(valid with { CapabilityManifest = valid.CapabilityManifest! with { Capabilities = valid.CapabilityManifest.Capabilities
            .Where(capability => capability.Id != GuestAsyncVoidErrorOwners.CapabilityId).ToArray() } }, "undeclared owner capability");
        var report = owner.Reports.First();
        GuestModule Edit(string blockId, Func<GuestBasicBlock, GuestBasicBlock> edit) => valid with {
            Functions = valid.Functions.Select(function => function.Id != report.FunctionId ? function : function with {
                Blocks = function.Blocks.Select(block => block.Id != blockId ? block : edit(block)).ToArray(),
            }).ToArray(),
        };
        var body = valid.Functions.Single(function => function.Id == report.FunctionId).Blocks.Single(block => block.Id == report.BlockId);
        Reject(Edit(body.Id, block => block with { Instructions = block.Instructions.Where((_, index) => index != 7).ToArray() }), "missing report");
        Reject(Edit(body.Id, block => block with { Instructions = block.Instructions.Select((instruction, index) => index != 7 ? instruction
            : instruction with { OperandIds = new[] { instruction.OperandIds[1], instruction.OperandIds[0], instruction.OperandIds[2] } }).ToArray() }), "swapped source/type tokens");
        Reject(Edit(body.Id, block => block with { Instructions = block.Instructions.Select((instruction, index) => index != 6 ? instruction
            : instruction with { OperandIds = new[] { body.Instructions[2].ResultId! } }).ToArray() }), "wrong root owner");
        Reject(Edit(body.Id, block => block with { Terminator = new("branch", null, body.Id + ":reported", null, null) }), "unchecked report");
        Reject(Edit(body.Id + ":report_rejected", block => block with { Terminator = new("branch", null, body.Id + ":reported", null, null) }), "rejected report enters release");
        Reject(Edit(body.Id + ":reported:release_error_owner", block => block with { Instructions = Array.Empty<GuestInstruction>() }), "missing owner release");
        Reject(Edit(body.Id + ":reported:owner_released", block => block with { Instructions = block.Instructions.Where(instruction =>
            instruction.TargetId != owner.OwnerLocalId).ToArray() }), "owner not cleared");
        check(!GuestModuleValidator.Validate(valid with { AsyncVoidErrorOwners = new(null!) }).Succeeded, "null owner collection fails closed");
        check(!GuestModuleValidator.Validate(valid with { AsyncVoidErrorOwners = new(new[] { owner with { Reports = null! } }) }).Succeeded,
            "null report collection fails closed");
    }
}
