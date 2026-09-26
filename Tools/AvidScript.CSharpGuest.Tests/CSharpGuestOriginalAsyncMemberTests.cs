using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.Loader;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading.Tasks;
using AvidScript.CSharpFrontend;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;

internal static class CSharpGuestOriginalAsyncMemberTests
{
    private sealed record Scenario(string Name, string Invoke, string Before = "", string After = "", string Second = "");
    private sealed record TaskObservation(string Name, string InitialState, string TerminalState, int Result, string? ErrorType);
    private sealed record ReferenceResult(SortedDictionary<string, int> Fields, TaskObservation[] Tasks,
        IReadOnlyList<SortedDictionary<string, int>> LoopSuspensions);

    private static readonly string[] BusinessFields = {
        "Trace", "ReceiverCalls", "ProducerCalls", "CleanupCount", "OriginalField", "OriginalValue", "OriginalSetters",
        "ReplacementField", "ReplacementValue", "ReplacementSetters", "TotalSetters", "LastId", "LastValue"
    };

    public static int Run()
    {
        int count = 0;
        void Check(bool valid, string reason) { if (!valid) throw new InvalidOperationException(reason); count++; }
        const string fixture = "Fixtures/Phase66/AwaitMemberAssignment.cs";
        DirectoryInfo? root = new(AppContext.BaseDirectory);
        while (root is not null && !File.Exists(Path.Combine(root.FullName, fixture))) root = root.Parent;
        Check(root is not null, "Original C10 fixture must be available");
        byte[] bytes = File.ReadAllBytes(Path.Combine(root!.FullName, fixture));
        Check(Convert.ToHexString(SHA256.HashData(bytes)).Equals(
            "265a7e71e2c744681d099ee6b11a93fcf94b151980205cddf155c46f602249d9", StringComparison.OrdinalIgnoreCase),
            "Original C10 business source must remain unchanged");
        string reference = File.ReadAllText(Path.Combine(root.FullName, "Fixtures/Phase66/AwaitMemberAssignment.Reference.cs"));
        List<Scenario> cases = new();
        foreach (bool property in new[] { false, true })
        {
            string flag = property ? "true" : "false";
            string kind = property ? "property" : "field";
            for (int mode = 0; mode < 8; ++mode)
                cases.Add(new(kind + "-mode-" + mode, $"AwaitMemberAssignment.Assign({flag}, {mode})",
                    mode == 5 ? "AwaitMemberAssignment.Lifetime.Cancel();" : "",
                    mode == 4 ? "AwaitMemberAssignment.Lifetime.Cancel();" : ""));
            cases.Add(new(kind + "-implicit", $"AwaitMemberAssignment.Original.AssignImplicit({flag})"));
            cases.Add(new(kind + "-loop", $"AwaitMemberAssignment.AssignTwice({flag})"));
            cases.Add(new(kind + "-concurrent", $"AwaitMemberAssignment.Assign({flag}, 0)",
                Second: $"AwaitMemberAssignment.Assign({flag}, 0)"));
            foreach (bool ready in new[] { false, true })
                cases.Add(new(kind + (ready ? "-task-ready" : "-task-pending"),
                    $"AwaitMemberAssignment.AssignTaskLocal({flag}, {(ready ? "true" : "false")})"));
        }
        cases.Add(new("property-setter-error", "AwaitMemberAssignment.Assign(true, 8)",
            "AwaitMemberAssignment.Original.ThrowOnSet = true;"));
        cases.Add(new("temporary", "AwaitMemberAssignment.AssignTemporary()"));
        cases.Add(new("temporary-cancel", "AwaitMemberAssignment.AssignTemporary()", After: "AwaitMemberAssignment.Lifetime.Cancel();"));
        Check(cases.Count == 29, "All original reference scenarios must be compiled");
        string? directory = Environment.GetEnvironmentVariable("AVIDSCRIPT_ORIGINAL_ASYNC_MEMBER_DIR");
        if (!string.IsNullOrWhiteSpace(directory)) Directory.CreateDirectory(directory);
        List<object> fixtures = new();
        foreach (var scenario in cases)
        {
            // Append the execution adapter, preserving every business-source byte.
            // Async executable reference units remain a separate frontend limitation.
            string source = Encoding.UTF8.GetString(bytes) + "\n" + Entry(scenario);
            bool loop = scenario.Name.EndsWith("-loop", StringComparison.Ordinal);
            var expected = Reference(source, reference, loop);
            Check(expected.Tasks.Length == (scenario.Second.Length == 0 ? 1 : 2), scenario.Name + " observes each source Task");
            Check(expected.LoopSuspensions.Count == (loop ? 2 : 0), scenario.Name + " observes both loop suspensions");
            var semantic = SemanticAnalyzer.Analyze(source, fixture, FrontendAnalyzer.Analyze(source, fixture).Source.Sha256,
                new[] { new SemanticReferenceSource(CSharpGuestContinuationTests.ReferenceFacade + CSharpGuestAsyncThrowRoutingTests.CancelFacade,
                    "generated://Continuations.cs", true) }, new SemanticCompilerWorkspace(), enableStaticInitialization: true,
                enableAsyncExceptionFlow: true, enableDirectAwaitCleanup: true, enableAsyncCancellationFlow: true,
                enableAsyncSynchronousExceptions: true);
            byte[] semanticBytes = SemanticSerializer.Serialize(semantic);
            if (!string.IsNullOrWhiteSpace(directory))
                File.WriteAllBytes(Path.Combine(directory, scenario.Name + ".semantic.json"), semanticBytes);
            Check(semantic.SchemaVersion == 51 && SemanticStaticInitializationValidator.IsValid(semantic),
                scenario.Name + " semantic: " + string.Join(" | ", semantic.Diagnostics.Select(item => item.Code + ":" + item.Message)));
            Check(semantic.AsyncMethods.SelectMany(method => method.Segments).Count(segment => segment.AwaitSite?.MemberAssignment is not null) == 9,
                scenario.Name + " must preserve all nine original member awaits");
            string hash = Convert.ToHexString(SHA256.HashData(semanticBytes)).ToLowerInvariant();
            Check(CSharpStaticInitializationCompiler.TryLower(semantic, hash, out var module, out string? error) && module is not null,
                scenario.Name + " lowering: " + error);
            Check(module!.SchemaVersion == 31 && module.DirectAwaitReadiness?.BaseSchemaVersion == 30
                && GuestModuleValidator.Validate(module).Succeeded, scenario.Name + " validated IR 31 with static async base");
            Check(module.Exports.Any(export => export.Name == "avid_on_begin_play")
                && module.Exports.Any(export => export.Name == "avid_on_tick")
                && module.Imports.Count(import => import.Module == "avidscript" && import.Name == "avid_language_error_report_v1") == 1,
                scenario.Name + " synchronous and asynchronous entries share one error-report ABI");
            var wasm = WasmModuleCompiler.Compile(module);
            Check(wasm.Succeeded, scenario.Name + " WASM: " + string.Join(" | ", wasm.Diagnostics.Select(item => item.Message)));
            byte[] ir = GuestIrSerializer.Serialize(module);
            Check(ir.SequenceEqual(GuestIrSerializer.Serialize(GuestIrSerializer.Deserialize(ir))), scenario.Name + " canonical IR");
            if (scenario.Name == "field-mode-0")
            {
                CheckRouteMutations(module, Check);
                CheckReadinessMutations(module, Check);
                CheckEntryImportComposition(module, Check);
            }
            int Offset(string name)
            {
                var field = semantic.Symbols.Single(symbol => symbol.Kind == "field" && symbol.Name == name
                    && symbol.ContainingSymbolId == "symbol:type:global::Script");
                var owner = semantic.StaticInitialization!.Types.Single(type => type.Fields.Any(item => item.FieldSymbolId == field.Id));
                string suffix = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(owner.TypeId + "\n" + field.Id))).ToLowerInvariant();
                return module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId == "global:symbol:field:$static:" + suffix).Offset;
            }
            var adapterMethod = semantic.Symbols.Single(symbol => symbol.Kind == "method" && symbol.Name == "Run"
                && symbol.ContainingSymbolId == "symbol:type:global::Script");
            var adapterAwaits = semantic.AsyncMethods.Single(method => method.MethodSymbolId == adapterMethod.Id).Segments
                .Where(segment => segment.AwaitSite?.TaskLocalSymbolId is not null).Select(segment => segment.AwaitSite!).ToArray();
            var taskObservations = expected.Tasks.Select(task => {
                var local = semantic.Symbols.Single(symbol => symbol.Kind == "local" && symbol.Name == task.Name
                    && symbol.ContainingSymbolId == adapterMethod.Id);
                int callback = adapterAwaits.Single(site => site.TaskLocalSymbolId == local.Id).CallbackId;
                Check(module.AsyncExceptionRoutes!.Count(route => route.CallbackId == callback) == 1,
                    scenario.Name + ":" + task.Name + " is identified by its validated source await route");
                // The original reference uses IsInstanceOfType: cancellation
                // accepts TaskCanceledException as an OperationCanceledException.
                Type? errorClass = task.ErrorType is null ? null : Type.GetType(task.ErrorType, throwOnError: true);
                int[] errorTypes = errorClass is null ? Array.Empty<int>() : module.LanguageErrorCatalog!.Types
                    .Where(type => type.TypeId.StartsWith("type:global::", StringComparison.Ordinal)
                        && errorClass.IsAssignableFrom(Type.GetType(type.TypeId["type:global::".Length..])))
                    .Select(type => type.Token).ToArray();
                Check(errorClass is null || errorTypes.Length > 0, scenario.Name + " source exception type is represented");
                return new { name = task.Name, callbackId = callback, initialState = task.InitialState,
                    terminalState = task.TerminalState, result = task.Result, errorType = task.ErrorType, errorTypeTokens = errorTypes };
            }).ToArray();
            int[] loopCallbacks = loop ? semantic.AsyncMethods.Single(method => method.MethodSymbolId == semantic.Symbols
                .Single(symbol => symbol.Kind == "method" && symbol.Name == "AssignTwice").Id).Segments
                .Where(segment => segment.AwaitSite?.MemberAssignment is not null)
                .Select(segment => segment.AwaitSite!.CallbackId).OrderBy(id => id).ToArray() : Array.Empty<int>();
            Check(loopCallbacks.Length == (loop ? 2 : 0) && loopCallbacks.All(callback =>
                module.AsyncExceptionRoutes!.Count(route => route.CallbackId == callback) == 1),
                scenario.Name + " loop checkpoints use validated field/property await routes");
            if (!string.IsNullOrWhiteSpace(directory))
            {
                File.WriteAllText(Path.Combine(directory, scenario.Name + ".cs"), source);
                File.WriteAllBytes(Path.Combine(directory, scenario.Name + ".guest-ir.json"), ir);
                File.WriteAllBytes(Path.Combine(directory, scenario.Name + ".wasm"), wasm.Bytes);
                fixtures.Add(new { name = scenario.Name, moduleId = module.ModuleId, staticSlots = module.StaticStorage!.Slots.Count,
                    cancel = false, expected = expected.Fields["Result"], trace = expected.Fields["Trace"], resultOffset = Offset("Result"), traceOffset = Offset("Trace"),
                    observations = expected.Fields.Select(item => new { name = item.Key, expected = item.Value, offset = Offset(item.Key) }).ToArray(),
                    taskObservations, loopCallbacks,
                    loopSuspensions = expected.LoopSuspensions.Select(fields => fields.Select(item =>
                        new { name = item.Key, expected = item.Value, offset = Offset(item.Key) }).ToArray()).ToArray() });
            }
        }
        if (!string.IsNullOrWhiteSpace(directory))
            File.WriteAllText(Path.Combine(directory, "cases.json"), JsonSerializer.Serialize(fixtures, new JsonSerializerOptions { WriteIndented = true }));
        return count;
    }

    private static string Entry(Scenario scenario) => $$"""
        public static class Script {
            public static int Result; public static int Status; public static int SecondResult;
            public static int InitialTrace; public static int InitialCleanup;
            public static int Trace; public static int ReceiverCalls; public static int ProducerCalls; public static int CleanupCount;
            public static int OriginalField; public static int OriginalValue; public static int OriginalSetters;
            public static int ReplacementField; public static int ReplacementValue; public static int ReplacementSetters;
            public static int TotalSetters; public static int LastId; public static int LastValue;
            [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
            public static async void BeginPlay() { Result = await Run(); }
            // Test-only observation at a suspension boundary; never called from an import.
            [System.Runtime.InteropServices.UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
            public static void Observe(float deltaSeconds) { Snapshot(); }
            public static async Task<int> Run() {
                AwaitMemberAssignment.Lifetime = AvidCancellationSource.Create();
                try {
                    {{scenario.Before}}
                    Task<int> pending = {{scenario.Invoke}};
                    {{(scenario.Second.Length == 0 ? "" : "Task<int> second = " + scenario.Second + ";")}}
        #if C10_REFERENCE
                    ReferenceTasks.Tasks.Add("pending", pending);
                    {{(scenario.Second.Length == 0 ? "" : "ReferenceTasks.Tasks.Add(\"second\", second);")}}
        #endif
                    InitialTrace = AwaitMemberAssignment.Trace;
                    InitialCleanup = AwaitMemberAssignment.CleanupCount;
                    {{scenario.After}}
                    int result = await pending;
                    {{(scenario.Second.Length == 0 ? "" : "SecondResult = await second;")}}
                    Status = 1;
                    return result;
                } catch (ArgumentException) { Status = 2; return 0; }
                  catch (OperationCanceledException) { Status = 3; return 0; }
                  catch (NullReferenceException) { Status = 4; return 0; }
                  catch (InvalidOperationException) { Status = 5; return 0; }
                finally { Snapshot(); AwaitMemberAssignment.Lifetime.Release(); }
            }
            public static void Snapshot() {
                    Trace = AwaitMemberAssignment.Trace;
                    ReceiverCalls = AwaitMemberAssignment.ReceiverCalls;
                    ProducerCalls = AwaitMemberAssignment.ProducerCalls;
                    CleanupCount = AwaitMemberAssignment.CleanupCount;
                    OriginalField = AwaitMemberAssignment.Original.Field;
                    OriginalValue = AwaitMemberAssignment.Original.Value;
                    OriginalSetters = AwaitMemberAssignment.Original.SetterCalls;
                    ReplacementField = AwaitMemberAssignment.Replacement.Field;
                    ReplacementValue = AwaitMemberAssignment.Replacement.Value;
                    ReplacementSetters = AwaitMemberAssignment.Replacement.SetterCalls;
                    TotalSetters = AwaitMemberAssignment.TotalSetterCalls;
                    LastId = AwaitMemberAssignment.LastAssignedId;
                    LastValue = AwaitMemberAssignment.LastAssignedValue;
            }
        }
        #if C10_REFERENCE
        public static class ReferenceTasks {
            public static readonly System.Collections.Generic.Dictionary<string, Task<int>> Tasks = new();
        }
        #endif
        """;

    private static ReferenceResult Reference(string source, string facade, bool observeLoop)
    {
        var references = ((string)AppContext.GetData("TRUSTED_PLATFORM_ASSEMBLIES")!).Split(Path.PathSeparator)
            .Select(path => MetadataReference.CreateFromFile(path));
        var compilation = CSharpCompilation.Create("OriginalMemberReference", new[] {
            CSharpSyntaxTree.ParseText(source, new CSharpParseOptions(preprocessorSymbols: new[] { "C10_REFERENCE" })),
            CSharpSyntaxTree.ParseText(facade) }, references,
            new CSharpCompilationOptions(OutputKind.DynamicallyLinkedLibrary));
        using var bytes = new MemoryStream();
        var emit = compilation.Emit(bytes);
        if (!emit.Success) throw new InvalidOperationException(string.Join(" | ", emit.Diagnostics));
        bytes.Position = 0;
        var context = new AssemblyLoadContext("original-member-reference", isCollectible: true);
        var previous = System.Threading.SynchronizationContext.Current;
        try
        {
            System.Threading.SynchronizationContext.SetSynchronizationContext(null);
            var assembly = context.LoadFromStream(bytes);
            var script = assembly.GetType("Script")!;
            var scheduler = assembly.GetType("AvidScript.AvidContinuations")!;
            var task = (Task<int>)script.GetMethod("Run")!.Invoke(null, null)!;
            var sourceTasks = (Dictionary<string, Task<int>>)assembly.GetType("ReferenceTasks")!.GetField("Tasks")!.GetValue(null)!;
            static string State(Task<int> task) => !task.IsCompleted ? "running" : task.IsCanceled ? "cancelled"
                : task.IsFaulted ? "faulted" : "succeeded";
            var initialStates = sourceTasks.ToDictionary(item => item.Key, item => State(item.Value), StringComparer.Ordinal);
            List<SortedDictionary<string, int>> loopSuspensions = new();
            void ObserveLoop()
            {
                if (!observeLoop || task.IsCompleted) return;
                script.GetMethod("Snapshot")!.Invoke(null, null);
                loopSuspensions.Add(new(BusinessFields.ToDictionary(name => name,
                    name => (int)script.GetField(name)!.GetValue(null)!), StringComparer.Ordinal));
            }
            ObserveLoop();
            for (int tick = 0; !task.IsCompleted && tick < 16; ++tick)
            {
                scheduler.GetMethod("Advance")!.Invoke(null, null);
                ObserveLoop();
            }
            if (!task.IsCompleted || (int)scheduler.GetProperty("PendingCount")!.GetValue(null)! != 0)
                throw new InvalidOperationException("Original reference left pending work");
            script.GetField("Result")!.SetValue(null, task.GetAwaiter().GetResult());
            var tasks = sourceTasks.Select(item => {
                int result = 0;
                string? error = null;
                try { result = item.Value.GetAwaiter().GetResult(); }
                catch (Exception exception) { error = exception.GetType().FullName; }
                return new TaskObservation(item.Key, initialStates[item.Key], State(item.Value), result, error);
            }).OrderBy(item => item.Name, StringComparer.Ordinal).ToArray();
            return new(new(script.GetFields(BindingFlags.Public | BindingFlags.Static)
                .ToDictionary(field => field.Name, field => (int)field.GetValue(null)!), StringComparer.Ordinal), tasks, loopSuspensions);
        }
        finally { System.Threading.SynchronizationContext.SetSynchronizationContext(previous); context.Unload(); }
    }

    private static void CheckEntryImportComposition(GuestModule module, Action<bool, string> check)
    {
        var export = module.Exports.Single(item => item.Name == "avid_on_tick");
        var adapter = module.Functions.Single(item => item.Id == export.FunctionId);
        string bodyId = adapter.Blocks.Single(block => block.Id == adapter.EntryBlockId).Instructions
            .Single(instruction => instruction.Op == "call").TargetId!;
        var originalSignature = module.Functions.Single(function => function.Id == bodyId) with { ReturnTypeId = "type:void" };
        var originals = new Dictionary<string, GuestFunction>(StringComparer.Ordinal) { [bodyId] = originalSignature };
        var exports = new[] { export with { FunctionId = bodyId } };
        var before = module with { Functions = module.Functions.Where(function => function != adapter).ToArray(),
            Exports = module.Exports.Where(item => item != export).ToArray() };
        var report = before.Imports.Single(import => import.Id == CSharpTaskResultAbi.LanguageErrorReportImportId);
        foreach (bool existing in new[] { false, true })
        {
            var input = existing ? before : before with { Imports = before.Imports.Where(import => import != report).ToArray() };
            check(CSharpLanguageErrorEntryAdapter.TryAdd(input, exports, originals, out var output, out _)
                && output is not null && GuestModuleValidator.Validate(output).Succeeded
                && WasmModuleCompiler.Compile(output).Succeeded
                && output.Imports.Count(import => import.Id == report.Id) == 1,
                "Sync entry can compose with " + (existing ? "an existing" : "a new") + " error-report import");
        }
        void Reject(IReadOnlyList<GuestImport> imports, string reason) => check(
            !CSharpLanguageErrorEntryAdapter.TryAdd(before with { Imports = imports }, exports, originals, out var output, out var error)
                && output is null && error == "The language-error report import identity is already occupied.",
            "Mixed entries reject " + reason);
        Reject(before.Imports.Append(report).ToArray(), "duplicate error-report imports");
        foreach (var collision in new[] {
            report with { Id = "import:alias" }, report with { Module = "env" }, report with { Name = "another_report" },
            report with { ReturnTypeId = "type:void" }, report with { ParameterTypeIds = new[] { "type:int32" } },
            report with { ParameterTypeIds = new[] { "type:int32", "type:int32", "type:int32" } }
        }) Reject(before.Imports.Select(import => import == report ? collision : import).ToArray(), "report ABI identity/signature collisions");
    }

    private static void CheckRouteMutations(GuestModule module, Action<bool, string> check)
    {
        var route = module.DirectAwaitRoutes!.Single();
        var stale = module with { DirectAwaitRoutes = new[] { route with { AwaitBlockId = "missing" } } };
        check(!GuestModuleValidator.Validate(stale).Succeeded && !WasmModuleCompiler.Compile(stale).Succeeded,
            "Missing scheduling block must fail closed");
        var invalid = module with { Functions = module.Functions.Select(function => {
            var schedule = function.Blocks.SingleOrDefault(block => block.Id == route.AwaitBlockId);
            string? callback = schedule?.Instructions.Single(instruction => instruction.Op == "call"
                && instruction.TargetId == route.ScheduleImportId).OperandIds[1];
            return function with { Blocks = function.Blocks.Select(block => block with {
                Instructions = block.Instructions.Select(instruction => instruction.ResultId == callback && instruction.Op == "constant"
                    ? instruction with { Constant = new("int32", (route.CallbackId + 1).ToString(System.Globalization.CultureInfo.InvariantCulture)) }
                    : instruction).ToArray() }).ToArray() };
        }).ToArray() };
        check(!GuestModuleValidator.Validate(invalid).Succeeded && !WasmModuleCompiler.Compile(invalid).Succeeded,
            "The scheduler must receive its declared callback identity");
    }

    private static void CheckReadinessMutations(GuestModule module, Action<bool, string> check)
    {
        var plan = module.DirectAwaitReadiness!;
        var guard = plan.Guards[0];
        var function = module.Functions.Single(item => item.Id == guard.FunctionId);
        var queryBlock = function.Blocks.Single(item => item.Id == guard.CheckBlockId);
        var query = queryBlock.Instructions[^3];
        string cancelCheck = queryBlock.Terminator.FalseTargetBlockId!;
        string invalidBlock = function.Blocks.Single(item => item.Id == cancelCheck).Terminator.FalseTargetBlockId!;
        var schedule = function.Blocks.Single(item => item.Id == guard.ScheduleBlockId);
        GuestModule Change(string blockId, Func<GuestBasicBlock, GuestBasicBlock> update) => module with {
            Functions = module.Functions.Select(item => item.Id != guard.FunctionId ? item : item with {
                Blocks = item.Blocks.Select(block => block.Id == blockId ? update(block) : block).ToArray() }).ToArray() };
        void Reject(GuestModule changed, string reason)
        {
            var validation = GuestModuleValidator.Validate(changed);
            check(!validation.Succeeded && validation.Diagnostics.Any(item => item.Code == "ASIR1038")
                && !WasmModuleCompiler.Compile(changed).Succeeded, "Readiness must reject " + reason);
        }
        Reject(module with { SchemaVersion = 30, IrVersion = "1.29" }, "metadata on the old version");
        Reject(module with { IrVersion = "1.29" }, "a mismatched outer version");
        Reject(module with { DirectAwaitReadiness = null }, "missing metadata");
        Reject(module with { DirectAwaitReadiness = plan with { BaseIrVersion = "1.28" } }, "a mismatched base version");
        Reject(module with { DirectAwaitReadiness = plan with { BaseSchemaVersion = 28, BaseIrVersion = "1.27" } }, "an unsupported base");
        Reject(module with { DirectAwaitReadiness = plan with { Guards = plan.Guards.Skip(1).ToArray() } }, "an unlisted query");
        Reject(module with { DirectAwaitReadiness = plan with { Guards = plan.Guards.Append(guard).ToArray() } }, "a duplicate guard");
        Reject(module with { Imports = module.Imports.Select(import => import.Id == GuestDirectAwaitReadiness.ImportId
            ? import with { Module = "env" } : import).ToArray() }, "a status import alias");
        Reject(Change(guard.CheckBlockId, block => block with {
            Instructions = block.Instructions.Where(item => item != query).ToArray() }), "a missing query");
        Reject(Change(guard.CheckBlockId, block => block with {
            Instructions = block.Instructions.Append(query).ToArray() }), "a duplicate query");
        Reject(Change(guard.CheckBlockId, block => block with {
            Instructions = block.Instructions.Select(item => item == query
                ? item with { OperandIds = new[] { schedule.Instructions[0].ResultId! } } : item).ToArray() }), "a different source token");
        Reject(Change(guard.CheckBlockId, block => block with {
            Instructions = block.Instructions.Select((item, index) => index == block.Instructions.Count - 2
                ? item with { Constant = new("int32", "2") } : item).ToArray() }), "an inverted open-status literal");
        Reject(Change(cancelCheck, block => block with {
            Terminator = block.Terminator with { TargetBlockId = guard.ScheduleBlockId } }), "cancellation routed to scheduling");
        Reject(Change(invalidBlock, block => block with {
            Terminator = new("branch", null, guard.ScheduleBlockId, null, null) }), "invalid status treated as open");
        Reject(module with { Functions = module.Functions.Select(item => item.Id != guard.FunctionId ? item : item with {
            Blocks = item.Blocks.Append(new GuestBasicBlock("readiness-test-bypass", Array.Empty<GuestInstruction>(),
                new("branch", null, guard.ScheduleBlockId, null, null))).ToArray() }).ToArray() }, "an extra edge bypassing the query");
        Reject(Change(guard.CancellationBlockId + ":cancelled_owner", block => block with {
            Terminator = block.Terminator with { TargetBlockId = guard.ScheduleBlockId } }), "cancellation bypassing source cleanup");
        Reject(Change(guard.CancellationBlockId + ":task_created", block => block with {
            Instructions = block.Instructions.Where(item => item.TargetId != "import:task_cancel_language_error_v1").ToArray() }),
            "missing typed cancellation ownership");

        byte[] bytes = GuestIrSerializer.Serialize(module);
        foreach (bool nested in new[] { false, true })
        foreach (string field in nested
            ? new[] { "function_id", "callback_id", "check_block_id", "schedule_block_id", "cancellation_block_id" }
            : new[] { "base_schema_version", "base_ir_version", "guards" })
        {
            var json = JsonNode.Parse(bytes)!.AsObject();
            var metadata = json["direct_await_readiness"]!.AsObject();
            (nested ? metadata["guards"]![0]!.AsObject() : metadata).Remove(field);
            bool rejected = false;
            try { GuestIrSerializer.Deserialize(Encoding.UTF8.GetBytes(json.ToJsonString())); }
            catch (InvalidDataException) { rejected = true; }
            check(rejected, "Missing required readiness field: " + field);
        }
    }
}
