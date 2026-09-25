using System;
using System.Globalization;
using System.IO;
using System.Linq;
using AvidScript.CSharpGuest;
using AvidScript.CSharpSemantic;
using AvidScript.GuestIr;
using AvidScript.WasmBackend;

internal static class CSharpGuestAsyncInvocationTests
{
    public static int Run()
    {
        int count = 0;
        foreach ((string name, string begin, string methods) in new[] {
            ("basic", "new Worker(1).Run(3);", """
                public async void Run(int amount) {
                    Value += amount; amount += 2;
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Value += amount; amount += 4;
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Value += amount; Script.Result = Value;
                }
                """),
            ("concurrent", "new Worker(10).Run(1, 1); new Worker(100).Run(2, 100);", """
                public async void Run(int amount, int weight) {
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Value += amount;
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Value += amount * 2; Script.Result += Value * weight;
                }
                """),
            ("repeated", "Worker worker = new Worker(10); worker.Run(1, 1); worker.Run(2, 100);", """
                public async void Run(int amount, int weight) {
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Value += amount;
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Value += amount * 2; Script.Result += Value * weight;
                }
                """),
            ("capture", "new Worker(2).Run(1);", """
                public async void Run(int amount) {
                    Func<int> read = () => Value + ++amount;
                    amount += 3;
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Value += read();
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Script.Result = read() + Value;
                }
                """),
            ("replace", "new Worker(7).Run(new Worker(2));", """
                private static void Replace(ref Worker peer) { peer = new Worker(20); }
                public async void Run(Worker peer) {
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Replace(ref peer);
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Script.Result = Value + peer.Value;
                }
                """),
            ("aggregate", "Worker peer = new Worker(5); Packet packet = default; packet.Peer = peer; packet.Marker = 3; new Worker(7).Run(packet, 2); packet.Marker = 99; peer.Value = 8;", """
                public async void Run(Packet packet, int amount) {
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Value += packet.Peer.Value + packet.Marker + amount;
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Script.Result = Value;
                }
                """),
            ("delegate", "Worker worker = new Worker(1); Action<int> run = worker.Run; run(4);", """
                public async void Run(int amount) {
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Value += amount;
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Script.Result = Value + amount;
                }
                """),
            ("nested", "new Worker(1).Run(4);", """
                public async void Run(int amount) {
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Helper(this, amount);
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    Script.Result += Value;
                }
                private static async void Helper(Worker worker, int amount) {
                    await AvidContinuations.NextTickAsync().WithCancellation(Script.Token);
                    worker.Value += amount; Script.Result += amount * 100;
                }
                """),
        })
        {
            string source = Source.Replace("// BEGIN", begin, StringComparison.Ordinal).Replace("// METHODS", methods, StringComparison.Ordinal);
            SemanticDocument document = CSharpGuestContinuationTests.Analyze(source, "Scripts/AsyncInvocation_" + name + ".cs");
            CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('d', 64));
            Check(lowered.Succeeded, name + ": " + string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
            GuestModule module = lowered.Module!;
            WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
            Check(compiled.Succeeded, string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
            Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes), "invocation round trip");
            Check(module.Functions.SelectMany(fn => fn.Blocks).SelectMany(block => block.Instructions).Any(op => op.Op == GuestContinuationState.StoreOp), "invocation uses rooted state");
            string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
            if (!string.IsNullOrWhiteSpace(output))
            {
                Directory.CreateDirectory(output);
                string stem = Path.Combine(output, "csharp-managed-async-invoke-" + name);
                GuestStateSlot result = module.MemoryLayout.StateSlots.Single(slot => slot.GlobalId.Contains(".Result:", StringComparison.Ordinal));
                File.WriteAllBytes(stem + ".wasm", compiled.Bytes);
                File.WriteAllBytes(stem + ".guest-ir.json", GuestIrSerializer.Serialize(module));
                File.WriteAllText(stem + ".result-offset", result.Offset.ToString(CultureInfo.InvariantCulture));
            }
            count++;
        }
        return count + TaskResultSemanticCompilesToWasm()
            + TaskLocalSemanticCompilesToWasm() + TaskConditionalLocalsCompileToWasm()
            + TaskParallelLocalsCompileToWasm()
            + TaskFieldAssignmentCompilesToWasm() + TaskIntegratedCompilesToWasm();
    }

    private static int TaskFieldAssignmentCompilesToWasm()
    {
        const string source = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    Result = await LoadScoreAsync();
                }
            }
            """;
        SemanticDocument document = CSharpGuestContinuationTests.Analyze(
            source, "Scripts/TaskFieldAssignment.cs");
        Check(document.Succeeded
            && document.SchemaVersion == SemanticContract.TaskAssignmentSchemaVersion,
            "the prior direct field assignment contract must remain available");
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('d', 64));
        Check(lowered.Succeeded && lowered.Module is not null
            && WasmModuleCompiler.Compile(lowered.Module).Succeeded,
            "Semantic 37 field assignment must still compile to WASM");
        return 1;
    }

    private static int TaskIntegratedCompilesToWasm()
    {
        DirectoryInfo? directory = new(Environment.CurrentDirectory);
        string? fixture = null;
        while (directory is not null)
        {
            string candidate = Path.Combine(directory.FullName,
                "Fixtures", "Phase66", "TaskIntIntegrated.cs");
            if (File.Exists(candidate))
            {
                fixture = candidate;
                break;
            }
            directory = directory.Parent;
        }
        Check(fixture is not null, "Task<int> integration source is missing");
        SemanticDocument document = CSharpGuestContinuationTests.Analyze(
            File.ReadAllText(fixture!), "Scripts/TaskIntIntegrated.cs");
        Check(document.Succeeded && document.SchemaVersion == SemanticContract.TaskAliasSchemaVersion
            && document.SemanticVersion == SemanticContract.TaskAliasSemanticVersion,
            "integrated Task alias requires Semantic 39: "
                + string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        Check(document.AsyncMethods.Single(method => method.TaskResultTypeId is not null
                && method.MethodSymbolId.Contains("RunScenarioAsync", StringComparison.Ordinal))
                .TaskLocalSymbolIds is { Count: 3 },
            "integrated Task alias must keep its unawaited source as a separate owner");
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('d', 64));
        Check(lowered.Succeeded,
            "integrated Task<int> lowering failed: "
                + string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        GuestModule module = lowered.Module!;
        Check(module.SchemaVersion == 19 && module.IrVersion == "1.18",
            "integrated Task<int> retains the task-local Guest IR version");
        SemanticCallable scenario = document.Callables.Single(callable => callable.MethodSymbolId
            .Contains("RunScenarioAsync", StringComparison.Ordinal));
        SemanticSymbol left = document.Symbols.Single(symbol => symbol.Kind == "local"
            && symbol.Name == "left" && symbol.ContainingSymbolId == scenario.MethodSymbolId);
        SemanticSymbol alias = document.Symbols.Single(symbol => symbol.Kind == "local"
            && symbol.Name == "leftAlias" && symbol.ContainingSymbolId == scenario.MethodSymbolId);
        GuestFunction scenarioFunction = module.Functions.Single(function =>
            function.Id == "function:" + scenario.MethodSymbolId);
        GuestBasicBlock aliasBlock = scenarioFunction.Blocks.Single(block => block.Instructions
            .Any(instruction => instruction.Op == "local_store"
                && instruction.TargetId?.Contains(alias.Id, StringComparison.Ordinal) == true));
        GuestInstruction[] aliasInstructions = aliasBlock.Instructions.ToArray();
        int aliasStoreIndex = Array.FindIndex(aliasInstructions, instruction =>
            instruction.Op == "local_store"
                && instruction.TargetId?.Contains(alias.Id, StringComparison.Ordinal) == true);
        int retainIndex = Array.FindIndex(aliasInstructions, instruction =>
            instruction.Op == "call" && instruction.TargetId == "import:$async:task_i32_v1");
        Check(retainIndex >= 0 && retainIndex < aliasStoreIndex
            && aliasInstructions.Any(instruction => instruction.Op == "local_load"
                && instruction.TargetId?.Contains(left.Id, StringComparison.Ordinal) == true
                && instruction.ResultId == aliasInstructions[aliasStoreIndex].OperandIds.Single()),
            "Task alias retains the source token but stores the token, not the Host acceptance flag");
        string resultGlobalId = module.MemoryLayout.StateSlots.Single(slot =>
            slot.GlobalId.Contains(".Result:", StringComparison.Ordinal)).GlobalId;
        Check(module.Functions.SelectMany(function => function.Blocks)
            .SelectMany(block => block.Instructions)
            .Any(instruction => instruction.Op == "global_store"
                && instruction.TargetId == resultGlobalId),
            "await result must write the script field in Guest IR");
        Check(!CSharpGuestLowerer.Lower(document with
        {
            SchemaVersion = SemanticContract.TaskAssignmentSchemaVersion,
            SemanticVersion = SemanticContract.TaskAssignmentSemanticVersion,
        }, new string('d', 64)).Succeeded,
            "Semantic 37 must not accept Task aliases or existing-local assignment");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Check(compiled.Succeeded,
            "integrated Task<int> WASM failed: "
                + string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(
                GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes),
            "integrated Task<int> WASM is deterministic after IR round-trip");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            string stem = Path.Combine(output, "csharp-task-int-integrated");
            GuestStateSlot result = module.MemoryLayout.StateSlots.Single(slot =>
                slot.GlobalId.Contains(".Result:", StringComparison.Ordinal));
            File.WriteAllBytes(stem + ".wasm", compiled.Bytes);
            File.WriteAllBytes(stem + ".guest-ir.json", GuestIrSerializer.Serialize(module));
            File.WriteAllText(stem + ".result-offset",
                result.Offset.ToString(CultureInfo.InvariantCulture));
            GuestStateSlot cleanups = module.MemoryLayout.StateSlots.Single(slot =>
                slot.GlobalId.Contains(".Cleanups:", StringComparison.Ordinal));
            File.WriteAllText(stem + ".cleanup-offset",
                cleanups.Offset.ToString(CultureInfo.InvariantCulture));
        }
        return 1;
    }

    private static int TaskParallelLocalsCompileToWasm()
    {
        const string source = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> LoadScoreAsync(int score)
                {
                    await AvidContinuations.NextTickAsync();
                    await AvidContinuations.NextTickAsync();
                    return score;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    Task<int> left = LoadScoreAsync(7);
                    Task<int> right = LoadScoreAsync(5);
                    await AvidContinuations.NextTickAsync();
                    int first = await left;
                    int second = await right;
                    Result = first * 10 + second;
                }
            }
            """;
        SemanticDocument document = CSharpGuestContinuationTests.Analyze(
            source, "Scripts/TaskIntParallelLocals.cs");
        Check(document.Succeeded && document.SchemaVersion == SemanticContract.TaskLocalSchemaVersion,
            "parallel Task locals require Semantic 36: "
                + string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('d', 64));
        Check(lowered.Succeeded,
            "parallel Task local lowering failed: "
                + string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        GuestModule module = lowered.Module!;
        Check(module.SchemaVersion == 19 && module.IrVersion == "1.18"
            && module.Imports.Count(imported => imported.Name ==
                "avid_task_retain_for_continuation_v1") == 1,
            "parallel Task locals retain the versioned Guest IR ownership contract");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Check(compiled.Succeeded,
            "parallel Task local WASM failed: "
                + string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(
                GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes),
            "parallel Task local WASM must be deterministic after IR round-trip");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            string stem = Path.Combine(output, "csharp-task-int-parallel");
            GuestStateSlot result = module.MemoryLayout.StateSlots.Single(slot =>
                slot.GlobalId.Contains(".Result:", StringComparison.Ordinal));
            File.WriteAllBytes(stem + ".wasm", compiled.Bytes);
            File.WriteAllBytes(stem + ".guest-ir.json", GuestIrSerializer.Serialize(module));
            File.WriteAllText(stem + ".result-offset",
                result.Offset.ToString(CultureInfo.InvariantCulture));
        }
        string sixDeclarations = string.Join(" ", Enumerable.Range(0, 6)
            .Select(index => $"Task<int> extra{index} = LoadScoreAsync({index});"));
        string sixAwaits = string.Join(" ", Enumerable.Range(0, 6)
            .Select(index => $"await extra{index};"));
        SemanticDocument atBudget = CSharpGuestContinuationTests.Analyze(source.Replace(
                "Task<int> right = LoadScoreAsync(5);",
                "Task<int> right = LoadScoreAsync(5); " + sixDeclarations,
                StringComparison.Ordinal).Replace("int second = await right;",
                "int second = await right; " + sixAwaits, StringComparison.Ordinal),
            "Scripts/TaskIntEightLocals.cs");
        CSharpGuestLoweringResult eight = CSharpGuestLowerer.Lower(atBudget, new string('d', 64));
        Check(eight.Succeeded && WasmModuleCompiler.Compile(eight.Module!).Succeeded,
            "eight parallel Task locals must fit the compiled continuation frame");
        return 1;
    }

    private static int TaskLocalSemanticCompilesToWasm()
    {
        const string source = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    int adjustment = 1;
                    await AvidContinuations.NextTickAsync();
                    Task<int> pending = LoadScoreAsync();
                    adjustment = adjustment + 1;
                    await AvidContinuations.NextTickAsync();
                    int first = await pending;
                    int second = await pending;
                    Result = first + second + adjustment - 2;
                }
            }
            """;
        SemanticDocument document = CSharpGuestContinuationTests.Analyze(
            source, "Scripts/TaskIntLocal.cs");
        Check(document.Succeeded
            && document.SchemaVersion == SemanticContract.TaskLocalSchemaVersion,
            "Task local source must select Semantic 36: "
                + string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('d', 64));
        Check(lowered.Succeeded,
            "Task local Guest lowering failed: "
                + string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        GuestModule module = lowered.Module!;
        Check(module.SchemaVersion == 19 && module.IrVersion == "1.18"
            && module.Imports.Count(imported => imported.Module == "avidscript"
                && imported.Name == "avid_task_retain_for_continuation_v1") == 1
            && module.Functions.SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions)
                .Any(instruction => instruction.TargetId == "import:$async:task_retain_for_continuation_v1"),
            "Task local WASM must transfer ownership to the continuation");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Check(compiled.Succeeded,
            "Task local WASM compilation failed: "
                + string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(
                GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes)
            && !GuestModuleValidator.Validate(module with
                { SchemaVersion = 18, IrVersion = "1.17" }).Succeeded,
            "Task local IR round-trips but cannot be relabeled as the older Task contract");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            string stem = Path.Combine(output, "csharp-task-int-local");
            GuestStateSlot result = module.MemoryLayout.StateSlots.Single(slot =>
                slot.GlobalId.Contains(".Result:", StringComparison.Ordinal));
            File.WriteAllBytes(stem + ".wasm", compiled.Bytes);
            File.WriteAllBytes(stem + ".guest-ir.json", GuestIrSerializer.Serialize(module));
            File.WriteAllText(stem + ".result-offset", result.Offset.ToString(CultureInfo.InvariantCulture));
        }
        const string earlyReturnSource = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> LoadScoreAsync()
                {
                    return 12;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    Task<int> pending = LoadScoreAsync();
                    if (Result == 0) return;
                    int score = await pending;
                    Result = score;
                }
            }
            """;
        SemanticDocument earlyDocument = CSharpGuestContinuationTests.Analyze(
            earlyReturnSource, "Scripts/TaskIntEarlyReturn.cs");
        Check(earlyDocument.Succeeded
            && SemanticAsyncInvocationValidator.IsValid(earlyDocument),
            "early return after task creation must preserve owner provenance");
        CSharpGuestLoweringResult earlyLowered = CSharpGuestLowerer.Lower(
            earlyDocument, new string('d', 64));
        Check(earlyLowered.Succeeded,
            "early return Guest lowering failed: "
                + string.Join(" | ", earlyLowered.Diagnostics.Select(item => item.Message)));
        WasmCompilationResult earlyCompiled = WasmModuleCompiler.Compile(earlyLowered.Module!);
        Check(earlyCompiled.Succeeded, "early return must compile to executable WASM");
        if (!string.IsNullOrWhiteSpace(output))
        {
            string stem = Path.Combine(output, "csharp-task-int-early-return");
            GuestStateSlot result = earlyLowered.Module!.MemoryLayout.StateSlots.Single(slot =>
                slot.GlobalId.Contains(".Result:", StringComparison.Ordinal));
            File.WriteAllBytes(stem + ".wasm", earlyCompiled.Bytes);
            File.WriteAllText(stem + ".result-offset", result.Offset.ToString(CultureInfo.InvariantCulture));
        }
        return 2;
    }

    private static int TaskConditionalLocalsCompileToWasm()
    {
        const string source = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static int Flag;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    if (Flag == 0)
                    {
                        await AvidContinuations.NextTickAsync();
                        Task<int> pending = LoadScoreAsync();
                        Result = await pending;
                        return;
                    }
                    Result = 7;
                }
            }
            """;
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        foreach ((string scenario, string script) in new[]
        {
            ("conditional-local", source),
            ("conditional-skip", source.Replace("if (Flag == 0)",
                "Flag = 1; if (Flag == 0)", StringComparison.Ordinal)),
        })
        {
            SemanticDocument document = CSharpGuestContinuationTests.Analyze(
                script, "Scripts/TaskInt" + scenario + ".cs");
            Check(document.Succeeded && SemanticAsyncInvocationValidator.IsValid(document),
                scenario + " must publish a valid nested Task owner: "
                    + string.Join(" | ", document.Diagnostics.Select(item => item.Message)));
            CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(
                document, new string('d', 64));
            Check(lowered.Succeeded,
                scenario + " Guest lowering failed: "
                    + string.Join(" | ", lowered.Diagnostics.Select(item => item.Message)));
            GuestModule module = lowered.Module!;
            WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
            Check(compiled.Succeeded,
                scenario + " WASM compilation failed: "
                    + string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
            Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(
                    GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes),
                scenario + " WASM must be deterministic after IR round-trip");
            if (!string.IsNullOrWhiteSpace(output))
            {
                Directory.CreateDirectory(output);
                string stem = Path.Combine(output, "csharp-task-int-" + scenario);
                GuestStateSlot result = module.MemoryLayout.StateSlots.Single(slot =>
                    slot.GlobalId.Contains(".Result:", StringComparison.Ordinal));
                File.WriteAllBytes(stem + ".wasm", compiled.Bytes);
                File.WriteAllText(stem + ".result-offset",
                    result.Offset.ToString(CultureInfo.InvariantCulture));
            }
        }
        return 2;
    }

    private static int TaskResultSemanticCompilesToWasm()
    {
        int count = 0;
        foreach ((string scenario, string producerBody) in new[]
        {
            ("deferred", "await AvidContinuations.NextTickAsync();"),
            ("immediate", ""),
        })
        {
            string source = $$"""
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> LoadScoreAsync()
                {
                    {{producerBody}}
                    return 12;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    int score = await LoadScoreAsync();
                    Result = score;
                }
            }
            """;
            CompileTaskFixture(scenario, source);
            count++;
        }
        const string chainSource = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> ReadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    return 12;
                }
                public static async Task<int> LoadScoreAsync()
                {
                    int score = await ReadScoreAsync();
                    return score + 1;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    int score = await LoadScoreAsync();
                    Result = score;
                }
            }
            """;
        CompileTaskFixture("chain", chainSource);
        const string argumentsSource = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                public static async Task<int> LoadScoreAsync(int startingScore, int bonus)
                {
                    await AvidContinuations.NextTickAsync();
                    return startingScore * 10 + bonus;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    int score = await LoadScoreAsync(7, 5);
                    Result = score;
                }
            }
            """;
        CompileTaskFixture("arguments", argumentsSource);
        const string combinedSource = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                private static T Identity<T>(T value) => value;
                public static async Task<int> LoadScoreAsync()
                {
                    int total = 0;
                    foreach (int value in new[] { 3, 5, 8 })
                    {
                        total += Identity<int>(value);
                    }
                    await AvidContinuations.NextTickAsync();
                    return total;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    int score = await LoadScoreAsync();
                    Result = score;
                }
            }
            """;
        CompileTaskFixture("combined", combinedSource);
        const string cleanupSource = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                private static int Cleanups;
                private static T Identity<T>(T value) => value;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync();
                    int total = 0;
                    try
                    {
                        foreach (int value in new[] { 3, 5, 8 })
                        {
                            total += Identity<int>(value);
                        }
                        return total;
                    }
                    finally
                    {
                        Cleanups++;
                    }
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    int score = await LoadScoreAsync();
                    Result = score * 10 + Cleanups;
                }
            }
            """;
        CompileTaskFixture("cleanup", cleanupSource);
        const string cancellationSource = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                [AvidTransient] private static AvidCancellationSource Cancellation;
                [AvidTransient] private static AvidCancellationToken Token;
                public static async Task<int> LoadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync().WithCancellation(Token);
                    return 99;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    Cancellation = AvidCancellationSource.Create();
                    Token = Cancellation.Token;
                    int score = await LoadScoreAsync();
                    Result = score;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
                public static void Tick(float deltaSeconds)
                {
                    Cancellation.Cancel();
                    Cancellation.Release();
                }
            }
            """;
        CompileTaskFixture("cancelled", cancellationSource);
        const string cancellationChainSource = """
            using AvidScript;
            using System.Runtime.InteropServices;
            using System.Threading.Tasks;
            public static class Script
            {
                public static int Result;
                [AvidTransient] private static AvidCancellationSource Cancellation;
                [AvidTransient] private static AvidCancellationToken Token;
                public static async Task<int> ReadScoreAsync()
                {
                    await AvidContinuations.NextTickAsync().WithCancellation(Token);
                    return 12;
                }
                public static async Task<int> LoadScoreAsync()
                {
                    int score = await ReadScoreAsync();
                    return score + 1;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
                public static async void BeginPlay()
                {
                    Cancellation = AvidCancellationSource.Create();
                    Token = Cancellation.Token;
                    int score = await LoadScoreAsync();
                    Result = score;
                }
                [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
                public static void Tick(float deltaSeconds)
                {
                    Cancellation.Cancel();
                    Cancellation.Release();
                }
            }
            """;
        CompileTaskFixture("cancelled-chain", cancellationChainSource);
        return count + 6;
    }

    private static void CompileTaskFixture(string scenario, string source)
    {
        SemanticDocument document = CSharpGuestContinuationTests.Analyze(
            source, "Scripts/TaskIntAbiBoundary_" + scenario + ".cs");
        if (scenario is "combined" or "cleanup")
            document = SemanticSerializer.Deserialize(SemanticSerializer.Serialize(document));
        Check(document.Succeeded
            && document.SchemaVersion == SemanticContract.TaskResultSchemaVersion,
            scenario + ": task result source must reach the new semantic contract");
        if (scenario == "combined")
        {
            SemanticCallable instance = document.Callables.Single(callable =>
                callable.GenericDefinitionSymbolId is not null);
            Check(document.Reachability!.ReachableCallableIds.Contains(instance.MethodSymbolId)
                && document.AsyncMethods.Any(method => method.Segments
                    .SelectMany(segment => segment.Statements)
                    .Any(statement => ContainsCall(statement.Operation, instance.MethodSymbolId))),
                "combined: the async segment must call a reachable closed generic instance");
            SemanticDocument missingInstance = document with
            {
                Reachability = document.Reachability with
                {
                    ReachableCallableIds = document.Reachability.ReachableCallableIds
                        .Where(id => id != instance.MethodSymbolId).ToArray(),
                },
            };
            Check(!CSharpGuestLowerer.Lower(missingInstance, new string('d', 64)).Succeeded,
                "combined: dropping the closed async call target must fail validation");
            string producerId = document.AsyncMethods.Single(method =>
                method.TaskResultTypeId == "type:int32").MethodSymbolId;
            Check(document.Reachability.ReachableCallableIds.Contains(producerId),
                "combined: direct Task<int> producer must be reachable from the entrypoint");
            string totalId = document.Symbols.Single(symbol => symbol.Name == "total"
                && symbol.ContainingSymbolId == producerId).Id;
            SemanticAsyncMethod producer = document.AsyncMethods.Single(method =>
                method.MethodSymbolId == producerId);
            Check(producer.Segments.Single(segment => segment.AwaitSite is not null)
                    .AwaitSite!.StateFrame?.Slots.Any(slot => slot.SymbolId == totalId) == true,
                "combined: the returned local must survive the suspension");
            SemanticDocument missingProducer = document with
            {
                Reachability = document.Reachability with
                {
                    ReachableCallableIds = document.Reachability.ReachableCallableIds
                        .Where(id => id != producerId).ToArray(),
                },
            };
            Check(!CSharpGuestLowerer.Lower(missingProducer, new string('d', 64)).Succeeded,
                "combined: dropping the awaited producer must fail validation");
        }
        if (scenario == "cleanup")
        {
            SemanticAsyncMethod producer = document.AsyncMethods.Single(method =>
                method.TaskResultTypeId == "type:int32");
            SemanticAsyncCompilerLocal returnLocal = producer.CompilerLocals.Single(local =>
                local.SymbolId.EndsWith(":finally_return", StringComparison.Ordinal));
            Check(returnLocal.Name == "<finally_return>"
                && returnLocal.TypeId == "type:int32",
                "cleanup: return value must use the validated finally storage");
            SemanticDocument forgedLocal = document with
            {
                AsyncMethods = document.AsyncMethods.Select(method =>
                    method.MethodSymbolId == producer.MethodSymbolId
                        ? method with
                        {
                            CompilerLocals = method.CompilerLocals.Select(local =>
                                local.SymbolId == returnLocal.SymbolId
                                    ? local with { Name = "<other>" } : local).ToArray(),
                        }
                        : method).ToArray(),
            };
            Check(!CSharpGuestLowerer.Lower(forgedLocal, new string('d', 64)).Succeeded,
                "cleanup: a forged finally return local must fail validation");
        }
        CSharpGuestLoweringResult lowered = CSharpGuestLowerer.Lower(document, new string('d', 64));
        Check(lowered.Succeeded,
            scenario + ": Task<int> Guest lowering failed: " + string.Join(" | ", lowered.Diagnostics.Select(item => item.Code + ":" + item.Message)));
        GuestModule module = lowered.Module!;
        Check(module.SchemaVersion == 18 && module.IrVersion == "1.17"
            && module.Imports.Count(imported => imported.Module == "avidscript"
                && imported.Name == "avid_task_i32_v1") == 1
            && module.Imports.Count(imported => imported.Module == "avidscript"
                && imported.Name == "avid_task_bind_producer_v1") == 1
            && module.Imports.Count(imported => imported.Module == "avidscript"
                && imported.Name == "avid_task_propagate_failure_v1") == 1
            && (scenario == "immediate"
                || module.Functions.SelectMany(function => function.Blocks)
                    .SelectMany(block => block.Instructions)
                    .Any(instruction => instruction.Op == "call"
                        && instruction.TargetId == "import:$async:task_bind_producer_v1")),
            scenario + ": Task<int> lowering must bind each pending producer to its continuation");
        Check(scenario != "cancelled-chain"
            || module.Functions.SelectMany(function => function.Blocks)
                .SelectMany(block => block.Instructions)
                .Any(instruction => instruction.Op == "call"
                    && instruction.TargetId == "import:$async:task_propagate_failure_v1"),
            scenario + ": nested Task<int> await must preserve child failure state");
        WasmCompilationResult compiled = WasmModuleCompiler.Compile(module);
        Check(compiled.Succeeded,
            scenario + ": Task<int> WASM compilation failed: " + string.Join(" | ", compiled.Diagnostics.Select(item => item.Message)));
        if (scenario == "combined")
            Check(compiled.Bytes.SequenceEqual(WasmModuleCompiler.Compile(
                    GuestIrSerializer.Deserialize(GuestIrSerializer.Serialize(module))).Bytes),
                "combined: serialized Guest IR must preserve the executable module");
        Check(WasmArtifactInspector.Inspect(compiled.Bytes).Imports.Any(imported =>
            imported.Module == "avidscript" && imported.Name == "avid_task_i32_v1"),
            scenario + ": Task<int> WASM must retain the exact Host import");
        Check(WasmArtifactInspector.Inspect(compiled.Bytes).Imports.Any(imported =>
            imported.Module == "avidscript" && imported.Name == "avid_task_bind_producer_v1"),
            scenario + ": Task<int> WASM must retain the producer binding import");
        Check(WasmArtifactInspector.Inspect(compiled.Bytes).Imports.Any(imported =>
            imported.Module == "avidscript" && imported.Name == "avid_task_propagate_failure_v1"),
            scenario + ": Task<int> WASM must retain the failure propagation import");
        string? output = Environment.GetEnvironmentVariable("AVIDSCRIPT_MANAGED_HEAP_WASM_DIR");
        if (!string.IsNullOrWhiteSpace(output))
        {
            Directory.CreateDirectory(output);
            string stem = Path.Combine(output, "csharp-task-int-" + scenario);
            GuestStateSlot result = module.MemoryLayout.StateSlots.Single(slot =>
                slot.GlobalId.Contains(".Result:", StringComparison.Ordinal));
            File.WriteAllBytes(stem + ".wasm", compiled.Bytes);
            File.WriteAllBytes(stem + ".guest-ir.json", GuestIrSerializer.Serialize(module));
            File.WriteAllText(stem + ".result-offset", result.Offset.ToString(CultureInfo.InvariantCulture));
        }
    }

    private static bool ContainsCall(SemanticOperation operation, string target) =>
        operation.SymbolId == target || operation.Children.Any(child => ContainsCall(child, target));

    private const string Source = """
        using System; using System.Runtime.InteropServices; using AvidScript;
        public struct Packet { public Worker Peer; public int Marker; }
        public class Worker {
            public int Value;
            public Worker(int value) { Value = value; }
            // METHODS
        }
        public static class Script {
            public static int Result;
            [AvidTransient] private static AvidCancellationSource Cancellation;
            [AvidTransient] public static AvidCancellationToken Token;
            [UnmanagedCallersOnly(EntryPoint = "avid_on_begin_play")]
            public static void BeginPlay() {
                Cancellation = AvidCancellationSource.Create();
                Token = Cancellation.Token;
                // BEGIN
            }
            [UnmanagedCallersOnly(EntryPoint = "avid_on_tick")]
            public static void Tick(float dt) { }
            [UnmanagedCallersOnly(EntryPoint = "avid_on_end_play")]
            public static void EndPlay() {
                AvidCancellationSource cancellation = Cancellation;
                cancellation.Cancel(); cancellation.Release();
            }
        }
        """;
    private static void Check(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }
}
