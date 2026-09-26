using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using System.Threading.Tasks;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestTaskLocalLifetimeTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string message) { if (!valid) throw new InvalidOperationException(message); count++; }
        GuestModule? baseline = null;
        GuestModule? scoped = null;
        foreach (var scenario in new[] {
            ("replace", "Task<int> pending = First(1); Task<int> saved = pending; pending = Second(2); pending = pending; int a = await saved; int b = await pending; return a + b;", 13),
            ("branch", "Task<int> pending; if (limit > 0) pending = First(limit); else pending = Second(2); int result = await pending; return result;", 3),
            ("for", "int total = 0; Task<int> saved = First(9); for (int i = 0; i < limit; i++) { Task<int> iteration = Second(i); saved = iteration; int value = await iteration; total += value; if (i == 0) continue; if (i == 2) break; } int final = await saved; return total + final;", 45),
            ("while", "int total = 0; while (total < limit) { Task<int> iteration = First(total); int value = await iteration; total += value + 1; } return total;", 3),
            ("do", "int total = 0; do { Task<int> iteration = First(total); int value = await iteration; total += value + 1; } while (total < limit); return total;", 3),
            ("nested", "int total = 0; Task<int> pending = First(1); { Task<int> saved = pending; pending = Second(2); int result = await saved; total += result; } int final = await pending; return total + final;", 13),
            ("unused", "Task<int> pending = First(1); pending = Second(2); return 7;", 7),
        })
        {
            foreach (bool deferred in new[] { false, true })
            {
                string name = scenario.Item1 + (deferred ? "-deferred" : "-ready");
                string source = Source(scenario.Item2, deferred, errors: false);
                Check(Reference(source) == (scenario.Item3, 0), name + " same-source .NET result and cleanup count");
                var module = Compile(source, name, errors: false, cancellation: false);
                baseline ??= module;
                if (module.TaskLocalLifetimes!.Functions.Any(function => function.ScopeExits.Count > 0)) scoped ??= module;
                Check(module.TaskLocalLifetimes is { ExceptionModel: "none", Functions.Count: > 0 }, name + " ownership plan");
                Check(module.TaskLocalLifetimes!.Functions.SelectMany(function => function.Releases).Any(), name + " retirement sites");
                Save(module, name, scenario.Item3);
            }
        }
        foreach (bool deferred in new[] { false, true })
        foreach (bool cancellation in new[] { false, true })
        {
            const string body = "Task<int> pending = First(1); try { for (int i = 0; i < limit; i++) { Task<int> iteration = First(-1); pending = iteration; int value = await iteration; } return 0; } catch (Exception) { return 9; } finally { Cleanups++; }";
            string name = "fault-" + (deferred ? "deferred" : "ready") + (cancellation ? "-cancel" : "");
            string source = Source(body, deferred, errors: true);
            Check(Reference(source) == (9, 1), name + " same-source .NET catch/finally");
            var module = Compile(source, name, errors: true, cancellation);
            Check(module.LanguageErrorCatalog is not null && module.AsyncExceptionRoutes is not null, name + " preserves exception routes");
            Save(module, name, 9);
            const string timerBody = "Task<int> pending = First(1); try { { Task<int> iteration = Second(2); pending = iteration; await AvidContinuations.NextTickAsync(); } int value = await pending; return value; } catch (Exception) { return 9; } finally { Cleanups++; }";
            string timerSource = Source(timerBody, deferred, errors: true);
            Check(Reference(timerSource) == (12, 1), "timer-" + name + " same-source .NET result and cleanup count");
            var timer = Compile(timerSource, "timer-" + name, errors: true, cancellation);
            Check(timer.DirectAwaitRoutes is { Count: > 0 }, "direct resume retains its status route before retiring locals");
            Save(timer, "timer-" + name, 12);
        }

        var valid = baseline!;
        var owner = valid.TaskLocalLifetimes!.Functions.First(function => function.Releases.Count > 0);
        var site = owner.Releases[0];
        GuestModule ChangeBlock(Func<GuestBasicBlock, GuestBasicBlock> change) => valid with {
            Functions = valid.Functions.Select(function => function.Id != owner.FunctionId ? function : function with {
                Blocks = function.Blocks.Select(block => block.Id == site.BlockId ? change(block) : block).ToArray() }).ToArray() };
        void Reject(GuestModule malformed, string name)
        {
            var validation = GuestModuleValidator.Validate(malformed);
            Check(!validation.Succeeded && validation.Diagnostics.Any(diagnostic => diagnostic.Code == "ASIR1033"), name);
            Check(!WasmModuleCompiler.Compile(malformed).Succeeded, name + " rejected by backend");
        }
        Reject(valid with { TaskLocalLifetimes = null }, "missing ownership plan");
        Reject(valid with { SchemaVersion = 24, IrVersion = "1.23" }, "downgraded IR");
        Reject(valid with { Provenance = valid.Provenance with { SemanticVersion = "1.53" } }, "mismatched source contract");
        Reject(valid with { TaskLocalLifetimes = valid.TaskLocalLifetimes with { Functions = new[] { owner, owner } } }, "duplicate function metadata");
        Reject(valid with { TaskLocalLifetimes = valid.TaskLocalLifetimes with { Functions = valid.TaskLocalLifetimes.Functions.Where(function => function != owner).ToArray() } }, "missing function lifetime plan");
        Reject(valid with { TaskLocalLifetimes = valid.TaskLocalLifetimes with { Functions = valid.TaskLocalLifetimes.Functions.Select(function =>
            function == owner ? function with { Releases = function.Releases.Skip(1).ToArray() } : function).ToArray() } }, "unlisted release");
        Reject(ChangeBlock(block => block with { Instructions = block.Instructions.Where(instruction =>
            !(instruction.Op == "call" && instruction.TargetId == GuestTaskLocalLifetimeValidator.ReleaseFunctionId
                && instruction.OperandIds.Contains(site.TokenRegisterId))).ToArray() }), "missing release instruction");
        Reject(ChangeBlock(block => block with { Instructions = block.Instructions.Where(instruction =>
            !(instruction.Op == "local_store" && instruction.TargetId == site.OwnerLocalId
                && instruction.OperandIds.Contains(site.ClearedValueRegisterId))).ToArray() }), "missing zero store");
        Reject(ChangeBlock(block => block with { Instructions = block.Instructions.Select(instruction =>
            instruction.Op == "call" && instruction.TargetId == GuestTaskLocalLifetimeValidator.ReleaseFunctionId
                ? instruction with { OperandIds = new[] { site.TokenRegisterId, site.TokenRegisterId } } : instruction).ToArray() }), "malformed release arity fails without throwing");
        foreach (string guardId in new[] { GuestTaskLocalLifetimeValidator.ReleaseFunctionId,
            GuestTaskLocalLifetimeValidator.RetainFunctionId, GuestTaskLocalLifetimeValidator.TransferFunctionId })
        {
            Reject(valid with { Functions = valid.Functions.Select(function => function.Id != guardId ? function : function with {
                Blocks = function.Blocks.Select(block => block.Id != "failed" ? block : block with {
                    Terminator = new("return", null, null, null, null) }).ToArray() }).ToArray() }, "ignored Host rejection: " + guardId);
            Reject(valid with { Functions = valid.Functions.Select(function => function.Id != guardId ? function : function with {
                Blocks = function.Blocks.Select(block => block.Id == "failed" ? function.Blocks[0] : block).ToArray() }).ToArray() }, "duplicate guard blocks fail without throwing");
        }
        var scopeOwner = scoped!.TaskLocalLifetimes!.Functions.First(function => function.ScopeExits.Count > 0);
        var edge = scopeOwner.ScopeExits[0];
        Reject(scoped with { Functions = scoped.Functions.Select(function => function.Id != scopeOwner.FunctionId ? function : function with {
            Blocks = function.Blocks.Select(block => block.Id != edge.SourceBlockId ? block : block with {
                Terminator = block.Terminator with {
                    TargetBlockId = block.Terminator.TargetBlockId == edge.ExitBlockId ? edge.TargetBlockId : block.Terminator.TargetBlockId,
                    FalseTargetBlockId = block.Terminator.FalseTargetBlockId == edge.ExitBlockId ? edge.TargetBlockId : block.Terminator.FalseTargetBlockId } }).ToArray() }).ToArray() }, "bypassing a scope retirement block");
        return count;

        GuestModule Compile(string source, string name, bool errors, bool cancellation)
        {
            string sourceId = "Scripts/TaskLifetime_" + name + ".cs";
            var frontend = FrontendAnalyzer.Analyze(source, sourceId);
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, frontend.Source.Sha256, new[] {
                new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CancelResumeFacade,
                    "generated://AvidScript.Continuations.generated.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: errors, enableDirectAwaitCleanup: errors, enableAsyncCancellationFlow: cancellation);
            Check(semantic.SchemaVersion == 45 && SemanticAsyncTaskLocalLifetimeValidator.IsValid(semantic), name + " source contract: "
                + string.Join(" | ", semantic.Diagnostics.Select(diagnostic => diagnostic.Message)));
            var lowering = CSharpGuestLowerer.Lower(semantic, new string('d', 64), enableAsyncLanguageErrors: errors);
            Check(lowering.Succeeded, name + ": " + string.Join(" | ", lowering.Diagnostics.Select(diagnostic => diagnostic.Message)));
            var module = lowering.Module!;
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(diagnostic => diagnostic.Message)));
            Check(wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes), name + " deterministic round trip");
            return module;
        }
    }

    private const string CancelResumeFacade = """

        internal static class CancelResumeHost {
            [System.Runtime.InteropServices.DllImport("avidscript", EntryPoint = "avid_continuation_delay_cancel_resume_v1")]
            internal static extern long Delay(float seconds, int callbackId);
        }
        """;

    private static (int Result, int Cleanups) Reference(string source)
    {
        // Only the engine scheduling adapter differs. The complete user source,
        // including task aliases, loops, handlers and finally bodies, is shared.
        const string facade = "namespace AvidScript { public static class AvidContinuations { public static System.Threading.Tasks.Task NextTickAsync() => System.Threading.Tasks.Task.Delay(1); } }";
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("TaskLifetimeReference", new[] {
            CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(facade) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("task-lifetime-reference", isCollectible: true);
        try
        {
            var script = context.LoadFromStream(bytes).GetType("Script")!;
            var task = (Task<int>)script.GetMethod("Run")!.Invoke(null, new object[] { 3 })!;
            if (!task.Wait(TimeSpan.FromSeconds(5))) throw new InvalidOperationException("Task lifetime .NET reference timed out.");
            return (task.GetAwaiter().GetResult(), (int)script.GetField("Cleanups")!.GetValue(null)!);
        }
        finally { context.Unload(); }
    }

    private static string Source(string body, bool deferred, bool errors) =>
        "using AvidScript; using System; using System.Runtime.InteropServices; using System.Threading.Tasks; public static class Script { "
        + "public static int Result; public static int Cleanups; "
        + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static async void BeginPlay() { Result = await Run(3); } "
        + "public static async Task<int> First(int value) { " + (deferred ? "await AvidContinuations.NextTickAsync(); " : "")
        + (errors ? "if (value < 0) throw new InvalidOperationException(); " : "") + "return value; } "
        + "public static async Task<int> Second(int value) { " + (deferred ? "await AvidContinuations.NextTickAsync(); " : "")
        + "return value + 10; } public static async Task<int> Run(int limit) { " + body + " } }";

    private static void Save(GuestModule module, string name, int expected)
    {
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_TASK_LIFETIME_FIXTURE_DIR");
        if (string.IsNullOrWhiteSpace(output)) return;
        Directory.CreateDirectory(output);
        string stem = Path.Combine(output, "task-lifetime-" + name);
        File.WriteAllBytes(stem + ".wasm", WasmModuleCompiler.Compile(module).Bytes);
        File.WriteAllBytes(stem + ".guest-ir.json", GuestIrSerializer.Serialize(module));
        File.WriteAllText(stem + ".module-id", module.ModuleId);
        File.WriteAllText(stem + ".result-offset", module.MemoryLayout.StateSlots.Single(slot =>
            slot.GlobalId.Contains(".Result:", StringComparison.Ordinal)).Offset.ToString(CultureInfo.InvariantCulture));
        File.WriteAllText(stem + ".expected", expected.ToString(CultureInfo.InvariantCulture));
        var cleanup = module.MemoryLayout.StateSlots.SingleOrDefault(slot => slot.GlobalId.Contains(".Cleanups:", StringComparison.Ordinal));
        if (cleanup is not null)
            File.WriteAllText(stem + ".cleanup-offset", cleanup.Offset.ToString(CultureInfo.InvariantCulture));
    }
}
