using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text.Json;
using System.Threading.Tasks;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestAsyncThrowRoutingTests
{
    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        GuestModule? baseline = null;
        GuestModule? mixed = null;
        List<object> fixtures = new();
        foreach (var scenario in new[] {
            ("try", "try { int value = await Read(3); throw new ArgumentException(); } catch (ArgumentException) { return 20; } finally { Trace++; }", 20, 1),
            ("catch", "try { try { int value = await Read(-1); return value; } catch (InvalidOperationException) { Trace = Trace * 10 + 1; throw new ArgumentException(); } finally { Trace = Trace * 10 + 2; } } catch (ArgumentException) { Trace = Trace * 10 + 3; return 80; } finally { Trace = Trace * 10 + 4; }", 80, 1234),
            ("finally-return", "try { try { int value = await Read(3); return value; } finally { Trace = Trace * 10 + 1; throw new ArgumentException(); } } catch (ArgumentException) { return 90; } finally { Trace = Trace * 10 + 2; }", 90, 12),
            ("finally-fault", "try { try { int value = await Read(-1); return value; } finally { Trace = Trace * 10 + 1; throw new ArgumentException(); } } catch (InvalidOperationException) { return 0; } catch (ArgumentException) { Trace = Trace * 10 + 2; return 91; } finally { Trace = Trace * 10 + 3; }", 91, 123),
            ("rethrow", "try { try { int value = await Read(3); throw new ArgumentException(); } catch (ArgumentException) { Trace = Trace * 10 + 1; throw; } finally { Trace = Trace * 10 + 2; } } catch (ArgumentException) { return 92; } finally { Trace = Trace * 10 + 3; }", 92, 123),
            ("before-await", "try { throw new ArgumentException(); } catch (ArgumentException) { Trace++; } int value = await Read(3); return value;", 3, 1),
            ("mixed-await", "try { int first = await Read(1); throw new ArgumentException(); } catch (ArgumentException) { Trace++; } int value = await Read(3); return value;", 3, 1),
            ("repeat-fault", "Task<int> pending = Read(-1); try { try { int value = await pending; } catch (InvalidOperationException) { throw new ArgumentException(); } } catch (ArgumentException) { Trace = Trace * 10 + 1; } try { int value = await pending; return 0; } catch (InvalidOperationException) { return 96; } finally { Trace = Trace * 10 + 2; }", 96, 12),
            ("root-propagation", "try { int result = await Failing(); return result; } catch (ArgumentException) { Trace = Trace * 10 + 2; return 97; } finally { Trace = Trace * 10 + 3; }", 97, 123),
            ("aliases", "Task<int> pending = Read(3); Task<int> alias = pending; try { int value = await pending; throw new ArgumentException(); } catch (ArgumentException) { Trace++; } int result = await alias; return result;", 3, 1),
            ("loop-scope", "Task<int> pending = Read(1); try { for (int i = 0; i < 2; i++) { Task<int> local = Read(3); pending = local; int value = await local; if (value > 0) throw new ArgumentException(); } } catch (ArgumentException) { Trace++; } int result = await pending; return result;", 3, 1),
            ("direct", "try { await AvidContinuations.NextTickAsync(); throw new ArgumentException(); } catch (ArgumentException) { return 21; } finally { Trace++; }", 21, 1),
        })
        foreach (bool deferred in new[] { false, true })
            Compile(scenario.Item1 + (deferred ? "-deferred" : "-ready"), scenario.Item2, deferred, false, scenario.Item3, scenario.Item4);

        foreach (var scenario in new[] {
            ("cancel-catch", "try { try { int value = await Read(3); return value; } catch (OperationCanceledException) { Trace = Trace * 10 + 1; throw new ArgumentException(); } finally { Trace = Trace * 10 + 2; } } catch (ArgumentException) { return 93; } finally { Trace = Trace * 10 + 3; }", 93, 123),
            ("cancel-finally", "try { try { int value = await Read(3); return value; } finally { Trace = Trace * 10 + 1; throw new ArgumentException(); } } catch (OperationCanceledException) { return 0; } catch (ArgumentException) { return 94; } finally { Trace = Trace * 10 + 2; }", 94, 12),
            ("cancel-direct", "try { try { await AvidContinuations.NextTickAsync(); return 0; } catch (OperationCanceledException) { Trace = Trace * 10 + 1; throw new ArgumentException(); } finally { Trace = Trace * 10 + 2; } } catch (ArgumentException) { return 95; } finally { Trace = Trace * 10 + 3; }", 95, 123),
        }) Compile(scenario.Item1, scenario.Item2, true, true, scenario.Item3, scenario.Item4);

        var valid = baseline!;
        var transfers = valid.AsyncExceptionTransfers!;
        var route = transfers.First(item => item.Kind == "raise_exception");
        string marker = route.BlockId + ":raise_exception";
        void Reject(GuestModule candidate, string reason)
        {
            Check(!GuestModuleValidator.Validate(candidate).Succeeded, reason + " IR rejection");
            Check(!WasmModuleCompiler.Compile(candidate).Succeeded, reason + " backend rejection");
        }
        GuestModule Replace(GuestAsyncExceptionTransfer changed) => valid with {
            AsyncExceptionTransfers = transfers.Select(item => item == route ? changed : item).ToArray() };
        GuestModule Rewrite(string id, Func<GuestBasicBlock, GuestBasicBlock> change)
        {
            if (!valid.Functions.Any(function => function.Blocks.Any(block => block.Id == id)))
                throw new InvalidOperationException("Missing mutation target: " + id);
            return valid with { Functions = valid.Functions.Select(function => function with {
                Blocks = function.Blocks.Select(block => block.Id == id ? change(block) : block).ToArray() }).ToArray() };
        }
        Reject(valid with { SchemaVersion = 25, IrVersion = "1.24" }, "old IR");
        Reject(valid with { SchemaVersion = 27, IrVersion = "1.26" }, "future IR");
        Reject(valid with { IrVersion = "1.24" }, "mismatched IR");
        Reject(valid with { Provenance = valid.Provenance with { SemanticSchemaVersion = 45, SemanticVersion = "1.54" } }, "old source");
        Reject(valid with { TaskLocalLifetimes = null }, "missing ownership model");
        Reject(valid with { AsyncExceptionTransfers = transfers.Where(item => item != route).ToArray() }, "unlisted raise");
        Reject(Replace(route with { Raise = null }), "missing raise metadata");
        Reject(Replace(route with { Raise = route.Raise! with { TypeToken = int.MaxValue } }), "forged type");
        Reject(Replace(route with { Raise = route.Raise! with { SourceToken = int.MaxValue } }), "forged source");
        Reject(Replace(route with { OwnerLocalId = route.TypeLocalId }), "wrong owner slot");
        Reject(Replace(route with { TargetBlockId = route.BlockId }), "wrong successor");
        Reject(Rewrite(marker, block => block with { Terminator = block.Terminator with { ConditionValueId = "missing" } }), "unchecked Task creation");
        Reject(Rewrite(marker + ":task_created", block => block with { Instructions = block.Instructions.Where(item => item.Op != "managed_set").ToArray() }), "missing exception type");
        Reject(Rewrite(marker + ":task_created", block => block with { Terminator = block.Terminator with { FalseTargetBlockId = block.Terminator.TargetBlockId } }), "ignored fault rejection");
        Reject(Rewrite(marker + ":fault_rejected", block => block with { Instructions = Array.Empty<GuestInstruction>() }), "leaked rejected Task");
        Reject(Rewrite(marker + ":fault_acquired:release_error_owner", block => block with { Instructions = block.Instructions.Where(item => item.Op != "call").ToArray() }), "missing old owner release");
        foreach (string slot in new[] { route.OwnerLocalId, route.TypeLocalId })
            Reject(Rewrite(marker + ":fault_acquired:owner_released", block => block with { Instructions = block.Instructions.Where(item => item.TargetId != slot).ToArray() }), "missing cleared slot " + slot);
        Reject(Rewrite(marker + ":publish", block => block with { Instructions = block.Instructions.Skip(1).ToArray() }), "missing replacement owner");
        Reject(Rewrite(marker + ":publish", block => block with { Terminator = block.Terminator with { TargetBlockId = route.BlockId } }), "wrong executable successor");
        var mixedModule = mixed!;
        var protectedRoutes = mixedModule.AsyncExceptionRoutes!;
        var protectedRoute = protectedRoutes.First();
        Reject(mixedModule with { AsyncExceptionRoutes = protectedRoutes.Where(item => item != protectedRoute).ToArray() },
            "unprotected await does not permit omission of a protected route");
        var terminal = mixedModule.Functions.SelectMany(function => function.Blocks).First(block =>
            block.Id.EndsWith(":task_failed", StringComparison.Ordinal) && block.Terminator.Kind == "return"
            && block.Id.Contains(".Run(", StringComparison.Ordinal));
        Reject(mixedModule with { Functions = mixedModule.Functions.Select(function => function with {
            Blocks = function.Blocks.Select(block => block.Id != terminal.Id ? block : block with {
                Instructions = block.Instructions.Where(item => item.TargetId != "import:$async:task_propagate_failure_v1").ToArray() }).ToArray() }).ToArray() },
            "unprotected await must propagate its failure");
        Reject(mixedModule with { Functions = mixedModule.Functions.Select(function => function with {
            Blocks = function.Blocks.Select(block => block.Id != terminal.Id ? block : block with {
                Instructions = block.Instructions.Select(item => item.TargetId != "import:$async:task_propagate_failure_v1" ? item
                    : item with { OperandIds = item.OperandIds.Reverse().ToArray() }).ToArray() }).ToArray() }).ToArray() },
            "unprotected failure cannot swap source and producer");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR");
        if (!string.IsNullOrWhiteSpace(output))
            File.WriteAllText(Path.Combine(output, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;

        void Compile(string name, string body, bool deferred, bool cancel, int expected, int trace)
        {
            string source = Source(body, deferred);
            Check(Reference(source, cancel) == (expected, trace), name + " same-source .NET result/cleanup order");
            string sourceId = "Scripts/AsyncThrow_" + name + ".cs";
            var semantic = SemanticAnalyzer.Analyze(source, sourceId, FrontendAnalyzer.Analyze(source, sourceId).Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CancelFacade,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(),
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true);
            Check(semantic.SchemaVersion == 46 && SemanticAsyncInvocationValidator.IsValid(semantic),
                name + " source: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Message)));
            string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ASYNC_THROW_FIXTURE_DIR");
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
                File.WriteAllText(Path.Combine(directory, name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, name + ".semantic.json"), SemanticSerializer.Serialize(semantic));
            }
            string hash = Convert.ToHexString(SHA256.HashData(SemanticSerializer.Serialize(semantic))).ToLowerInvariant();
            Check(!CSharpGuestLowerer.Lower(semantic, hash).Succeeded, name + " requires bounded errors");
            var lowering = CSharpGuestLowerer.Lower(semantic, hash, enableAsyncLanguageErrors: true);
            Check(lowering.Succeeded, name + " lowering: " + string.Join(" | ", lowering.Diagnostics.Select(item => item.Code + ": " + item.Message)));
            var module = lowering.Module!;
            Check(module.SchemaVersion == 26 && module.IrVersion == "1.25" && module.AsyncExceptionTransfers!.Any(item => item.Raise is not null), name + " versioned raises");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            byte[] json = GuestIrSerializer.Serialize(module);
            var restored = GuestIrSerializer.Deserialize(json);
            Check(json.SequenceEqual(GuestIrSerializer.Serialize(restored)) && wasm.Bytes.SequenceEqual(WasmModuleCompiler.Compile(restored).Bytes), name + " deterministic round trip");
            baseline ??= module;
            if (name == "mixed-await-deferred") mixed = module;
            if (string.IsNullOrWhiteSpace(directory)) return;
            File.WriteAllBytes(Path.Combine(directory, name + ".wasm"), wasm.Bytes);
            File.WriteAllBytes(Path.Combine(directory, name + ".guest-ir.json"), json);
            fixtures.Add(new { name, moduleId = module.ModuleId, cancel, expected, trace,
                resultOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Result:", StringComparison.Ordinal)).Offset,
                traceOffset = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Trace:", StringComparison.Ordinal)).Offset });
        }
    }

    private const string CancelFacade = """

        internal static class CancelResumeHost {
            [System.Runtime.InteropServices.DllImport("avidscript", EntryPoint = "avid_continuation_delay_cancel_resume_v1")]
            internal static extern long Delay(float seconds, int callbackId);
        }
        """;

    private static string Source(string body, bool deferred) =>
        "using AvidScript; using System; using System.Runtime.InteropServices; using System.Threading.Tasks; public static class Script { "
        + "public static int Result; public static int Trace; "
        + "[UnmanagedCallersOnly(EntryPoint = \"avid_on_begin_play\")] public static async void BeginPlay() { Result = await Run(); } "
        + "public static async Task<int> Read(int value) { " + (deferred ? "await AvidContinuations.NextTickAsync(); " : "")
        + "if (value < 0) throw new InvalidOperationException(); return value; } "
        + "public static async Task<int> Run() { " + body + " } "
        + (body.Contains("Failing()", StringComparison.Ordinal)
            ? "public static async Task<int> Failing() { try { int value = await Read(3); return value; } finally { Trace = Trace * 10 + 1; throw new ArgumentException(); } } " : "")
        + "}";

    private static (int Result, int Trace) Reference(string source, bool cancel)
    {
        // The scheduler is the only adapter; all user code and handlers are identical.
        const string facade = """
            namespace AvidScript { public static class AvidContinuations {
                public static bool Cancel;
                public static System.Threading.Tasks.Task NextTickAsync() {
                    if (Cancel) { Cancel = false; return System.Threading.Tasks.Task.FromCanceled(new System.Threading.CancellationToken(true)); }
                    return System.Threading.Tasks.Task.Delay(1);
                }
            } }
            """;
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("AsyncThrowReference", new[] { CSharpSyntaxTree.ParseText(source), CSharpSyntaxTree.ParseText(facade) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emitted = compilation.Emit(bytes);
        if (!emitted.Success) throw new InvalidOperationException(string.Join(" | ", emitted.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("async-throw-reference", isCollectible: true);
        try
        {
            var assembly = context.LoadFromStream(bytes);
            assembly.GetType("AvidScript.AvidContinuations")!.GetField("Cancel")!.SetValue(null, cancel);
            var script = assembly.GetType("Script")!;
            var task = (Task<int>)script.GetMethod("Run")!.Invoke(null, null)!;
            if (!task.Wait(TimeSpan.FromSeconds(5))) throw new InvalidOperationException("Async throw .NET reference timed out.");
            return (task.GetAwaiter().GetResult(), (int)script.GetField("Trace")!.GetValue(null)!);
        }
        finally { context.Unload(); }
    }
}
